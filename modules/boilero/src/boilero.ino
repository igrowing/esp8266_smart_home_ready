#include <Homie.h>
#include <cppQueue.h>
#include <EasyButton.h>
#include <Adafruit_SSD1306.h>
#include <Fonts/FreeSans9pt7b.h>
// has bug fixes, don't use stock lib:
#include "../lib/Adafruit_BMP085/src/Adafruit_BMP085.h"

#define FW_VER       "1.0.7" 
#define DEBUG

#define PIN_LED_RED  14
#define PIN_LED_BLUE 13
#define PIN_CURRENT  ADC
#define PIN_RELAY    15
#define PIN_BTN_CTR  0
#define PIN_BTN_UP   2
#define PIN_BTN_DN   12
#define INTERACTION_TIMEOUT  120000

#define CLEARED     -10
#define UCLEARED     0

/* Struct for Queueing of postponed MQTT messages. Needed to gather messages while Wifi isn't connected yet. */
typedef struct strRec {
	char	topic[10];
	char	msg[90];
} Rec;
Queue msg_q(sizeof(Rec), 7, FIFO, true); 

unsigned long epoch_time = CLEARED;         // local time
uint32_t epoch_recv_ts = UCLEARED;          // timestamp in ms when recieved the epoch time

const String PREFIX_VAR = "/var_";
const long ERR_NUM = -1;

enum boilerStates {
  DISPLAY_OFF,
  SHOW_STATUS,
  DURATION,
  RESET
} current_state;

/******************  Multi-source Relay control - how it works   ***********************
 * Every time relay is on, the reason must be updated.
 * This is required to pick proper timeout and update the UI properly.
 **************************************************************************************/
enum runReason {
  AUTO,
  REMOTE,
  MANUAL
} run_reason;
volatile bool lastRelaystate = false;   // Used to display the state change when controlled not by human
volatile bool relayState;               // Keeps actual relay status

HomieNode powerNode("boiler", "switch");
HomieSetting<long> tempMinC_Setting("temp_min_c", "Temperature to use 100% of timer");  // id, description
HomieSetting<long> tempMaxC_Setting("temp_max_c", "Temperature to use 0% of timer");  // id, description
HomieSetting<long> timeIncrementM_Setting("time_increment_m", "Minutes to increase/decrease on every +/- button click");  // id, description
HomieSetting<bool> suspend_Setting("suspend", "Suspend automation while on vacation");  // id, description
HomieSetting<double> repeatOn_Setting("repeat_on", "Time of day the boiler must start");  // id, description

EasyButton button_ctr(PIN_BTN_CTR);
EasyButton button_up(PIN_BTN_UP);
EasyButton button_dn(PIN_BTN_DN);

/**********             Lightweight NTP Replacement               ***********************************
 * WHY:
 * NTP works on ESP8266 with Homie. However, it takes too much resource and makes OTA getting stuck in the middle.
 * This makes OTA impossible and impractical.
 * 
 * How it works:
 * - When Homie is connected:
 *   1. Periodic time_timer started with 1 hour update. This is for case the time was not received from MQTT broker.
 *   2. The 'time' topic sent to MQTT broker, requesting the epoch time. 
 *      Once epoch time is received and set, the time_timer is set to update the epoch time every week.
 * - Every time the epoch time received, extract from epoch time the local hour and minute. Keep a diff with millis(). 
 *   In loop compare the updated diff with requested hour to start the heating.
 ********************************************************************************************************************/
Ticker time_timer;              // Manages time update
Ticker schedule_timer;          // Controls the periodic relay on/off
Ticker write_timer;             // Allows to postpone write to SPIIF: saves the flash of re-write
Ticker current_timer;           // Manages current/power readings
Ticker relay_timer;             // Expiration timer
Ticker led_timer;
#define MAX_DUTY             10     // Range of visible change in LED PWM duty cycle
#define RELAY_TIME_DENOM     11000  // How often LED state is updated.
uint32_t last_interaction_ts = UCLEARED;
uint32_t total_relay_time_ms = UCLEARED;    // How long to run the boiler
uint32_t adapted_relay_time_ms = UCLEARED;  // How long to run the boiler calculated from the weahter and 100% stored runtime
uint32_t current_relay_time_ms = UCLEARED;  // How long to run the boiler calculated from the weahter and 100% stored runtime
uint32_t remote_relay_time_ms = UCLEARED;   // How long to run the boiler requested from MQTT
uint32_t start_relay_time_ms = UCLEARED;    // When boiler started
int32_t manual_relay_duration_m = CLEARED;  // How long to run the boiler requested from buttons
uint8_t start_time_h = UCLEARED;
uint8_t start_time_m = UCLEARED;
uint8_t start_every_h = UCLEARED;
uint32_t energy = UCLEARED;

