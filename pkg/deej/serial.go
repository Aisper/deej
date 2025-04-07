package deej

import (
	"bufio"
	"errors"
	"fmt"
	"io"
	"regexp"
	"strconv"
	"strings"
	"time"

	"github.com/jacobsa/go-serial/serial"
	"go.uber.org/zap"

	"github.com/omriharel/deej/pkg/deej/util"
)

// SerialIO provides a deej-aware abstraction layer to managing serial I/O
type SerialIO struct {
	comPort  string
	baudRate uint

	deej   *Deej
	logger *zap.SugaredLogger

	stopChannel chan bool
	connected   bool
	connOptions serial.OpenOptions
	conn        io.ReadWriteCloser

	lastKnownNumSliders        int
	currentSliderPercentValues []float32

	sliderMoveConsumers []chan SliderMoveEvent
}

// SliderMoveEvent represents a single slider move captured by deej
type SliderMoveEvent struct {
	SliderID     int
	PercentValue float32
}

var expectedLinePattern = regexp.MustCompile(`^\d{1,4}(\|\d{1,4})*\r\n$`)

// NewSerialIO creates a SerialIO instance that uses the provided deej
// instance's connection info to establish communications with the arduino chip
func NewSerialIO(deej *Deej, logger *zap.SugaredLogger) (*SerialIO, error) {
	logger = logger.Named("serial")

	sio := &SerialIO{
		deej:                deej,
		logger:              logger,
		stopChannel:         make(chan bool),
		connected:           false,
		conn:                nil,
		sliderMoveConsumers: []chan SliderMoveEvent{},
	}

	logger.Debug("Created serial i/o instance")

	// respond to config changes
	sio.setupOnConfigReload()

	return sio, nil
}

// Start attempts to connect to our arduino chip
func (sio *SerialIO) Start() error {
	// don't allow multiple concurrent connections
	if sio.connected {
		sio.logger.Warn("Already connected, can't start another without closing first")
		return errors.New("serial: connection already active")
	}

	// set minimum read size according to platform (0 for windows, 1 for linux)
	// this prevents a rare bug on windows where serial reads get congested,
	// resulting in significant lag
	minimumReadSize := 0
	if util.Linux() {
		minimumReadSize = 1
	}

	sio.connOptions = serial.OpenOptions{
		PortName:        sio.deej.config.ConnectionInfo.COMPort,
		BaudRate:        uint(sio.deej.config.ConnectionInfo.BaudRate),
		DataBits:        8,
		StopBits:        1,
		MinimumReadSize: uint(minimumReadSize),
	}

	sio.logger.Debugw("Attempting serial connection",
		"comPort", sio.connOptions.PortName,
		"baudRate", sio.connOptions.BaudRate,
		"minReadSize", minimumReadSize)

	var err error
	sio.conn, err = serial.Open(sio.connOptions)
	if err != nil {

		// might need a user notification here, TBD
		sio.logger.Warnw("Failed to open serial connection", "error", err)
		return fmt.Errorf("open serial connection: %w", err)
	}

	namedLogger := sio.logger.Named(strings.ToLower(sio.connOptions.PortName))

	namedLogger.Infow("Connected", "conn", sio.conn)
	sio.connected = true

	if err := sio.performInitializationSequence(); err != nil {
		sio.close(namedLogger) // Clean up if initialization fails
		return fmt.Errorf("initialization failed: %w", err)
	}

	// read lines or await a stop
	go func() {
		connReader := bufio.NewReader(sio.conn)
		lineChannel := sio.readLine(namedLogger, connReader)

		for {
			select {
			case <-sio.stopChannel:
				sio.close(namedLogger)
			case line := <-lineChannel:
				sio.handleLine(namedLogger, line)
			}
		}
	}()

	return nil
}

const (
	handshakeTimeout       = 5 * time.Second
	handshakeRetryInterval = 500 * time.Millisecond
	handshakeChar          = '>'
	ackChar                = '<'
)

