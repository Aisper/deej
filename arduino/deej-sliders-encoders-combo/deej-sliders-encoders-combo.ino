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

bool receiveData(uint8_t* receivedData, uint8_t len) {
  bool bReceiving = false;
  uint8_t index = 0;

  while (Serial.available() > 0) {
    uint8_t byte = Serial.read();

    bReceiving = true;
    receivedData[index] = byte;

    index++;

    if (index >= len) {
      break;
    }
  }

  return bReceiving;
}

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
  uint8_t receivedData[NUM_ENCODERS + NUM_POTS];
  if (receiveData(receivedData, NUM_ENCODERS + NUM_POTS)) {
    // process incoming data
    for (uint8_t i = 0; i < NUM_POTS + NUM_ENCODERS; i++) {
      int mappedValue = map((byte)receivedData[i], 0, 255, 0, 1023);
      setValue(i, mappedValue);

      if (i >= NUM_POTS) {
        mappedValue = map(mappedValue, 0, 1023, ENCODER_MIN, ENCODER_MAX);
        encoders[i - NUM_POTS]->setPosition(mappedValue);
      }
    }
  }

  for (int i = 0; i < NUM_BUTTONS; i++) {
    tickButton(i);
  }

  for (int i = 0; i < NUM_ENCODERS; i++) {
    tickEncoder(i);
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