#define STATUS_INTERVAL    3000             // Change status display every 3 sec.
uint32_t last_display_update_ts = UCLEARED;
bool status_state = false;                  // Indicate which status display to show: now=false, future=true
bool reset_confirm = false;

Adafruit_SSD1306 display = Adafruit_SSD1306(128, 64, &Wire);  // Use standard GPIO4-SDA, GPIO5-SCL
Adafruit_BMP085 bmp;
Ticker weather_timer;                     // Manage weather sensor
#define WEATHER_PERIOD         3600.0     // seconds
uint8_t min_temp = 100;                   // Register minimal temperature in a day, start/end by noon
int last_temp = 25;
int last_pres = CLEARED;

/***********************************************************************
 *                   UTILITY / SPECIFIC FUNCTIONS                      *
 **********************************************************************/
uint32_t read_free_space() {
  // Get space remaining.
  FSInfo info;
  SPIFFS.info(info);
  return (info.totalBytes - info.usedBytes);
} 

uint32_t restoreVariable(char *name, uint32_t def_val = ERR_NUM) {
  // Return numeric value stored in file named as: /var_varname.
  // Return -1 or provided default value if no file found or file is empty (stores nothing).
  File file = SPIFFS.open(PREFIX_VAR + String(name), "r");
  if (file) {
#ifdef DEBUG
    Homie.getLogger() << ">> file len = " << String(file.size()) << endl;
#endif
    // String val = file.readString();  // This crashes the ESP. Workaround:
    String val = "";
    while (file.available()) val += char(file.read());
    file.close();
#ifdef DEBUG
    Serial.println(">> " + String(name) + ": " + val);
    Serial.flush();
#endif
    return (val != "")?atol(val.c_str()):def_val;
  } else {
    Homie.getLogger() << "Can't restore: " << name << endl;
    return def_val;
  }
}

/* Return true if value stored in file named as: /var_varname.
*  Return false if writing problem.
*/
bool storeVariable(char *name, uint32_t val) {
  // Alert on low free space
  uint32_t freeBytes = read_free_space();
  if (freeBytes <= 1024) {
    Rec r = {"alert", "Low storage space is free on SPIFF. Toggle valve twice to free some space."};
    msg_q.push(&r);
  }

  if (restoreVariable(name) == val) return true;  // Avoid wearout

  File file = SPIFFS.open(PREFIX_VAR + String(name), "w");
  if (!file.print(String(val))) {
    powerNode.setProperty("alert").setRetained(false).send("Failed to write variable to SPIFFS: " + String(name) + "=" + String(val));
    return false;
  } else {
    return true;
  }
  file.close();
} 

void ls() {
    Dir dir = SPIFFS.openDir("/");
    while (dir.next()) {
      powerNode.setProperty("status").setRetained(false).send(dir.fileName() + " / " + dir.fileSize());
    }
}

void cat() {
    Dir dir = SPIFFS.openDir("/");
    while (dir.next()) {
        File file = SPIFFS.open(dir.fileName(), "r");
        String content = "";
        while (file.available()) content += char(file.read());
        file.close();
        powerNode.setProperty("status").setRetained(false).send(dir.fileName() + " : " + content);
        file.close();
    }
}

/* Wait and let background processes to run. */
void soft_delay(int ms) {
  uint32_t start = millis();
  while (start + ms < millis());
} 

uint32_t calc_mins(long time) {
  // Calc time of the day in minutes
  uint32_t min = 60;
  uint32_t hour = min * 60;
  uint32_t day = hour * 24;
  uint8_t hours_now = time % day / hour;
  uint8_t mins_now = time % hour / min;
  return hours_now * 60 + mins_now;
}