func (sio *SerialIO) performHandshake() error {
	sio.logger.Info("Starting handshake protocol")

	// Flush any existing data in the buffer
	if flusher, ok := sio.conn.(interface{ Flush() error }); ok {
		flusher.Flush()
	}

	handshakeDone := make(chan bool)
	errChan := make(chan error)

	// Start handshake routine
	go func() {
		buf := make([]byte, 1)
		retryTicker := time.NewTicker(handshakeRetryInterval)
		defer retryTicker.Stop()

		// Send initial handshake signal
		if _, err := sio.conn.Write([]byte{handshakeChar}); err != nil {
			errChan <- fmt.Errorf("failed to send handshake: %w", err)
			return
		}

		for {
			select {
			case <-handshakeDone:
				return
			case <-retryTicker.C:
				// Resend handshake periodically
				if _, err := sio.conn.Write([]byte{handshakeChar}); err != nil {
					errChan <- fmt.Errorf("failed to resend handshake: %w", err)
					return
				}
			default:
				// Check for response
				n, err := sio.conn.Read(buf)
				if err != nil {
					if err != io.EOF {
						errChan <- fmt.Errorf("read error during handshake: %w", err)
						return
					}
					continue
				}

				if n > 0 {
					switch buf[0] {
					case handshakeChar:
						// Arduino is initiating handshake
						sio.logger.Debug("Received handshake initiation from Arduino")
						if _, err := sio.conn.Write([]byte{ackChar}); err != nil {
							errChan <- err
							return
						}
						handshakeDone <- true
						return
					case ackChar:
						// Arduino acknowledged our handshake
						sio.logger.Debug("Received handshake ACK from Arduino")
						handshakeDone <- true
						return
					}
				}
			}
		}
	}()

	// Wait for handshake completion or timeout
	select {
	case <-handshakeDone:
		sio.logger.Info("Handshake completed successfully")
		return nil
	case err := <-errChan:
		return err
	case <-time.After(handshakeTimeout):
		return fmt.Errorf("handshake timed out after %v", handshakeTimeout)
	}
}

func (sio *SerialIO) produceInitData() []byte {
	volumes := make(map[int]int)

	for ind, targets := range sio.deej.config.SliderMapping.m {

		found := false

		// for each possible target for this slider...
		for _, target := range targets {

			// resolve the target name by cleaning it up and applying any special transformations.
			// depending on the transformation applied, this can result in more than one target name
			resolvedTargets := sio.deej.sessions.resolveTarget(target)

			// for each resolved target...
			for _, resolvedTarget := range resolvedTargets {

				// check the map for matching sessions
				sessions, ok := sio.deej.sessions.get(resolvedTarget)

				// no sessions matching this target - move on
				if !ok {
					continue
				}

				volumes[ind] = int(sessions[0].GetVolume())
				found = true

				break
			}

			if found {
				break
			}
		}

		if found {
			continue
		}
	}

	// Send initialization data to the Arduino
	initData := make([]byte, 0, len(volumes)+2)

	initData = append(initData, '>')
	for _, value := range volumes {

		if value < 0 || value > 255 {
			panic("value exceeds 2 bytes")
		}

		initData = append(initData, byte(value))
	}
	initData = append(initData, '<')

	return initData
}

func (sio *SerialIO) performInitializationSequence() error {
	if err := sio.performHandshake(); err != nil {
		return fmt.Errorf("handshake failed: %w", err)
	}

	// Send initialization data
	initData := sio.produceInitData()
	if _, err := sio.conn.Write(initData); err != nil {
		return fmt.Errorf("failed to send initialization data: %w", err)
	}

	// Wait for ACK that data was received
	buf := make([]byte, 1)
	if _, err := sio.conn.Read(buf); err != nil {
		return fmt.Errorf("failed to read ACK: %w", err)
	}

	if buf[0] != ackChar {
		return fmt.Errorf("invalid ACK received: %v", buf[0])
	}

	sio.logger.Info("Initialization sequence completed successfully")
	return nil
}

// Stop signals us to shut down our serial connection, if one is active
func (sio *SerialIO) Stop() {
	if sio.connected {
		sio.logger.Debug("Shutting down serial connection")
		sio.stopChannel <- true
	} else {
		sio.logger.Debug("Not currently connected, nothing to stop")
	}
}

// SubscribeToSliderMoveEvents returns an unbuffered channel that receives
// a sliderMoveEvent struct every time a slider moves
func (sio *SerialIO) SubscribeToSliderMoveEvents() chan SliderMoveEvent {
	ch := make(chan SliderMoveEvent)
	sio.sliderMoveConsumers = append(sio.sliderMoveConsumers, ch)

	return ch
}

