#include <Homie.h>
#include <cppQueue.h>
#include <EasyButton.h>
#include <Adafruit_SSD1306.h>
#include <Fonts/FreeSans9pt7b.h>
// has bug fixes, don't use stock lib:
#include "../lib/Adafruit_BMP085/src/Adafruit_BMP085.h"

#define FW_VER       "1.0.G" 
// #define DEBUG

#define PIN_BTN_CTR  0
#define PIN_BTN_UP   2
#define PIN_BTN_DN   12
#define PIN_LED_RED  14
#define PIN_LED_BLUE 13
#define PIN_RELAY    15
#define INTERACTION_TIMEOUT  120000

#define CLEARED     -10
#define UCLEARED     0

/* Struct for Queueing of postponed MQTT messages. Needed to gather messages while Wifi isn't connected yet. */
typedef struct strRec {
	char	topic[10];
	char	msg[90];
} Rec;
cppQueue msg_q(sizeof(Rec), 7, FIFO, true); 

typedef struct times {
	uint32_t	now;
	uint32_t	on;
	uint32_t	in;
} Times;

unsigned long epoch_time = CLEARED;         // local time
uint32_t epoch_recv_ts = UCLEARED;          // timestamp in ms when recieved the epoch time

const String PREFIX_VAR = "/var_";
const long ERR_NUM = -1;

enum boilerStates {
  DISPLAY_OFF,
  SHOW_STATUS,
  SET_DURATION
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
volatile bool last_relay_state = false;   // Used to display the state change when controlled not by human
volatile bool relayState;               // Keeps actual relay status

/* Switching from Homie 2.0.0 to Homie 3.0.1:
In HomieNode constructor add node ID as 1st argument. This implies the MQTT topic:
  Device ID from config.json (Homie 2.0.0 & 3.0.1) 
                    |
                    v
MQTT topic: homie/pump/pump/relay/
                        ^
                        |
  Device ID from name arg in constructor of Homie2.0.0
        and from id arg in constructor of Homie3.0.1
*/
HomieNode powerNode("boiler", "switch");

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
float current_timer_interval = CLEARED;
uint8_t start_time_h = UCLEARED;
uint8_t start_time_m = UCLEARED;
uint32_t agg_energy = UCLEARED;
int last_current_ma = CLEARED;
uint32_t last_current_alert_ts = UCLEARED;  // Use to fire single message if current leakage detected
#define DEFAULT_CURRENT_INTERVAL_S  10.0 
#define CURRENT_TOLERANCE_MA 1000
#define STATUS_INTERVAL      3000           // Change status display every 3 sec.
uint32_t last_display_update_ts = UCLEARED;
bool status_state = false;                  // Indicate which status display to show: now=false, future=true

bool suspend = false;
uint8_t tempMinC = 15;
uint8_t tempMaxC = 25;
uint8_t timeIncrementM = 10;

Adafruit_SSD1306 display = Adafruit_SSD1306(128, 64, &Wire);  // Use standard GPIO4-SDA, GPIO5-SCL
Adafruit_BMP085 bmp;
Ticker weather_timer;                     // Manage weather sensor
#define WEATHER_PERIOD         3600.0     // seconds
uint8_t min_temp = 100;                   // Register minimal temperature in a day, start/end by noon
int last_temp = 25;
int last_pres = CLEARED;
int day_top_pres = CLEARED;

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
    powerNode.setProperty("status").setRetained(false).send(">> " + String(name) + ": " + String(val));
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

