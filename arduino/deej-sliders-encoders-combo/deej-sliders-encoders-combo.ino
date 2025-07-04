#include <RotaryEncoder.h>
#include <AisperButton.h>

#define NUM_POTS 3
#define NUM_BUTTONS 5
#define NUM_ENCODERS 2

#define LONG_CLICK_TIME 1000
#define MAX_TIME_BETWEEN_CLICKS 300

#define DATA_RESOLUTION 1024
#define DENOIZE 8
#define DATA_SEND_THRESHOLD 10

const int potPins[NUM_POTS] = { A1, A2, A3 };
const int buttonPins[NUM_BUTTONS] = { 14, 15, 18, 4, 7 };
const int encoderPins[NUM_ENCODERS * 2] = { 2, 3, 5, 6 };

struct VolData {
  int value;
  bool mute = false;
};

VolData lastSentValues[NUM_POTS + NUM_ENCODERS];
VolData values[NUM_POTS + NUM_ENCODERS];
Button* buttons[NUM_BUTTONS];
RotaryEncoder* encoders[NUM_ENCODERS];

void setup() {
  Serial.begin(9600);
  delay(300);
  Init();
}

void Init() {
  for (int i = 0; i < NUM_POTS; i++) {
    pinMode(potPins[i], INPUT);
  }

  for (int i = 0; i < NUM_BUTTONS; i++) {
    buttons[i] = new Button(buttonPins[i], MAX_TIME_BETWEEN_CLICKS, LONG_CLICK_TIME, true);
  }

  for (int i = 0; i < NUM_ENCODERS; i++) {
    encoders[i] = new RotaryEncoder(encoderPins[i * 2], encoderPins[i * 2 + 1], RotaryEncoder::LatchMode::TWO03);
  }
}

void loop() {
  for (int i = 0; i < NUM_BUTTONS; i++) {
    tickButton(i);
  }

  for (int i = 0; i < NUM_ENCODERS; i++) {
    tickEncoder(i);
  }

  tickPots();

  trySendValues();

  //printValues();
}

void setValue(uint8_t index, int newValue) {
  int clamped = constrain(newValue, -(DATA_RESOLUTION - 1), DATA_RESOLUTION - 1);
  values[index].value = clamped;
}

int getValue(uint8_t index) {
  return values[index].mute ? 0 : values[index].value;
}

void tickEncoder(uint8_t index) {
  RotaryEncoder* encoder = encoders[index];

  encoder->tick();

  const RotaryEncoder::Direction direction = encoder->getDirection();

  const int newValue = (int)direction * 22;

  const int valueIndex = NUM_POTS + index;

  if (values[valueIndex].value != newValue) {
    setValue(valueIndex, newValue);
  }
}

void tickButton(uint8_t index) {
  Button* button = buttons[index];

  button->tick();

  if (button->isPressed() && button->currentStateTime() >= 300) {
    values[index].mute = true;
    return;
  }

  if (button->isThereAnEvent()) {
    values[index].mute = !values[index].mute;

    button->ClearEvent();
  }
}

void tickPots() {
  for (int i = 0; i < NUM_POTS; i++) {
    int rawValue = analogRead(potPins[i]);
    int mappedValue = map(rawValue, 0, 1023, 0, DATA_RESOLUTION - 1);

    if (abs(abs(mappedValue) - abs(values[i].value)) > DENOIZE) {
      setValue(i, mappedValue);
    }
  }
}

void trySendValues() {
  bool shouldSendValues = false;

  for (int i = 0; i < NUM_POTS + NUM_ENCODERS; i++) {
    const int minValue = min(lastSentValues[i].value, values[i].value);
    const int maxValue = max(lastSentValues[i].value, values[i].value);

    // using threshold only for pots because encoders have synthetic data that can be compared raw
    const bool changeSignificantEnough = i < NUM_POTS ? abs(maxValue - minValue) >= DATA_SEND_THRESHOLD : lastSentValues[i].value != values[i].value;

    if (changeSignificantEnough || lastSentValues[i].mute != values[i].mute) {
      shouldSendValues = true;
      break;
    }
  }

  if (shouldSendValues) {
    sendValues();
  }
}

void sendValues() {
  String builtString = String("");

  for (int i = 0; i < NUM_POTS + NUM_ENCODERS; i++) {
    builtString += String(getValue(i));

    if (i < NUM_POTS + NUM_ENCODERS - 1) {
      builtString += String("|");
    }

    lastSentValues[i] = values[i];
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