uint32_t calc_time_now() {
  // Calc how long passed since epoch time received
  uint32_t passed = (millis() - epoch_recv_ts) / 1000;
  // Add passed time to epoch time.
  long now = epoch_time + passed;
  return calc_mins(now);
}

String lead_zero(uint8_t num) {
  return (num < 10)?("0" + String(num)):String(num);
}

String mins2str(uint32_t mins) {
  return lead_zero(mins / 60) + ":" + lead_zero(mins % 60);
}

/***********************************************************************
 *                  HARDWARE RELATED FUNCTIONS                         *
 **********************************************************************/
void blink_led(int pin = 2) {
  // Blocking Active low turn LED on for 0.1 sec.
  uint32_t start = millis();
  digitalWrite(pin, LOW);
  while (millis() < start + 100);
  digitalWrite(pin, HIGH);
}

void setTimedLedColor() {
  uint32_t passed_ms = millis() - start_relay_time_ms;
  #define MAX_LIGHT         20
  #define OFF_LIGHT         30  
  // goes from 0 to 10 (MAX_DUTY). Higher value === less light.
  uint8_t led_duty = MAX_DUTY * passed_ms / current_relay_time_ms;  
  analogWrite(PIN_LED_BLUE, MAX_LIGHT + led_duty);
  analogWrite(PIN_LED_RED, OFF_LIGHT - led_duty);
}

void read_current() {
  /*  ADC encodes every 3.2mV per unit. 0.5V(500mV) max deviation from half range (1.65V) encodes measured TrueRMS 7.6A.
  *   => 155 units of ADC = 7.6A => 1 unit = 49mA TrueRMS.
  *   Power per unit: 49mA * 230V = 11.25W TrueRMS.
  *   To calculate consumed energy: Average power * time in hours = kWh 
  */
  int adc = 0;
  for(int i = 0; i<100; i++) {
    int t = analogRead(A0);
    soft_delay(7);
    if (t > adc) adc = t;
  }
  int current = abs(adc - 476) * 49;    // mA   476 = arbitrary zero
  int power = current * 230 / 1000;     // W 
  energy += power;
  powerNode.setProperty("current").setRetained(false).send("{\"current_ma\":" + String(current) + 
      ",\"power_w\":" + String(power) + "}");
}

void read_bmp(bool is_ready) {
  // Init first. Repeat until success.
  if (! is_ready && ! bmp.begin(BMP085_STANDARD))
  {
    blink_led(PIN_LED_RED);
    weather_timer.once(5.0, read_bmp, false);
    Homie.getLogger() << "Cannot begin BMP180 over I2C" << endl;
    // Give few tries before alerting
    if (millis() > 30000) {
      Rec r = {"alert", "Cannot begin BMP180 over I2C"};
      msg_q.push(&r);
    }
    blink_led(PIN_LED_RED);
  }

  // Clean minimal temperature by noon
  uint32_t minutes_now = calc_time_now();
  if (minutes_now > 720 && minutes_now <= 780) min_temp = 100;
  // Read pressure first. Read each measurement twice: first measure is wrong.
  last_pres = bmp.readPressure() / 100;
  last_pres = bmp.readPressure() / 100;
  if (last_pres < 600) last_pres *= 2;  // Correction of measure when sensor gets crazy.
  last_temp = bmp.readTemperature();
  last_temp = bmp.readTemperature();

  if (last_temp > 50) {
#ifdef DEBUG
    Homie.getLogger() << ">> Wrong weather reading, retrying..." << endl;
#endif
    weather_timer.once(1.0, read_bmp, false);
    return;
  }
  if (last_temp < min_temp) min_temp = last_temp;
  powerNode.setProperty("weather").setRetained(false).send("{\"temperature\":" + String(last_temp) + 
      // ",\"min_temp\":" + String(min_temp) + "}");
      ",\"min_temp\":" + String(min_temp) + ",\"pressure\":" + String(last_pres) + ",\"time\":" + mins2str(minutes_now) + "}");
  weather_timer.once(WEATHER_PERIOD, read_bmp, true);
}

void display_off() {
  current_state = DISPLAY_OFF;
  display.clearDisplay();
#ifdef DEBUG
  powerNode.setProperty("status").setRetained(false).send(String(millis()) + ":Display off " + String(current_state));
  Homie.getLogger() << "Display off " << String(current_state) << endl;
#endif
  display.display();
}

