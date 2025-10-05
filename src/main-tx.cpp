#include <Arduino.h>
#include <uButton.h>

#include <SPI.h>
#include "nRF24L01.h"
#include "RF24.h"

// Hardware pins (адаптируйте при необходимости)
const int LED_BLUE = 2; // PWM
const int LED_GREEN = 3;
const int LED_RED = 4;
const int BUTTON_PIN = 5;

uButton b(BUTTON_PIN);

#define CE_PIN 9
#define CSN_PIN 10
RF24 radio(CE_PIN, CSN_PIN);

#define CH_NUM 0x60
#define SIG_POWER RF24_PA_MAX
#define SIG_SPEED RF24_250KBPS

byte address[][6] = {"1Node", "2Node", "3Node", "4Node", "5Node", "6Node"};

uint8_t txData[1];
bool currentState = 0;

enum State
{
  OFF,
  ON,
  FINDING
};
State blue_led_state = FINDING;
uint8_t pwm = 0; // текущая яркость 0..255

// Timers / intervals
const unsigned long SEND_INTERVAL_MS = 150UL; // отправка каждые 150 ms
const unsigned long STATS_WINDOW_MS = 900UL; // окно для подсчёта качества 1 s
const unsigned long LINK_LOST_MS = 2000UL;    // считаем связь потерянной если нет успеха > 2 s
const unsigned long BLUE_ON_MS = 150UL;       // длительность вспышки в режиме FINDING
const unsigned long BLUE_OFF_MS = 1850UL;     // длительность паузы в режиме FINDING

unsigned long lastSendMs = 0;
unsigned long statsWindowStart = 0;
unsigned long attemptsCnt = 0;
unsigned long successCnt = 0;
uint8_t qualityPct = 0;

unsigned long lastSuccessMs = 0; // время последнего успешного ACK
unsigned long findingStart = 0;  // таймер цикла FINDING

// helper: квантование процента в уровни (процент), возвращает 0/20/40/60/80/100
int quantizeToLevel(uint8_t pct)
{
  if (pct == 0)
    return 0;
  if (pct >= 90)
    return 100;
  if (pct >= 70)
    return 80;
  if (pct >= 50)
    return 60;
  if (pct >= 30)
    return 40;
  return 20; // 1..29 -> 20%
}

void setBlueModeFinding()
{
  if (blue_led_state != FINDING)
  {
    blue_led_state = FINDING;
    findingStart = millis();
    // turn off PWM immediately
    analogWrite(LED_BLUE, 0);
  }
}

void setBlueModeOnPWM(uint8_t pwmVal)
{
  if (blue_led_state == FINDING)
  {
    // stop finding blinking by resetting findingStart (not strictly necessary)
  }
  blue_led_state = ON;
  pwm = pwmVal;
  analogWrite(LED_BLUE, pwm);
}

void setBlueModeOff()
{
  blue_led_state = OFF;
  pwm = 0;
  analogWrite(LED_BLUE, 0);
}

// tick for blue led (non-blocking)
void blueLed_tick()
{
  unsigned long now = millis();
  switch (blue_led_state)
  {
  case OFF:
    // already set to 0 in setter
    break;

  case ON:
    // pwm already set in setter; nothing to do here
    // ensure analogWrite holds value (some MCUs keep it until changed)
    analogWrite(LED_BLUE, pwm);
    break;

  case FINDING:
    // simple finite-state blink: ON for BLUE_ON_MS, OFF for BLUE_OFF_MS, repeat
    if (now - findingStart < BLUE_ON_MS)
    {
      // ON part of the cycle
      analogWrite(LED_BLUE, 255);
    }
    else if (now - findingStart < (BLUE_ON_MS + BLUE_OFF_MS))
    {
      // OFF part
      analogWrite(LED_BLUE, 0);
    }
    else
    {
      // period finished -> restart cycle
      findingStart = now;
    }
    break;
  }
}

