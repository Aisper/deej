#include <RotaryEncoder.h>
#include <AisperButton.h>

#define NUM_POTS 3
#define NUM_BUTTONS 5
#define NUM_ENCODERS 2

#define LONG_CLICK_TIME 1000
#define MAX_TIME_BETWEEN_CLICKS 300

#define DATA_RESOLUTION 1024

#define ENCODER_RESOLUTION 50
#define ENCODER_MAX ENCODER_RESOLUTION
#define ENCODER_MIN 0

const char HANDSHAKE_CHAR = '>';
const char ACK_CHAR = '<';
const unsigned long HANDSHAKE_TIMEOUT = 5000;

const int potPins[NUM_POTS] = { A1, A2, A3 };
const int buttonPins[NUM_BUTTONS] = { 14, 15, 18, 4, 7 };
const int encoderPins[NUM_ENCODERS * 2] = { 2, 3, 5, 6 };

struct VolData {
  int value;
  bool mute = false;
};

VolData values[NUM_POTS + NUM_ENCODERS];
Button* buttons[NUM_BUTTONS];
RotaryEncoder* encoders[NUM_ENCODERS];

bool initialized_soft;
bool initialized_hard;

bool receiveData(uint8_t* outArray, uint8_t size, char startMarker = '>', char endMarker = '<') {
  uint8_t index = 0;
  bool receiving = false;

  while (Serial.available() > 0) {
    uint8_t byte = Serial.read();

    if (byte == startMarker) {
      receiving = true;
      continue;
    }

    if (!receiving) {
      continue;
    }

    if (byte == endMarker) {
      break;
    }

    outArray[index] = byte;
    index++;

    if (index >= size) {
      index = size - 1;
    }
  }

  return receiving;
}

bool performHandshake() {
  unsigned long startTime = millis();
  unsigned long lastHandshakeTime = 0;
  const unsigned long handshakeInterval = 300;  // Send every 300ms

  while (millis() - startTime < HANDSHAKE_TIMEOUT) {
    // Check for incoming data
    if (Serial.available() > 0) {
      char received = Serial.read();
      if (received == HANDSHAKE_CHAR) {
        Serial.write(ACK_CHAR);
        return true;
      } else if (received == ACK_CHAR) {
        return true;
      }
    }

    // Send handshake periodically
    if (millis() - lastHandshakeTime >= handshakeInterval) {
      Serial.write(HANDSHAKE_CHAR);
      lastHandshakeTime = millis();
    }
  }

  return false;
}

void setup() {
  Serial.begin(9600);
}

void waitInit() {
  while (!performHandshake()) {
    delay(300);
    return;
  }

  uint8_t dataSize = NUM_POTS + NUM_ENCODERS;
  uint8_t initBytes[dataSize];

  // Handshake successful, wait for initialization data
  if (receiveData(initBytes, dataSize)) {
    for (int i = 0; i < dataSize; i++) {
      int mappedValue = map(initBytes[i], 0, 255, 0, 1023);
      setValue(i, mappedValue);
    }
  }

  Init();

  // Send ACK that we received the data
  Serial.write(ACK_CHAR);
}

void Init() {
  if (!initialized_hard) {
    for (int i = 0; i < NUM_POTS; i++) {
      pinMode(potPins[i], INPUT);
    }

    for (int i = 0; i < NUM_BUTTONS; i++) {
      buttons[i] = new Button(buttonPins[i], MAX_TIME_BETWEEN_CLICKS, LONG_CLICK_TIME, true);
    }

    for (int i = 0; i < NUM_ENCODERS; i++) {
      encoders[i] = new RotaryEncoder(encoderPins[i], encoderPins[i + 1], RotaryEncoder::LatchMode::TWO03);
      int mappedValue = map(getValue(NUM_POTS + i), 0, DATA_RESOLUTION, ENCODER_MIN, ENCODER_RESOLUTION);

      encoders[i]->setPosition(mappedValue);
    }
    initialized_hard = true;
  }

  initialized_soft = true;
}

void loop() {
  if (Serial.available() > 1) {
    if (Serial.read() == HANDSHAKE_CHAR) {
      initialized_soft = false;
    }
  }

  if (!initialized_soft) {
    waitInit();
    return;
  }

  for (int i = 0; i < NUM_ENCODERS; i++) {
    tickEncoder(i);
  }

  for (int i = 0; i < NUM_BUTTONS; i++) {
    tickButton(i);
  }

  tickPots();

  //sendValues();

  printValues();
}

void setValue(uint8_t index, int newValue) {
  int clamped = constrain(newValue, 0, DATA_RESOLUTION - 1);
  values[index].value = clamped;
}

int getValue(uint8_t index) {
  return values[index].mute ? 0 : values[index].value;
}

void tickEncoder(uint8_t index) {
  RotaryEncoder* encoder = encoders[index];

  encoder->tick();

  int rawValue = encoder->getPosition();

  int clampedValue = constrain(rawValue, ENCODER_MIN, ENCODER_MAX);
  int mappedValue = map(clampedValue, ENCODER_MIN, ENCODER_MAX, 0, DATA_RESOLUTION - 1);

  if (rawValue != clampedValue) {
    encoder->setPosition(clampedValue);
  }

  int valueIndex = NUM_POTS + index;

  if (abs(abs(mappedValue) - abs(values[valueIndex].value)) != 0) {
    setValue(valueIndex, mappedValue);
  }
}

void tickButton(uint8_t index) {
  Button* button = buttons[index];

  button->tick();

  if (button->isPressed() && button->currentStateTime() >= 200) {
    values[index].mute = true;
  }

  if (button->isThereAnEvent()) {
    Button::ButtonEvent event = button->getCurrentEvent();

    values[index].mute = !values[index].mute;

    button->ClearEvent();
  }
}

void tickPots() {
  for (int i = 0; i < NUM_POTS; i++) {
    int rawValue = analogRead(potPins[i]);
    int mappedValue = map(rawValue, 0, 1023, 0, DATA_RESOLUTION - 1);

    if (abs(abs(mappedValue) - abs(values[i].value)) > 2) {
      setValue(i, mappedValue);
    }
  }
}

void sendValues() {
  String builtString = String("");

  for (int i = 0; i < NUM_POTS + NUM_ENCODERS; i++) {
    builtString += String(getValue(i));

    if (i < NUM_POTS + NUM_ENCODERS - 1) {
      builtString += String("|");
    }
  }

  Serial.println(builtString);
}

void printValues() {
  for (int i = 0; i < NUM_POTS + NUM_ENCODERS; i++) {
    String printedString = String("Value #") + String(i + 1) + String(": ") + String(getValue(i));

    if (values[i].mute) {
      printedString += "(m)";
    }

    Serial.write(printedString.c_str());

    if (i < NUM_POTS + NUM_ENCODERS - 1) {
      Serial.write(" | ");
    } else {
      Serial.write("\n");
    }
  }
}