void display_status() {
#ifdef DEBUG
  Homie.getLogger() << "Display status " << String(current_state) << "," << status_state << endl;
#endif
  char * h1 = "ANY KEY: DISPLAY OFF";
  char * h2 = "  LONG: SET DURATION";
  h2[0] = 7;
  display.clearDisplay();
	display.setTextColor(WHITE);
  display.setFont();
	display.setCursor(0,0);
	display.println(h1);
	display.setCursor(0,10);
	display.println(h2);
  display.setFont(&FreeSans9pt7b);
  if (!status_state) {
    String s1 = "BOILER:" + String((relayState)?"ON":"OFF");
    if (relayState) {
      String s2 = ">II " + String((pick_timeout(true) - (millis() - start_relay_time_ms)) / 1000 / 60) + " MIN";
      display.setCursor(0,32);
      display.println(s1.c_str());
      display.setCursor(0,50);
      display.println(s2.c_str());
    } else {
      display.setCursor(0,36);
      display.println(s1.c_str());
    }
    display.setFont();
    display.setCursor(0, 56);
    display.println(WiFi.localIP().toString() + " v" + FW_VER); 
  } else {
    String s1 = "NEXT ON:";
    String s2 = mins2str(start_time_h * 60 + start_time_m);
    display.setCursor(0,32);
    display.println(s1.c_str());
    display.setCursor(0,50);
    display.println(s2.c_str());
    display.setFont();
    display.setCursor(0, 56);
    display.println(mins2str(calc_time_now()) + "      Temp:" + String(last_temp) + "\"C"); 
  }
  display.display();
}

void display_duration() {
#ifdef DEBUG
  Homie.getLogger() << "Display duration " << String(current_state) << endl;
#endif
  char * h1 = " / : SET DURATION";
  char * h2 = "  LONG: TOGGLE BOILER";
  h1[0] = 30;
  h1[2] = 31;
  h2[0] = 7;
  String s1 = "OFF AFTER";
  String s2 = String(manual_relay_duration_m) + " MIN";
  display.clearDisplay();
  display.setFont();
  display.setTextColor(WHITE);
  display.setCursor(0,0);
  display.println(h1);
  display.setCursor(0,10);
  display.println(h2);
  display.setFont(&FreeSans9pt7b);
  display.setCursor(0,35);
  display.println(s1);
  display.setCursor(0,58);
  display.println(s2);
  display.display();
}

void display_reset() {
#ifdef DEBUG
  Homie.getLogger() << "Display reset " << String(current_state) << endl;
#endif
  char * h1 = " / : YES/NO";
  char * h2 = "  LONG: DO";
  h1[0] = 30;
  h1[2] = 31;
  h2[0] = 7;
  String s1 = "RESET ALL?";
  String s2 = (reset_confirm)?"YES":"NO";
  display.clearDisplay();
  display.setFont();
  display.setTextColor(WHITE);
  display.setCursor(0,0);
  display.println(h1);
  display.setCursor(0,10);
  display.println(h2);
  display.setFont(&FreeSans9pt7b);
  display.setCursor(0,35);
  display.println(s1);
  display.setCursor(0,58);
  display.println(s2);
  display.display();
}

void onLongPressCtr() {
  // Long press means "exit from state"
  switch (current_state)
  {
  case SHOW_STATUS:
    current_state = DURATION;
    display_duration();
    break;

  case DURATION:
    // total_relay_time_ms = manual_relay_duration_m * 60 * 1000;  // TODO: treat case of 0 duration
    run_reason = MANUAL;
    toggleRelay();
    if (relayState) {
      current_state = SHOW_STATUS;
    } else {
      current_state = RESET;
      display_reset();
    }
    break;

  case RESET:
    Homie.reset();  // TODO: Reset user settings
    current_state = SHOW_STATUS;
    break;  

  default:
    display_status();
    current_state = SHOW_STATUS;
    break;
  }
  last_interaction_ts = millis();
#ifdef DEBUG
  powerNode.setProperty("status").setRetained(false).send(String(millis()) + ":Ctr Button long pressed! New state:" + String(current_state));
  Homie.getLogger() << "Ctr Button long pressed! New state:" << String(current_state) << endl;
#endif
}