void setup()
{
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_BLUE, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_RED, OUTPUT);

  // По старте — состояние OFF (локальные светодиоды корректируем под currentState)
  currentState = false;
  digitalWrite(LED_GREEN, HIGH);
  digitalWrite(LED_RED, LOW);
  analogWrite(LED_BLUE, 0);

  // radio
  radio.begin();
  radio.setAutoAck(1);
  radio.setRetries(1, 15);
  radio.setPayloadSize(32);
  radio.openWritingPipe(address[0]);
  radio.setChannel(CH_NUM);
  radio.setPALevel(SIG_POWER);
  radio.setDataRate(SIG_SPEED);
  radio.powerUp();
  radio.stopListening();

  // timers init
  lastSendMs = millis();
  statsWindowStart = millis();
  lastSuccessMs = 0; // ещё не было успеха
  findingStart = millis();
  setBlueModeFinding();
}

void loop()
{
  unsigned long now = millis();

  // tick blue LED (independent)
  blueLed_tick();

  // обработка кнопки (uButton)
  if (b.tick())
  {
    if (b.click())
    {
      currentState = !currentState;
      Serial.print("Button toggled, new state: ");
      Serial.println(currentState ? "ON" : "OFF");
      // локальные LED меняем сразу (опционально, сейчас делаем мгновенно)
      if (currentState)
      {
        digitalWrite(LED_GREEN, LOW);
        digitalWrite(LED_RED, HIGH);
      }
      else
      {
        digitalWrite(LED_GREEN, HIGH);
        digitalWrite(LED_RED, LOW);
      }
    }
  }

  // Периодическая отправка состояния
  if (now - lastSendMs >= SEND_INTERVAL_MS)
  {
    lastSendMs = now;
    txData[0] = currentState ? 1 : 0;

    attemptsCnt++;
    bool ok = radio.write(&txData, sizeof(txData));
    if (ok)
    {
      successCnt++;
      lastSuccessMs = now;
      // Если раньше были в режиме FINDING — сразу прекратить мигание
      if (blue_led_state == FINDING)
      {
        // кратко включим полный свет при обнаружении приёмника
        analogWrite(LED_BLUE, 255);
        // запомним pwm — будет скорректировано в stats window
        pwm = 255;
        blue_led_state = ON;
      }
    }
  }

  // Проверка потери связи: если последний успешный ACK старше LINK_LOST_MS -> FINDING
  if ((lastSuccessMs == 0) || (now - lastSuccessMs > LINK_LOST_MS))
  {
    setBlueModeFinding();
  }
  // иначе — связь есть, яркость будет установлена по статистике ниже

  // Статистика качества каждое STATS_WINDOW_MS (1 сек)
  if (now - statsWindowStart >= STATS_WINDOW_MS)
  {
    if (attemptsCnt == 0)
    {
      qualityPct = 0;
    }
    else
    {
      // корректный расчёт (без потери точности)
      qualityPct = (uint8_t)((successCnt * 100UL) / attemptsCnt);
    }

    // Debug
    Serial.print("Attempts: ");
    Serial.print(attemptsCnt);
    Serial.print(", Success: ");
    Serial.print(successCnt);
    Serial.print(", Link quality %: ");
    Serial.println(qualityPct);

    // сброс окна
    attemptsCnt = 0;
    successCnt = 0;
    statsWindowStart = now;

    // применяем уровень только если связь есть (прошёл последний успех в пределах LINK_LOST_MS)
    if ((lastSuccessMs != 0) && (now - lastSuccessMs <= LINK_LOST_MS))
    {
      int lvlPct = quantizeToLevel(qualityPct); // 0/20/40/60/80/100
      if (lvlPct == 0)
      {
        // treat as finding (no successful ack in this window)
        setBlueModeFinding();
      }
      else
      {
        int pwmVal = map(lvlPct, 0, 100, 0, 255);
        setBlueModeOnPWM((uint8_t)pwmVal);
        Serial.print("Quantized quality %: ");
      Serial.println(pwmVal);
      }
    }
    else
    {
      // связь потеряна — FINDING already set above
      setBlueModeFinding();
    }
  }
}
