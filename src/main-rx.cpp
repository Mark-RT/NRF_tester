#include <SPI.h>
#include "nRF24L01.h"
#include "RF24.h"

#define CE_PIN 9
#define CSN_PIN 10
RF24 radio(CE_PIN, CSN_PIN);

#define CH_NUM 0x60
#define SIG_POWER RF24_PA_HIGH
#define SIG_SPEED RF24_250KBPS

const int LED_GREEN = 3;
const int LED_RED  = 4;

byte pipeNo;
byte address[][6] = {"1Node","2Node","3Node","4Node","5Node","6Node"};
uint8_t received_data[1];

void setup() {
  Serial.begin(115200);
  pinMode(LED_RED, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);

  radio.begin();
  radio.setAutoAck(1);
  radio.setRetries(1, 15);
  radio.setPayloadSize(32);
  radio.openReadingPipe(1, address[0]);
  radio.setChannel(CH_NUM);
  radio.setPALevel(SIG_POWER);
  radio.setDataRate(SIG_SPEED);
  radio.powerUp();
  radio.startListening();

  digitalWrite(LED_RED, HIGH);
  digitalWrite(LED_GREEN, HIGH);
}

void loop() {
  // слушаем эфир
  while (radio.available(&pipeNo)) {
    radio.read(&received_data, sizeof(received_data));
    uint8_t state = received_data[0];

    // применяем состояние к LED
    if (state) {
      digitalWrite(LED_GREEN, HIGH);
      digitalWrite(LED_RED, LOW);
    } else {
      digitalWrite(LED_GREEN, LOW);
      digitalWrite(LED_RED, HIGH);
    }
  }
}