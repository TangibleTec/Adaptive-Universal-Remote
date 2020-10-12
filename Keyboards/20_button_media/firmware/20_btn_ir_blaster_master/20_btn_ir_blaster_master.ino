/*****************************************************************************
   AUTHOR: Ruben Kackstaetter
   DATE: September 14, 2020
   
   DESCRIPTION: Keyboard firmware for SparkFun Thing Plus (ESP32 WROOM).
   Developed to be paired to the Sparkfun WiFi IR Blaster over ESP-NOW, and
   Configured as a Roku TV remote.

    Depends on:
   * https://github.com/espressif/arduino-esp32
   * https://github.com/nickgammon/Keypad_Matrix
 ****************************************************************************/

#include "WiFi.h"
#include <EEPROM.h>
#include <esp_now.h>
#include <driver/rtc_io.h>
#include <Keypad_Matrix.h>

// IR Blaster MAC address used once to set EEPROM data
// No changes will be made when address is all 0xFF or
// this address matches what is in EEPROM
const uint8_t IR_BLASTER_MAC[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

enum key_cmd {
  send_ir,
  learn_ir,
  f_reset
};

// Structure to send cmd to IR blaster
typedef struct cmd_data {
  uint8_t key;
  uint8_t cmd;

} cmd_data;

// Declare cmdData
cmd_data cmdData;

RTC_DATA_ATTR uint32_t bootCount = 0;

typedef struct eeprom_data {
  uint8_t broadcastAddress[6];
  int analogOffset;
} eeprom_data;

// Declare EEPROM data
eeprom_data edata;

const uint16_t EEPROM_DATA_ADDR = 0;

const uint8_t DEFAULT_SLEEP_TIMEOUT_S = 60;
const uint8_t LOWBAT_SLEEP_TIMEOUT_S = 10;

// GPIO for the key matrix scanning
const byte ROWS = 5;
const byte ROW_PINS[ROWS] = {19, 5, 4, 15, 32}; 

const byte COLS = 5;
const byte COL_PINS[COLS] = {16, 18, 14, 21, 17}; 

// Define how the keypad is laid out
const char KEY_LAYOUT[ROWS][COLS] = {
  { 0, 1, 2, 3, 4},
  { 5, 6, 7, 8, 9},
  {10,11,12,13,14},
  {15,16,17,18,19},
  {20,21,22,23,24},
};

const char FN_KEY = 20;
const char PWR_KEY = 5;
const uint16_t FACTORY_RESET_HOLD_MS = 8000;

bool pwr_key_is_down = false;
unsigned long lastPwrKeyDown = 0;

// TODO: Redesign hardware to use RTC pins for all rows and columns.
// RTC GPIO for wake up, We can only use GPIO 4, 14, 15, 32 from key matrix.
// Rows as Wake Interrupt Triggers
const gpio_num_t WAKE_PIN = GPIO_NUM_4;
// Cols as Outputs.
const byte RTC_COLS = 1; // Ideally we would just use COLS from earlier, but not all cols are RTC
const gpio_num_t RTC_COL_PINS[RTC_COLS] = {GPIO_NUM_14};

const byte VBAT_SENSE_PIN = A2;

const byte INTERNAL_BTN_PIN = 0;
const uint16_t DO_CALIBRATION_HOLD_MS = 2000;
bool int_btn_is_down = false;
unsigned long lastIntBtnDown = 0;

const byte HAPTIC_PIN = 25;
const uint8_t HAPTIC_DUR_MS = 200;
unsigned long hapticStopMils = 0;

const byte BACKLIGHT_PIN = 26;
// Use first PWM channel of 16 channels
const uint8_t BACKLIGHT_CH = 0;
// Use 13 bit precision for LEDC timer
const uint8_t LEDC_TIMER_13_BIT = 13;
// Use 5000 Hz as a LEDC base frequency
const uint32_t LEDC_BASE_FREQ = 5000;
// Set Max Value for PWM Channel
const uint32_t BACKLIGHT_MAX_VAL = 255;
// Backlight Brightness and step amount with each fn key press
const uint8_t BACKLIGHT_BRIGHTNESS_STEP = BACKLIGHT_MAX_VAL/5;
RTC_DATA_ATTR int backlightBrightness = BACKLIGHT_BRIGHTNESS_STEP;
// Backlight Pattern Variables
unsigned long backlightMilOld = 0;
uint8_t backlightPatCnt = 0;
uint16_t backlightPatDurMs = 0;
uint8_t backlightPatTimes = 0;

// These values are use to determine when to consider the battery at certain states.
const uint16_t VBAT_LOW = 3680;
const uint16_t VBAT_CRITICAL = 3630;
bool lowBattery = false;

// Esp now tx retry
const uint32_t MAX_TX_RETRY = 24;
int txRetryCount = 0;

Keypad_Matrix kpd = Keypad_Matrix( makeKeymap (KEY_LAYOUT), ROW_PINS, COL_PINS, ROWS, COLS );

hw_timer_t *sleepTimer = NULL;

// LED Analog Write function to set backlight brightness
void setBacklight(uint32_t value) {
  // calculate duty, 8191 from 2 ^ 13 - 1
  uint32_t duty = (8191 / BACKLIGHT_MAX_VAL) * min(value, BACKLIGHT_MAX_VAL);
  ledcWrite(BACKLIGHT_CH, duty);
}

// Increase Brightness by one step
void stepBacklightBrightness() {
  if (backlightBrightness < BACKLIGHT_MAX_VAL && !lowBattery) {
    backlightBrightness += BACKLIGHT_BRIGHTNESS_STEP;
  } else {
    backlightBrightness = BACKLIGHT_BRIGHTNESS_STEP;
  }

  if (lowBattery) {
    backlightLowBattery();
  }
  
  Serial.print(F("Backlight: "));
  Serial.println(backlightBrightness);
  setBacklight(backlightBrightness);
}

// Non repeating flash pattern for backlight
// First entry always turns the backlight on
void updateBacklight () {
  if(backlightPatCnt < backlightPatTimes) {
    if((backlightMilOld + backlightPatDurMs) < millis()) {
      backlightMilOld = millis();
      if(backlightPatCnt % 2 == 0) {
          setBacklight(backlightBrightness);
      } else {
          setBacklight(0);
      }
      backlightPatCnt++;
    }
  }
}

void backlightSetPattern(uint16_t duration, uint8_t times) {
  backlightPatCnt = 0;
  backlightMilOld = 0;
  backlightPatDurMs = duration;
  backlightPatTimes = times;
}

void backlightLowBattery() {
  backlightSetPattern(100, 7);
}

void backlightLearnModeSent() {
  backlightSetPattern(100, 5);
}

void backlightDataSaved() {
  backlightSetPattern(100, 11);
}

void hapticVibrate() {
  hapticStopMils = millis() + HAPTIC_DUR_MS;
  digitalWrite(HAPTIC_PIN, HIGH);
}

void updateHaptic() {
  if (millis() >= hapticStopMils) {
    digitalWrite(HAPTIC_PIN, LOW);
  }
}

///////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////
// Since this analog input reading for the battery voltage varies
// between each device. An offset is calculated to ensure they all 
// at the very least will recognize 3600 analog reading as 3600 mV.
//
// Calculates and saves the offset of the current analog read to 3600.
// To do this this function can only be called when vBat is at 3600 mV.
///////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////
void calcSaveAnalogOffset() {
  uint32_t vbatValue = 0;
  const uint8_t samples = 30;
  
  for (uint8_t i = 0; i < samples; i++) {
   vbatValue += analogRead(VBAT_SENSE_PIN);
  }

  int vbatAveVal = vbatValue / samples;

  edata.analogOffset = (3600 - vbatAveVal);
  EEPROM.put(EEPROM_DATA_ADDR, edata);
  EEPROM.commit();
  Serial.print(F("Analog Offset: "));
  Serial.println(edata.analogOffset);
  backlightDataSaved();
}

void setBrodcastAddr() {
  const uint8_t NULL_ADDR[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
  bool isNullAddr = (memcmp(NULL_ADDR, IR_BLASTER_MAC, sizeof(IR_BLASTER_MAC)) == 0);

  if (!isNullAddr) {
    bool isNewAddr = (memcmp(edata.broadcastAddress, IR_BLASTER_MAC, sizeof(IR_BLASTER_MAC)) != 0);
    if (isNewAddr) {    
      memcpy(edata.broadcastAddress, IR_BLASTER_MAC, 6);
      printBrodcastAddress();
      EEPROM.put(EEPROM_DATA_ADDR, edata);
      EEPROM.commit();
      backlightDataSaved();
    }
  }
}

// Key Press Handler
void keyDown (const char which) {

  // Reset sleep timer on any key press
  timerWrite(sleepTimer, 0);
  
  if (backlightBrightness == BACKLIGHT_MAX_VAL) { 
    hapticVibrate();
  }
  
  cmdData.key = (int)which;

  if (which == FN_KEY) {
    cmdData.cmd = learn_ir;
  }

  if (which == PWR_KEY) {
    pwr_key_is_down = true;
    lastPwrKeyDown = millis();
  }
  
  Serial.print (F("Key down: "));
  Serial.println (cmdData.key);
}

// Key Release Handler
void keyUp (const char which) {
  Serial.print (F("Key up: "));
  Serial.println ((int)which);
  
  if (which == FN_KEY || cmdData.cmd == f_reset) {
    cmdData.cmd = send_ir;
    if (cmdData.key == FN_KEY) {
      stepBacklightBrightness();
    }
  } else {
    sendCmd(cmdData);
    if(cmdData.cmd == learn_ir) {
      backlightLearnModeSent();
    }
  }

  if (which == PWR_KEY) {
    pwr_key_is_down = false;
  }
  
}

void checkLongKeyPress() {
  unsigned long now = millis();
  if (pwr_key_is_down && (now - lastPwrKeyDown) > FACTORY_RESET_HOLD_MS) {
    cmdData.cmd = f_reset;
    sendCmd(cmdData);
    pwr_key_is_down = false;
    backlightDataSaved();
    Serial.println(F("Sending Factory Reset"));
  }
}

void sendCmd(cmd_data cmdData) {     
  esp_err_t result = esp_now_send(edata.broadcastAddress, (uint8_t *) &cmdData, sizeof(cmd_data));
     
  if (result != ESP_OK) {
    Serial.println(F("Error sending the data"));
  }
  
  Serial.print(F("key: "));
  Serial.print(cmdData.key);
  Serial.print(F(", cmd: "));
  Serial.println(cmdData.cmd);
}

/* ESPNOW Callback after data is sent */
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  Serial.print(F("\nLast Packet:\t"));
  if (status == ESP_NOW_SEND_SUCCESS) {
    Serial.println(F("Delivery Success"));
  } else {
    Serial.println(F("Delivery Failed, Retrying last command"));
    if (txRetryCount < MAX_TX_RETRY) {
      sendCmd(cmdData);
      txRetryCount += 1;
    } else {
      txRetryCount = 0;
    }
  }
}

/* Print what triggered ESP32 wakeup from sleep */
void print_wakeup_trigger(){
  switch(esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_EXT0 : Serial.println("Wakeup triggered by EXT0 (RTC_IO)"); break;
    case ESP_SLEEP_WAKEUP_EXT1 :
        Serial.println("Wakeup triggered by EXT1 (RTC_CNTL)");
        Serial.print("GPIO that triggered the wake up: GPIO ");
        Serial.println((log(esp_sleep_get_ext1_wakeup_status()))/log(2), 0);
      break;
    case ESP_SLEEP_WAKEUP_TIMER : Serial.println("Wakeup triggerd by Timer"); break;
    case ESP_SLEEP_WAKEUP_TOUCHPAD : Serial.println("Wakeup Triggerd by Touchpad"); break;
    case ESP_SLEEP_WAKEUP_ULP : Serial.println("Wakeup Triggerd by ULP Program"); break;
    default : Serial.printf("Wakeup Triggered by Unknown: %d\n",esp_sleep_get_wakeup_cause()); break;
  }
}

/* Configure RTC GPIO to use the keyboard matrix as an external trigger */
void initRtcGpio() {
  // Configure the wake up source as external triggers.
  esp_sleep_enable_ext0_wakeup(WAKE_PIN, HIGH);
  // Ensure wake pin is pulled down to prevent false triggers.
  pinMode(WAKE_PIN, INPUT_PULLDOWN);
  
  // Set all keyboard matrix columns to be on during sleep.
  for (uint8_t i = 0; i < RTC_COLS; i++) {
    rtc_gpio_init(RTC_COL_PINS[i]);
    rtc_gpio_set_direction(RTC_COL_PINS[i], RTC_GPIO_MODE_OUTPUT_ONLY);
    rtc_gpio_set_level(RTC_COL_PINS[i], HIGH);
    rtc_gpio_hold_en(RTC_COL_PINS[i]);
  }
}

/* Remove GPIO holds for all keyboard matrix columns */
void clearRtcGpioHolds() {
  for (uint8_t i = 0; i < RTC_COLS; i++) {
    rtc_gpio_hold_dis(RTC_COL_PINS[i]);
  }
}

void IRAM_ATTR startDeepSleep() {
  initRtcGpio();
  Serial.println("Entering Deep Sleep");
  delay(1000);
  esp_deep_sleep_start();
}

void checkBattery() {
  uint32_t vbatValue = 0;
  const uint8_t samples = 30;
  
  for (uint8_t i = 0; i < samples; i++) {
   vbatValue += analogRead(VBAT_SENSE_PIN);
  }

  uint16_t vbatAveVal = vbatValue / samples;
  vbatAveVal += edata.analogOffset;

  Serial.print (F("Battery Level: "));
  Serial.println (vbatAveVal);
  
  if (vbatAveVal < VBAT_CRITICAL) {
    Serial.println (F("Battery CRITICAL!"));
    checkInternalButton();
    if (int_btn_is_down) {
      Serial.println (F("Skipping deep-sleep on CRITICAL battery for calibration"));
      backlightLearnModeSent();
      lowBattery = true;
    } else {
      startDeepSleep();
    }
  } else  if (vbatAveVal < VBAT_LOW) {
    if (!lowBattery) {
      Serial.println (F("Battery Low!"));
      lowBattery = true;
      backlightLowBattery();
    }
  } else {
    if (lowBattery) {
      Serial.println (F("Battery No longer Low"));
      lowBattery = false;
    }
  }
}

void printBrodcastAddress() {
  Serial.print(F("Paired to MAC: "));
  for (uint8_t i = 0; i < 6; i++) {
    Serial.print(edata.broadcastAddress[i],HEX);
    if (i < 5) {
      Serial.print(":");
    }
  }
  Serial.println();
}

void checkInternalButton() {
  unsigned long now = millis();
  bool btnIsDown = (digitalRead(INTERNAL_BTN_PIN) == LOW);
  
  if (int_btn_is_down != btnIsDown) {
      int_btn_is_down = btnIsDown;
      if (int_btn_is_down) {
        lastIntBtnDown = now;
      }
  }
  
  if (int_btn_is_down && (now - lastIntBtnDown) > DO_CALIBRATION_HOLD_MS) {
    calcSaveAnalogOffset();
    int_btn_is_down = false;
  }
}

void startSleepTimer() {
  sleepTimer = timerBegin(0, 80, true);
  timerAttachInterrupt(sleepTimer, &startDeepSleep, true);
  if (lowBattery) {
    timerAlarmWrite(sleepTimer, LOWBAT_SLEEP_TIMEOUT_S * 1000000, false);
  } else {
    timerAlarmWrite(sleepTimer, DEFAULT_SLEEP_TIMEOUT_S * 1000000, false);
  }
  timerAlarmEnable(sleepTimer);
}

void initEsp32Now() {
  WiFi.mode(WIFI_STA);
 
  Serial.println();
  Serial.print(F("MAC Address: "));
  Serial.println(WiFi.macAddress());
 
  if (esp_now_init() != ESP_OK) {
    Serial.println(F("Error initializing ESP-NOW"));
    return;
  }

  // Register for send CB to get the status of TX data
  esp_now_register_send_cb(OnDataSent);
 
  // register peer
  esp_now_peer_info_t peerInfo;

  setBrodcastAddr();
  memcpy(peerInfo.peer_addr, edata.broadcastAddress, 6);
  peerInfo.channel = 0;  
  peerInfo.encrypt = false;
         
  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Failed to add peer");
    return;
  }
}