void onPressCtr() {
  switch (current_state)
  {
  case SHOW_STATUS:
  case DURATION:
  case RESET:
    display_off();
    break;

  default:
    display_status();
    current_state = SHOW_STATUS;
    last_interaction_ts = millis();
    break;
  }
#ifdef DEBUG
  powerNode.setProperty("status").setRetained(false).send(String(millis()) + ":Ctr Button pressed! New state:" + String(current_state));
  Homie.getLogger() << "Ctr Button pressed! New state:" << String(current_state) << endl;
#endif
}

void onPressUp() {
  Serial.println("Button has been pressed!");
  switch (current_state) {
    case SHOW_STATUS:
      current_state = DISPLAY_OFF;
      display_off();
      break;
    case DURATION:
      manual_relay_duration_m += timeIncrementM_Setting.get();
      display_duration();
      break;
    case RESET:
      reset_confirm = ! reset_confirm;
      display_reset();
      break;
    default:
      display_status();
      current_state = SHOW_STATUS;
      break;
  }
  last_interaction_ts = millis();
#ifdef DEBUG
  powerNode.setProperty("status").setRetained(false).send(String(millis()) + ":Up Button pressed! New state:" + String(current_state) + ", new duration:" + String(manual_relay_duration_m));
  Homie.getLogger() << "Up Button pressed! New state:" << String(current_state) << ", new duration:" << String(manual_relay_duration_m) << endl;
#endif
}

void onPressDn() {
  switch (current_state) {
    case SHOW_STATUS:
      current_state = DISPLAY_OFF;
      display_off();
      break;
    case DURATION:
      manual_relay_duration_m -= timeIncrementM_Setting.get();
      if (manual_relay_duration_m < 0) manual_relay_duration_m = 0;
      display_duration();
      break;
    case RESET:
      reset_confirm = ! reset_confirm;
      display_reset();
      break;
    default:
      display_status();
      current_state = SHOW_STATUS;
      break;
  }
  last_interaction_ts = millis();
#ifdef DEBUG
  powerNode.setProperty("status").setRetained(false).send(String(millis()) + ":Down Button pressed! New state:" + String(current_state) + ", new duration:" + String(manual_relay_duration_m));
  Homie.getLogger() << "Down Button pressed! New state:" << String(current_state) << ", new duration:" << String(manual_relay_duration_m) << endl;
#endif
}

void setRelay(const bool on, uint32_t timeout = total_relay_time_ms) {
  // Active high logic
  relayState = on;
  digitalWrite(PIN_RELAY, relayState ? HIGH : LOW);
  powerNode.setProperty("on").setRetained(false).send(relayState ? "true" : "false");
  Homie.getLogger() << "Boiler is " << (relayState ? "on" : "off") << endl;

  // Do not set timer to turn relay off if timeout == 0
  if (timeout > 0) {
    current_relay_time_ms = timeout;  // Announce running time for LEDs.
    setTimedLedColor();               // Turn LEDs as needed + run LED timer.
    led_timer.attach(timeout / RELAY_TIME_DENOM, setTimedLedColor);
    relay_timer.once(timeout / 1000, offRelay);  // Set timer for relay only: LED is treated in setRelay
  } else {
    led_timer.attach(3.0, blink_led, PIN_LED_RED);
  }

  if (relayState) {
    start_relay_time_ms = millis();
    current_timer.attach(timeout / RELAY_TIME_DENOM, read_current);
    energy = UCLEARED;
  } else {
    led_timer.detach();
    current_timer.detach();
    blink_led(PIN_LED_BLUE);
    blink_led(PIN_LED_RED);
    String e = String((((float)energy / (float)(RELAY_TIME_DENOM / 1000)) * (float)(millis() - start_relay_time_ms) / 1000.0 / 3600.0) / 1000.0);
    powerNode.setProperty("current").setRetained(false).send("{\"energy_kwh\":" + e + "}");
    energy = UCLEARED;
  }
}

uint32_t pick_timeout(bool is_relay_already_set) {
  uint32_t timeout = 0;
  bool choice = (is_relay_already_set)?relayState:!relayState;
  switch (run_reason) {
    case MANUAL:
      timeout = (choice)?manual_relay_duration_m*60*1000:0;
      break;
    case REMOTE:
      timeout = (choice)?remote_relay_time_ms:0;
      break;
    default:
      timeout = (choice)?adapted_relay_time_ms:0;
      break;
  }
#ifdef DEBUG
  powerNode.setProperty("status").setRetained(false).send(String(millis()) + ":picked timeout:" + String(timeout) 
            + ", for relay state:" + String(relayState) + " and run reason:" + String(run_reason));
#endif
  return timeout;
}