func (sio *SerialIO) setupOnConfigReload() {
	configReloadedChannel := sio.deej.config.SubscribeToChanges()

	const stopDelay = 50 * time.Millisecond

	go func() {
		for {
			select {
			case <-configReloadedChannel:

				// make any config reload unset our slider number to ensure process volumes are being re-set
				// (the next read line will emit SliderMoveEvent instances for all sliders)\
				// this needs to happen after a small delay, because the session map will also re-acquire sessions
				// whenever the config file is reloaded, and we don't want it to receive these move events while the map
				// is still cleared. this is kind of ugly, but shouldn't cause any issues
				go func() {
					<-time.After(stopDelay)
					sio.lastKnownNumSliders = 0
				}()

				// if connection params have changed, attempt to stop and start the connection
				if sio.deej.config.ConnectionInfo.COMPort != sio.connOptions.PortName ||
					uint(sio.deej.config.ConnectionInfo.BaudRate) != sio.connOptions.BaudRate {

					sio.logger.Info("Detected change in connection parameters, attempting to renew connection")
					sio.Stop()

					// let the connection close
					<-time.After(stopDelay)

					if err := sio.Start(); err != nil {
						sio.logger.Warnw("Failed to renew connection after parameter change", "error", err)
					} else {
						sio.logger.Debug("Renewed connection successfully")
					}
				}
			}
		}
	}()
}

func (sio *SerialIO) close(logger *zap.SugaredLogger) {
	if err := sio.conn.Close(); err != nil {
		logger.Warnw("Failed to close serial connection", "error", err)
	} else {
		logger.Debug("Serial connection closed")
	}

	sio.conn = nil
	sio.connected = false
}

func (sio *SerialIO) readLine(logger *zap.SugaredLogger, reader *bufio.Reader) chan string {
	ch := make(chan string)

	go func() {
		for {
			line, err := reader.ReadString('\n')
			if err != nil {

				if sio.deej.Verbose() {
					logger.Warnw("Failed to read line from serial", "error", err, "line", line)
				}

				// just ignore the line, the read loop will stop after this
				return
			}

			if sio.deej.Verbose() {
				logger.Debugw("Read new line", "line", line)
			}

			// deliver the line to the channel
			ch <- line
		}
	}()

	return ch
}

func (sio *SerialIO) handleLine(logger *zap.SugaredLogger, line string) {
	// this function receives an unsanitized line which is guaranteed to end with LF,
	// but most lines will end with CRLF. it may also have garbage instead of
	// deej-formatted values, so we must check for that! just ignore bad ones
	if !expectedLinePattern.MatchString(line) {
		return
	}

	// trim the suffix
	line = strings.TrimSuffix(line, "\r\n")

	// split on pipe (|), this gives a slice of numerical strings between "0" and "1023"
	splitLine := strings.Split(line, "|")
	numSliders := len(splitLine)

	// update our slider count, if needed - this will send slider move events for all
	if numSliders != sio.lastKnownNumSliders {
		logger.Infow("Detected sliders", "amount", numSliders)
		sio.lastKnownNumSliders = numSliders
		sio.currentSliderPercentValues = make([]float32, numSliders)

		// reset everything to be an impossible value to force the slider move event later
		for idx := range sio.currentSliderPercentValues {
			sio.currentSliderPercentValues[idx] = -1.0
		}
	}

	// for each slider:
	moveEvents := []SliderMoveEvent{}
	for sliderIdx, stringValue := range splitLine {

		// convert string values to integers ("1023" -> 1023)
		number, _ := strconv.Atoi(stringValue)

		// turns out the first line could come out dirty sometimes (i.e. "4558|925|41|643|220")
		// so let's check the first number for correctness just in case
		if sliderIdx == 0 && number > 1023 {
			sio.logger.Debugw("Got malformed line from serial, ignoring", "line", line)
			return
		}

		// map the value from raw to a "dirty" float between 0 and 1 (e.g. 0.15451...)
		dirtyFloat := float32(number) / 1023.0

		// normalize it to an actual volume scalar between 0.0 and 1.0 with 2 points of precision
		normalizedScalar := util.NormalizeScalar(dirtyFloat)

		// if sliders are inverted, take the complement of 1.0
		if sio.deej.config.InvertSliders {
			normalizedScalar = 1 - normalizedScalar
		}

		// check if it changes the desired state (could just be a jumpy raw slider value)
		if util.SignificantlyDifferent(sio.currentSliderPercentValues[sliderIdx], normalizedScalar, sio.deej.config.NoiseReductionLevel) {

			// if it does, update the saved value and create a move event
			sio.currentSliderPercentValues[sliderIdx] = normalizedScalar

			moveEvents = append(moveEvents, SliderMoveEvent{
				SliderID:     sliderIdx,
				PercentValue: normalizedScalar,
			})

			if sio.deej.Verbose() {
				logger.Debugw("Slider moved", "event", moveEvents[len(moveEvents)-1])
			}
		}
	}

	// deliver move events if there are any, towards all potential consumers
	if len(moveEvents) > 0 {
		for _, consumer := range sio.sliderMoveConsumers {
			for _, moveEvent := range moveEvents {
				consumer <- moveEvent
			}
		}
	}
}