void setup() {
  Serial.begin(115200);

  EEPROM.begin(sizeof(edata));
  EEPROM.get(EEPROM_DATA_ADDR, edata);

  // There is some timing issue I don't understand here.
  // initEsp32Now() must be called as soon as possible to avoid
  // "ESPNOW: Peer interface is invalid" error.
  initEsp32Now();

  print_wakeup_trigger();

  pinMode(VBAT_SENSE_PIN, INPUT);
  pinMode(INTERNAL_BTN_PIN, INPUT_PULLUP);
 
  pinMode(HAPTIC_PIN, OUTPUT);
  digitalWrite(HAPTIC_PIN, LOW);

  // Setup PWM timer and attach to backlight pin
  ledcSetup(BACKLIGHT_CH, LEDC_BASE_FREQ, LEDC_TIMER_13_BIT);
  ledcAttachPin(BACKLIGHT_PIN, BACKLIGHT_CH);

  Serial.print(F("Analog Offset: "));
  Serial.println(edata.analogOffset);
  
  checkBattery();
  
  ++bootCount;
  Serial.print(F("Boot number: "));
  Serial.println(bootCount);

  printBrodcastAddress();

  clearRtcGpioHolds();
  
  kpd.begin();
  kpd.setKeyDownHandler(keyDown);
  kpd.setKeyUpHandler(keyUp);
  
  cmdData.cmd = send_ir;

  setBacklight(backlightBrightness);
  startSleepTimer();
}

void loop() {
  kpd.scan();
  checkLongKeyPress();
  updateBacklight();
  updateHaptic();
  checkInternalButton();
}