// Handle button click to the switch.
void toggleRelay() {
  setRelay(!relayState, pick_timeout(false));
}

// Separated relay control for scheduler
void run_schedule() {
  // TODO: Improve adaptation of time with change of pressure: more clouds more time.
  adapted_relay_time_ms = total_relay_time_ms;
  
  // Do not change time if temperature is too low. Do not heat if temperature too high. Else Adapt.
  if (min_temp >= tempMaxC_Setting.get()) {
    powerNode.setProperty("status").setRetained(false).send("Skip heating water. Min.temp:" + String(min_temp));
    calc_next_run();
    return;
  }

  if (min_temp >= tempMinC_Setting.get()) {
    uint8_t delta_t = tempMaxC_Setting.get() - tempMinC_Setting.get();
    int delta_now = min_temp - tempMinC_Setting.get();
    if (delta_now > 0) adapted_relay_time_ms = delta_now * total_relay_time_ms / delta_t;
  }  

#ifdef DEBUG
  powerNode.setProperty("status").setRetained(false).send("Adapted time:" + String(adapted_relay_time_ms / 60 / 1000) + " for Min.temp:" + String(min_temp));
#endif  
  setRelay(true, adapted_relay_time_ms);
  run_reason = AUTO;
}

// Special workaround for calling from Ticker
void offRelay() {
  setRelay(false, 0);
}

/***********************************************************************
 *                     MQTT RELATED FUNCTIONS                          *
 **********************************************************************/

// Handle incoming MQTT message /homie/boiler/boiler/on/set <- true/false/toggle
bool relayOnHandler(const HomieRange& range, const String& value) {
  if (value == "toggle") {
    toggleRelay();
    run_reason = REMOTE;
    return true;
  }

  if (value != "true" && value != "false") return false;
  // bug: boilero uses internal timer setting when turned from network.
  setRelay((value == "true"), remote_relay_time_ms);
  run_reason = REMOTE;
  return true;
}

// homie/boiler/boiler/heat-now-m/set 40 - Set time for remotely controlled heating. Used with on/set
bool heatForMHandler(const HomieRange& range, const String& value) {
  long val = value.toInt();
  if (val < 0 ) return false;

  remote_relay_time_ms = val * 60 * 1000;
  return true;
}

bool displayHandler(const HomieRange& range, const String& value) {
  return true;
}

// homie/boiler/boiler/reset/set true - reboot remotely
bool resetHandler(const HomieRange& range, const String& value) {
  if (value != "true") return false;

  Homie.reboot();
  return true; 
}

// homie/boiler/boiler/weather/set true - get weather so far
bool weatherHandler(const HomieRange& range, const String& value) {
  if (value != "true") return false;

  read_bmp(true);
  return true; 
}

// homie/boiler/boiler/ls/set true - get weather so far
bool lsHandler(const HomieRange& range, const String& value) {
  if (value != "true") return false;

  ls();
  return true; 
}

// homie/boiler/boiler/cat/set true - get weather so far
bool catHandler(const HomieRange& range, const String& value) {
  if (value != "true") return false;

  cat();
  return true; 
}

// homie/boiler/boiler/time/set anything - Get epoch time
bool timeHandler(const HomieRange& range, const String& value) {
  // Drop ms to parse the number into 4 bytes
  String cut_val = value.substring(0, value.length()-3);
  unsigned long val = cut_val.toInt();
  if (val <= 0 ) return false;
  
  time_timer.detach();
  epoch_time = val;
  epoch_recv_ts = millis();
  calc_next_run();
  time_timer.once(7.0*24*3600, request_epoch);
  return true;
}

// homie/boiler/boiler/time2run/set anything - ask how long to wait till next run. Response with MQTT
bool timeToRunHandler(const HomieRange& range, const String& value) { 
  uint32_t now_minutes = calc_time_now();

  // Calc when next turn on
  uint32_t on_minutes = start_time_h * 60 + start_time_m;
  uint32_t mins2run = (now_minutes > on_minutes)?(24*60 - now_minutes + on_minutes):(on_minutes - now_minutes);
  powerNode.setProperty("time2run").setRetained(false).send("{\"now\":\"" + mins2str(now_minutes) + 
            "\",\"run\":\"" + mins2str(on_minutes) + "\",\"in\":\"" + mins2str(mins2run) + "\"}");
  return true;
}