  uint32_t restored = restoreVariable(name);
  if ((restored == val) && (restored != ERR_NUM)) return true;  // Avoid wearout

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
  uint32_t passed = (millis() - epoch_recv_ts) / 1000;  // div by 1000 since epoch time comes in seconds, not in millis 
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

Times calc_next_run() {
  Times res;
  res.now = calc_time_now();

  // Calc when next turn on
  res.on = start_time_h * 60 + start_time_m;
  res.in = (res.now > res.on)?(24*60 - res.now + res.on):(res.on - res.now);

  return res;
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
  int max_adc = 0;
  int min_adc = 1024;
  for(int i = 0; i<100; i++) {
    int t = analogRead(A0);
    soft_delay(7);
    if (t > max_adc) max_adc = t;
    if (t < min_adc) min_adc = t;
  }
  
  last_current_ma = (max_adc - min_adc) / 2 * 52;        // * mA per unit, calibrated
  if (relayState) {
    int power = last_current_ma * 230 / 1000;     // W 
    agg_energy += power;
    if ((millis() - start_relay_time_ms) / 1000 % 600 < 10) {
      powerNode.setProperty("current").setRetained(false).send("{\"current_ma\":" + String(last_current_ma) + 
                                                              ",\"power_w\":" + String(power) + "}");
    }
  }

  if (!relayState && last_current_ma > CURRENT_TOLERANCE_MA && 
      (UCLEARED == last_current_alert_ts || millis() > last_current_alert_ts + (int)(WEATHER_PERIOD * 1000))) {
    last_current_alert_ts = millis();
    powerNode.setProperty("alert").setRetained(false).send("Current of " + String(last_current_ma) + " mA detected while boiler is off");
    powerNode.setProperty("status").setRetained(false).send("min_adc:" + String(min_adc) + " max_adc:" + String(max_adc));
  }
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
  last_temp = bmp.readTemperature() - 9;  // TODO: Improve Correction due to local heating Empirically.

  if (last_temp > 50) {
#ifdef DEBUG
    Homie.getLogger() << ">> Wrong weather reading, retrying..." << endl;
#endif
    weather_timer.once(1.0, read_bmp, false);
    return;
  }
  if (last_temp < min_temp) min_temp = last_temp;
  reportWeather();
  weather_timer.once(WEATHER_PERIOD, read_bmp, true);
  if (last_pres > day_top_pres) day_top_pres = last_pres;
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
      String s2 = (manual_relay_duration_m > 0)?
                  (">II " + String((pick_timeout(true) - (millis() - start_relay_time_ms)) / 1000 / 60) + " MIN"):
                  "TAKE CARE";
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

void onLongPressCtr() {
  // Long press means "exit from state"
  switch (current_state)
  {
  case SHOW_STATUS:
    current_state = SET_DURATION;
    display_duration();
    break;

  case SET_DURATION:
    // total_relay_time_ms = manual_relay_duration_m * 60 * 1000;  // TODO: treat case of 0 duration
    run_reason = MANUAL;
    toggleRelay();
    // TODO: check there is no bug here: the SHOW_STATUS should be enabled from loopHandler on relay change.
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
  case SET_DURATION:
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
    case SET_DURATION:
      manual_relay_duration_m += timeIncrementM;
      display_duration();
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
    case SET_DURATION:
      manual_relay_duration_m -= timeIncrementM;
      if (manual_relay_duration_m < 0) manual_relay_duration_m = 0;
      display_duration();
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
    current_timer_interval = timeout / RELAY_TIME_DENOM;
    powerNode.setProperty("status").setRetained(false).send("Stop in " + String(timeout / 1000 / 60) + " minutes");
    digitalWrite(PIN_LED_BLUE, LOW);                             // Start with Blue (cold) LED
    led_timer.attach(current_timer_interval, setTimedLedColor);  // Turn LEDs as needed + run LED timer.
    relay_timer.once(timeout / 1000, offRelay);  // Set timer for relay only: LED is treated in setRelay
  } else {
    led_timer.attach(3.0, blink_led, PIN_LED_RED);
  }

  if (relayState) {
    start_relay_time_ms = millis();
    agg_energy = UCLEARED;
  } else {
    led_timer.detach();
    blink_led(PIN_LED_BLUE);
    blink_led(PIN_LED_RED);
    uint32_t on_time_s = (millis() - start_relay_time_ms) / 1000;
    // e = avg_current * hours / 1000 (kWh)
    // avg_current = total_current / num_of_measures
    String e = String((((float)agg_energy / ((float)on_time_s/DEFAULT_CURRENT_INTERVAL_S)) * (float)on_time_s / 3600.0) / 1000.0);
    powerNode.setProperty("current").setRetained(false).send("{\"energy_kwh\":" + e + "}");
    agg_energy = UCLEARED;
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

// Separated relay control for scheduler. Runs relay with adapted timeout.
void run_schedule() {
  if (suspend) return;

  Times t = calc_next_run();
  if (t.in != 0) return;

  // TODO: Improve adaptation of time with change of pressure: more clouds more time.
  adapted_relay_time_ms = total_relay_time_ms;
  
  // Do not change time if temperature is too low. Do not heat if temperature too high. Else Adapt.
  if (min_temp >= tempMaxC) {
    powerNode.setProperty("status").setRetained(false).send("Skip heating water. Min.temp:" + String(min_temp));
    return;
  }

  if (min_temp >= tempMinC) {
    uint8_t delta_t = tempMaxC - tempMinC;
    int delta_now = min_temp - tempMinC;
    if (delta_now > 0) adapted_relay_time_ms = total_relay_time_ms - delta_now * total_relay_time_ms / delta_t;
  }  

  int dp = day_top_pres - last_pres;
  if (dp > 0) {
    powerNode.setProperty("status").setRetained(false).send("Adapted time:" + String(adapted_relay_time_ms / 60 / 1000) + 
                                                            " for delta pressure:" + String(dp) +
                                                            " resulting: " + String(adapted_relay_time_ms / 60 / 1000 + dp / 2));
    adapted_relay_time_ms += dp * 60 * 1000 / 2;  // Add half minute of heat per each hPa of pressure drop.
  }
  day_top_pres = last_pres;

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

// homie/boiler/boiler/factory-reset/set true - reboot remotely
bool factoryResetHandler(const HomieRange& range, const String& value) {
  if (value != "true") return false;

  bool res = true;    
  Dir dir = SPIFFS.openDir("/");
  while (dir.next()) {
    bool r = SPIFFS.remove(dir.fileName());
    if (res && !r) res = r;  // Keep any negative result
  }
  powerNode.setProperty("status").setRetained(false).send("Configuration was" + String((res)?"":" not") + " removed");

  Homie.reboot();
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
  reportTimeToRun();
  time_timer.once(7.0*24*3600, request_epoch);
  reportEssentials();
  return true;
}

void reportTimeToRun() {
  Times t = calc_next_run();

  if (suspend) {
    powerNode.setProperty("status").setRetained(false).send("Boiler suspended, no plans for next run");
    return;
  }

  if (t.in == 0) t.in = 24 * 60;
  powerNode.setProperty("time2run").setRetained(false).send("{\"now\":\"" + mins2str(t.now) + 
            "\",\"run\":\"" + mins2str(t.on) + "\",\"in\":\"" + mins2str(t.in) + "\"}");
}

// homie/boiler/boiler/time2run/set anything - ask how long to wait till next run. Response with MQTT
bool timeToRunHandler(const HomieRange& range, const String& value) { 
  reportTimeToRun();
  return true;
}

// homie/boiler/boiler/repeat-for/set minutes - 100% of heating time when cold.
bool repeatForHandler(const HomieRange& range, const String& value) {
  long val = value.toInt();
  if (val <= 0) return false;
  total_relay_time_ms = val * 60 * 1000;
  storeVariable("total_relay_time_ms", total_relay_time_ms);
  return true;
}

// homie/boiler/boiler/repeat-on-h/set hours - time to start initial schedule (then use interval of repeat-every).
bool repeatOnHHandler(const HomieRange& range, const String& value) {
  long val = value.toInt();
  if (val < 0 || val > 23) return false;
  start_time_h = val;
  storeVariable("start_time_h", start_time_h);
  reportTimeToRun();
  return true;
}

// homie/boiler/boiler/repeat-on-m/set minutes - time to start initial schedule (then use interval of repeat-every).
bool repeatOnMHandler(const HomieRange& range, const String& value) {
  long val = value.toInt();
  if (val < 0 || val > 59) return false;
  start_time_m = val;
  storeVariable("start_time_m", start_time_m);
  reportTimeToRun();
  return true;
}

// homie/boiler/boiler/suspend/set true/false - suspend/resume boiler automation.
bool suspendHandler(const HomieRange& range, const String& value) {
  if ((value == "true") || (value == "false")) {
    suspend = value == "true";
    storeVariable("suspend", suspend);
    return true;
  }
  return false;
}

// homie/boiler/boiler/temp-min-c/set number - Lowest air temperature which allows max heating time.
bool tempMinCHandler(const HomieRange& range, const String& value) {
  uint8_t val = value.toInt();
  if (val <= 0 || val > tempMaxC) return false;
  tempMinC = val;
  storeVariable("tempMinC", tempMinC);
  return true;
}

// homie/boiler/boiler/temp-max-c/set number - Highest air temperature which stops heating.
bool tempMaxCHandler(const HomieRange& range, const String& value) {
  uint8_t val = value.toInt();
  if (val < tempMinC || val > 50) return false;
  tempMaxC = val;
  storeVariable("tempMaxC", tempMaxC);
  return true;
}

// homie/boiler/boiler/time-increment-m/set number - Increment/decrement time by buttons.
bool timeIncrementMHandler(const HomieRange& range, const String& value) {
  uint8_t val = value.toInt();
  if (val <= 0 || val > 60) return false;
  timeIncrementM = val;
  storeVariable("timeIncrementM", timeIncrementM);
  return true;
}


void reportWeather() {
  powerNode.setProperty("weather").setRetained(false).send("{\"temperature\":" + String(last_temp) + 
    // ",\"min_temp\":" + String(min_temp) + "}");
    ",\"min_temp\":" + String(min_temp) + ",\"max_pressure\":" + String(day_top_pres) +
    ",\"pressure\":" + String(last_pres) + ",\"time\":\"" + mins2str(calc_time_now()) + "\"}");
}

void reportEssentials() {
  powerNode.setProperty("time-incremrent-m").setRetained(false).send(String(timeIncrementM));
  powerNode.setProperty("temp-max-c").setRetained(false).send(String(tempMaxC));
  powerNode.setProperty("temp-min-c").setRetained(false).send(String(tempMinC));
  powerNode.setProperty("suspend").setRetained(false).send(suspend ? "true" : "false");
  powerNode.setProperty("repeat-on-h").setRetained(false).send(String(start_time_h));
  powerNode.setProperty("repeat-on-m").setRetained(false).send(String(start_time_m));
  powerNode.setProperty("repeat-for").setRetained(false).send(String(total_relay_time_ms / 60 / 1000));
  powerNode.setProperty("heat-now-m").setRetained(false).send(String(remote_relay_time_ms / 60 / 1000));
  powerNode.setProperty("on").setRetained(false).send(relayState ? "true" : "false");
  reportWeather();
}

void request_epoch() {
  // Request time if not set yet or set week ago.
  powerNode.setProperty("time").setRetained(false).send(String(millis()));
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

  if (relayState != last_relay_state) {
    last_relay_state = relayState;
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
  run_reason = AUTO;
  led_timer.detach();
  current_timer.attach(DEFAULT_CURRENT_INTERVAL_S, read_current);

  blink_led(PIN_LED_BLUE);
  blink_led(PIN_LED_RED);
  
  display.stopscroll();
  display.display();
  display.setTextSize(1);
  display_status();
  last_interaction_ts = last_display_update_ts = millis();
  // Get epoch time from hub
  time_timer.attach(WEATHER_PERIOD, request_epoch);
  request_epoch();  // If request succeeded then time_timer will be retriggered for rare updates only.
  schedule_timer.attach(60.0, run_schedule);
  day_top_pres = last_pres;
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
  led_timer.attach(0.5, blink_led, PIN_LED_BLUE);

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
  powerNode.advertise("factory-reset").settable(factoryResetHandler);
  powerNode.advertise("reset").settable(resetHandler);
  powerNode.advertise("on").settable(relayOnHandler);
  powerNode.advertise("ls").settable(lsHandler);
  powerNode.advertise("cat").settable(catHandler);
  powerNode.advertise("alert");
  powerNode.advertise("weather").settable(weatherHandler); 
  powerNode.advertise("time").settable(timeHandler);                  // Epoch time
  powerNode.advertise("repeat-on-h").settable(repeatOnHHandler);      // Start Hours schedule
  powerNode.advertise("repeat-on-m").settable(repeatOnMHandler);      // Start Minutes schedule
  powerNode.advertise("repeat-for").settable(repeatForHandler);       // MAx heat time Minutes schedule
  powerNode.advertise("heat-now-m").settable(heatForMHandler);        // Minutes now
  powerNode.advertise("time-increment-m").settable(timeIncrementMHandler);        // Minutes now
  powerNode.advertise("temp-min-c").settable(tempMinCHandler);        // Minutes now
  powerNode.advertise("temp-max-c").settable(tempMaxCHandler);        // Minutes now
  powerNode.advertise("suspend").settable(suspendHandler);            // true/false
  powerNode.advertise("time2run").settable(timeToRunHandler);         // Ask how long is till scheduled run
  
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

  suspend = restoreVariable("suspend", false);
  timeIncrementM = restoreVariable("timeIncrementM", 10);
  tempMinC = restoreVariable("tempMinC", 15);
  tempMaxC = restoreVariable("tempMaxC", 25);
  total_relay_time_ms = restoreVariable("total_relay_time_ms", 45 * 60 * 1000);
  start_time_h = restoreVariable("start_time_h", 5);
  start_time_m = restoreVariable("start_time_m", 55);
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