// homie/boiler/boiler/repeat-for/set minutes - 100% of heating time when cold.
bool repeatForHandler(const HomieRange& range, const String& value) {
  long val = value.toInt();
  if (val <= 0 ) return false;
  total_relay_time_ms = val * 60 * 1000;
  storeVariable("total_relay_time_ms", total_relay_time_ms);
  return true;
}

// homie/boiler/boiler/repeat-every/set hours - Set period of run.
bool repeatEveryHandler(const HomieRange& range, const String& value) {
  long val = value.toInt();
  if (val <= 0 ) return false;
  start_every_h = val;
  storeVariable("start_every_h", start_every_h);
  return true;
}

bool suspendHandler(const HomieRange& range, const String& value) {
  return true;
}

void reportEssentials() {
  powerNode.setProperty("repeat-for").setRetained(false).send(String(total_relay_time_ms / 60 / 1000));
  powerNode.setProperty("repeat-every").setRetained(false).send(String(start_every_h));
  powerNode.setProperty("heat-now-m").setRetained(false).send(String(remote_relay_time_ms / 60 / 1000));
  // powerNode.setProperty("max-seconds").setRetained(false).send(String());
  // powerNode.setProperty("max-seconds").setRetained(false).send(String());
  read_bmp(true);
}

void request_epoch() {
  // Request time if not set yet or set week ago.
  powerNode.setProperty("time").setRetained(false).send(String(millis()));
}

void calc_next_run() {
  // Calculate when next time boiler should be on. Launch timer.
  uint32_t now_minutes = calc_time_now();

  // Calc when next turn on
  uint32_t on_minutes = start_time_h * 60 + start_time_m;
  float run_in = (now_minutes > on_minutes)?(24*60 - now_minutes + on_minutes):(on_minutes - now_minutes);
  schedule_timer.once(run_in * 60.0, run_schedule);
}


/***********************************************************************
 *                      SKETCH BASE FUNCTIONS                          *
 **********************************************************************/
void loopHandler() {
  button_ctr.read();
  button_up.read();
  button_dn.read();

  while (!msg_q.isEmpty() && Homie.isConnected()) {
    Rec r;
    msg_q.pop(&r);
    powerNode.setProperty(r.topic).setRetained(false).send(r.msg);
#ifdef DEBUG
    Homie.getLogger() << r.topic << ' >> ' << r.msg << endl;
#endif  
  }  

  // Turn display off
  if ((millis() - last_interaction_ts > INTERACTION_TIMEOUT) && current_state != DISPLAY_OFF) display_off();

  if (relayState != lastRelaystate && current_state != RESET) {
    lastRelaystate = relayState;
    display_status();
    current_state = SHOW_STATUS;
    last_interaction_ts = millis();
  }

  // Workaround timer limitation: callback cannot use yield() or delay(). Some I2C fucntions of display use yield().
  // So timer causes panic => crash. This old-fashion method works.
  if (current_state == SHOW_STATUS && millis() > (last_display_update_ts + STATUS_INTERVAL)) {
    last_display_update_ts = millis();
    status_state = !status_state;
    display_status();
  }
}

void setupHandler() {
  // Perform this function after Homie connected to Wifi.
  setRelay(false);
  run_reason = AUTO;
  blink_led(PIN_LED_BLUE);
  blink_led(PIN_LED_RED);
  // led_timer.attach(2.0, fade_led);
  
  display.stopscroll();
  display.display();
  display.setTextSize(1);
  display_status();
  last_interaction_ts = last_display_update_ts = millis();
  // Get epoch time from hub
  time_timer.attach(WEATHER_PERIOD, request_epoch);
  request_epoch();  // If request succeeded then time_timer will be retriggered for rare updates only.
  reportEssentials();
}

void setup() {
  SPIFFS.begin();  // Call SPIFFS before Homie awake: the state required ASAP.
  Serial.begin(115200);
  Serial << endl << endl;

  // Operate relay directly: Homie isn't ready yet to treat ESP properly at this stage.
  pinMode(PIN_LED_BLUE, OUTPUT);
  pinMode(PIN_LED_RED, OUTPUT);
  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_LED_BLUE, HIGH);
  digitalWrite(PIN_LED_RED, HIGH);
  digitalWrite(PIN_RELAY, LOW);
  relayState = LOW;

  // Init I2C ASAP to avoid conflicts with display
  read_bmp(false);
  pinMode(PIN_BTN_CTR, INPUT_PULLUP);
  pinMode(PIN_BTN_DN, INPUT_PULLUP);
  pinMode(PIN_BTN_UP, INPUT_PULLUP);
  Homie_setFirmware("boilero", FW_VER);
  // Homie_setBrand("shm");  // homie ???
  // Homie.setResetTrigger(PIN_BUTTON, LOW, 5000);
  Homie.disableResetTrigger();

  // Prohibit the parameters to be set from MQTT broker
  powerNode.advertise("current");
  powerNode.advertise("status");
  powerNode.advertise("display").settable(displayHandler);
  powerNode.advertise("reset").settable(resetHandler);
  powerNode.advertise("on").settable(relayOnHandler);
  powerNode.advertise("ls").settable(lsHandler);
  powerNode.advertise("cat").settable(catHandler);
  powerNode.advertise("alert");
  powerNode.advertise("weather").settable(weatherHandler); 
  powerNode.advertise("time").settable(timeHandler);                 // Epoch time
  powerNode.advertise("repeat-for").settable(repeatForHandler);      // Minutes schedule
  powerNode.advertise("repeat-every").settable(repeatEveryHandler);      // Minutes schedule
  powerNode.advertise("heat-now-m").settable(heatForMHandler);            // Minutes now
  powerNode.advertise("suspend").settable(suspendHandler);           // true/false
  powerNode.advertise("time2run").settable(timeToRunHandler);        // Ask how long is till scheduled run
  
  timeIncrementM_Setting.setDefaultValue(10).setValidator([] (long candidate) {
    return (candidate >= 0) && (candidate <= 100);
  });
  tempMinC_Setting.setDefaultValue(16).setValidator([] (long candidate) {
    return (candidate >= 0) && (candidate <= tempMaxC_Setting.get());
  });
  tempMaxC_Setting.setDefaultValue(23).setValidator([] (long candidate) {
    return (candidate >= tempMinC_Setting.get()) && (candidate <= 50);
  });
  suspend_Setting.setDefaultValue(false).setValidator([] (bool candidate) {
    return true;
  });
  repeatOn_Setting.setDefaultValue(5.55f).setValidator([] (double candidate) {
    return (candidate > 0.0f) && (candidate < 24.0f) && (100*(candidate - (int) candidate) < 60);
  });  // TODO: bug here, repeat_on is not read from settings
  // TODO: replace with store/restore and regular handler.
  // Calc time to turn relay on
  start_time_h = (uint8_t) repeatOn_Setting.get();
  start_time_m = (repeatOn_Setting.get() - start_time_h) * 100;

  Homie.setSetupFunction(setupHandler).setLoopFunction(loopHandler);

  Homie.setup();
  button_ctr.begin();
  button_up.begin();
  button_dn.begin();
  // Add the callback function to be called when the button is pressed.
  button_ctr.onPressedFor(400, onLongPressCtr);
  button_ctr.onPressed(onPressCtr);
  button_up.onPressed(onPressUp);
  button_dn.onPressed(onPressDn);

  total_relay_time_ms = restoreVariable("total_relay_time_ms", 45 * 60 * 1000);
  manual_relay_duration_m = total_relay_time_ms / 1000 / 60;
  adapted_relay_time_ms = total_relay_time_ms;
  current_state = SHOW_STATUS;

  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);  
  display.clearDisplay();
  display.display();
  // Display intro while booting
  display.setTextSize(3);
  display.setTextColor(BLACK, WHITE);
  display.setCursor(0, 30);
  display.println(" BOILERO ");
  display.startscrollright(0x00, 0x07);
  display.display();

  analogWriteFreq(50);
  // De facto, PWM works in values 20-30 when 20 is 100 LED on and 30 is LED off.
  analogWriteRange(30);
}

void loop() {
  Homie.loop();
}