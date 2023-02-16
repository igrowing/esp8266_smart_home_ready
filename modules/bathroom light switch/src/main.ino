#include <Homie.h>
#include <EasyButton.h>

#define FW_VER       "0.1.3" 

#define PIN_PIR       9
#define PIN_BTN       10
#define PIN_RELAY1    12
#define PIN_RELAY2    5
#define PIN_LED       13
#define UCLEARED      0

// #define DEBUG

enum runReason {
  AUTO,
  REMOTE,
  MANUAL
} run_reason;

typedef struct times {
  uint8_t h;
  uint8_t m;
} Times;

unsigned long epoch_time = UCLEARED;        // local time
uint32_t epoch_recv_ts = UCLEARED;          // timestamp in ms when recieved the epoch time
uint8_t offtime_end_h = 6;
uint8_t offtime_end_m = 0;
uint8_t offtime_start_h = 23;
uint8_t offtime_start_m = 0;
bool pir_readable = true;
uint32_t on_time_s = 120;
const String PREFIX_VAR = "/var_";
const long ERR_NUM = -1;

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
HomieNode powerNode("bathroom", "switch");

EasyButton button(PIN_BTN);
Ticker led_timer;
Ticker pir_timer;
Ticker relay_timer;
Ticker time_timer;              // Manages time update
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
 * 
 * --+--------------------------------------+------------------------------------------>
 *   +- epoch_time                          \_ millis
 *   \_ epoch_recv_ts
 ********************************************************************************************************************/



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

Times calc_time_now() {
  // Calc how long passed since epoch time received
  uint32_t passed = (millis() - epoch_recv_ts) / 1000;  // div by 1000 since epoch time comes in seconds, not in millis 
  // Add passed time to epoch time.
  long now = epoch_time + passed;
  // Calc time of the day in minutes
  uint32_t min = 60;
  uint32_t hour = min * 60;
  uint32_t day = hour * 24;
  uint8_t hours_now = now % day / hour;
  uint8_t mins_now = now % hour / min;
  Times t;
  t.h = hours_now;
  t.m = mins_now;
#ifdef DEBUG
  Homie.getLogger() << ">> passed:" << String(passed) << endl;
  Homie.getLogger() << ">> now:" << String(now) << endl;
  Homie.getLogger() << ">> hours_now:" << String(hours_now) << endl;
  Homie.getLogger() << ">> mins_now:" << String(mins_now) << "`" << lead_zero(mins_now) << "`" << lead_zero(5) << endl;
#endif

  return t;
}

String lead_zero(uint8_t num) {
  return (num < 10)?("0" + String(num)):String(num);
}

void make_pir_readable() {
  pir_readable = true;
}

uint32_t time2mins(uint8_t h, uint8_t m) {
  return (uint32_t)h * 60 + m;
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

void setRelay(uint8_t gpio, const bool on) {
  // Active low relay requires logic inversion.
  bool r = on;
  digitalWrite(gpio, r ? HIGH : LOW);
  String pwr = String("power") + String((gpio == PIN_RELAY1) ? "1" : "2");
  powerNode.setProperty(pwr).setRetained(false).send(r ? "true" : "false");
  powerNode.setProperty("status").setRetained(false).send(String(run_reason));
  Homie.getLogger() << pwr << " is " << (r ? "on" : "off") << endl;
}

// Handle button click to the switch.
void toggleRelay(uint8_t gpio) {
  bool on = digitalRead(gpio) == LOW;
  setRelay(gpio, on ? HIGH : LOW); 
}

void onPress() {
  run_reason = MANUAL;
  toggleRelay(PIN_RELAY1);
}

void double_click() {
  run_reason = MANUAL;
  toggleRelay(PIN_RELAY2);
}

void lights_off() {
  setRelay(PIN_RELAY1, false);
  if (digitalRead(PIN_RELAY2) == HIGH) setRelay(PIN_RELAY2, false);
}



/***********************************************************************
 *                     MQTT RELATED FUNCTIONS                          *
 **********************************************************************/

// Handle incoming MQTT message /homie/dual_r2/bathroom/power1/set <- true/false/toggle
bool relay1Handler(const HomieRange& range, const String& value) {
  return _relayHandler(PIN_RELAY1, value);
}

bool relay2Handler(const HomieRange& range, const String& value) {
  return _relayHandler(PIN_RELAY2, value);
}

bool _relayHandler(uint8_t gpio, const String& value) {
  if (value == "toggle") {
    run_reason = REMOTE;
    toggleRelay(gpio);
    return true;
  }

  if (value != "true" && value != "false") return false;
  run_reason = REMOTE;
  setRelay(gpio, (value == "true"));
  return true;
}

// homie/dual_r2/bathroom/factory-reset/set true - reboot remotely
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

// homie/dual_r2/bathroom/reset/set true - reboot remotely
bool resetHandler(const HomieRange& range, const String& value) {
  if (value != "true") return false;

  Homie.reboot();
  return true; 
}

// homie/dual_r2/bathroom/ls/set true - get weather so far
bool lsHandler(const HomieRange& range, const String& value) {
  if (value != "true") return false;

  ls();
  return true; 
}

// homie/dual_r2/bathroom/cat/set true - get weather so far
bool catHandler(const HomieRange& range, const String& value) {
  if (value != "true") return false;

  cat();
  return true; 
}

// homie/dual_r2/bathroom/time/set anything - Get epoch time
bool timeHandler(const HomieRange& range, const String& value) {
  // Drop ms to parse the number into 4 bytes
  String cut_val = value.substring(0, value.length()-3);
  unsigned long val = cut_val.toInt();
  if (val <= 0 ) return false;
  
  time_timer.detach();
  epoch_time = val;
  epoch_recv_ts = millis();
  time_timer.once(3600.0, request_epoch);
#ifdef DEBUG
    Homie.getLogger() << ">> epoch_time:" << String(epoch_time) << endl;
    Homie.getLogger() << ">> epoch_recv_ts:" << String(epoch_recv_ts) << endl;
#endif
  reportEssentials();
  return true;
}

// homie/dual_r2/bathroom/offtime-start-hhmm/set HHMM - time to start upper light disabled
bool offtimeStartHandler(const HomieRange& range, const String& value) {
  long val = value.toInt();
  byte hrs = val / 100;
  byte mins = val % 100;
  if (hrs < 0 || hrs > 23 || mins < 0 || mins > 59) return false;
  offtime_start_h = hrs;
  offtime_start_m = mins;
  storeVariable("offtime_start_h", offtime_start_h);
  storeVariable("offtime_start_m", offtime_start_m);
  powerNode.setProperty("offtime-start-hhmm").setRetained(false).send(String(offtime_start_h * 100 + offtime_start_m));
  return true;
}

// homie/dual_r2/bathroom/offtime-end-hhmm/set HHMM - time to end upper light disabled
bool offtimeEndHandler(const HomieRange& range, const String& value) {
  long val = value.toInt();
  byte hrs = val / 100;
  byte mins = val % 100;
  if (hrs < 0 || hrs > 23 || mins < 0 || mins > 59) return false;
  offtime_end_h = hrs;
  offtime_end_m = mins;
  storeVariable("offtime_end_h", offtime_end_h);
  storeVariable("offtime_end_m", offtime_end_m);
  powerNode.setProperty("offtime-end-hhmm").setRetained(false).send(String(offtime_end_h * 100 + offtime_end_m));
  return true;
}

// homie/dual_r2/bathroom/on-time-s/set int - time to keep lights on before re-triggering
bool ontimeHandler(const HomieRange& range, const String& value) {
  long val = value.toInt();
  if (val < 0) return false;
  on_time_s = val;
  storeVariable("on_time_s", on_time_s);
  powerNode.setProperty("on-time-s").setRetained(false).send(String(on_time_s));
  return true;
}

void reportEssentials() {
  powerNode.setProperty("offtime-start-hhmm").setRetained(false).send(String(offtime_start_h * 100 + offtime_start_m));
  powerNode.setProperty("offtime-end-hhmm").setRetained(false).send(String(offtime_end_h * 100 + offtime_end_m));
  powerNode.setProperty("power1").setRetained(false).send(digitalRead(PIN_RELAY1) ? "true" : "false");
  powerNode.setProperty("power2").setRetained(false).send(digitalRead(PIN_RELAY2) ? "true" : "false");
  powerNode.setProperty("on-time-s").setRetained(false).send(String(on_time_s));
  Times t_arr = calc_time_now();
  powerNode.setProperty("status").setRetained(false).send("time: " + lead_zero(t_arr.h) + ":" + lead_zero(t_arr.m));
  powerNode.setProperty("status").setRetained(false).send("last run reason: " + String(run_reason));
#ifdef DEBUG
  Homie.getLogger() << ">> arr0:" << lead_zero(t_arr.h) << "`" << String(t_arr.h) << endl;
  Homie.getLogger() << ">> arr1:" << lead_zero(t_arr.m) << "`" << String(t_arr.m) << endl;
#endif

}

// homie/dual_r2/bathroom/status/set any - respond to status request
bool statusHandler(const HomieRange& range, const String& value) {
  reportEssentials();
  return true;
}

void request_epoch() {
  // Request time if not set yet or set week ago.
  powerNode.setProperty("time").setRetained(false).send(String(millis()));
}


/***********************************************************************
 *                      SKETCH BASE FUNCTIONS                          *
 **********************************************************************/
void loopHandler() {
  button.read();

  // TODO
  if (pir_readable) {
    if (digitalRead(PIN_PIR) == LOW) {
      if (relay_timer.active()) relay_timer.detach();
      pir_timer.once(10.0, make_pir_readable);
      pir_readable = false;

      // Calculate if second relay is eligible to turn on
      Times t_arr = calc_time_now();
      int mins_now = time2mins(t_arr.h, t_arr.m);
      int mins_start = time2mins(offtime_start_h, offtime_start_m);
      int mins_end = time2mins(offtime_end_h, offtime_end_m);
#ifdef DEBUG
      powerNode.setProperty("status").setRetained(false).send("time: " + lead_zero(t_arr.h) + ":" + lead_zero(t_arr.m));
      String msg = String("mins_now:") + String(mins_now) + String(", mins_start:") + String(mins_start) + String(", mins_end:") + String(mins_end) + String("inverse:") + String(mins_start > mins_end);
      powerNode.setProperty("status").setRetained(false).send(msg);
#endif
      bool is_in_range = (mins_now - mins_start) * (mins_end - mins_now) > 0;
      if (mins_start > mins_end) {
        is_in_range = !is_in_range;
      }
#ifdef DEBUG
      msg = String("mins_now:") + String(mins_now) + String(", mins_start:") + String(mins_start) + String(", mins_end:") + String(mins_end) + String("inrange:") + String(is_in_range);
      powerNode.setProperty("status").setRetained(false).send(msg);
#endif
      setRelay(PIN_RELAY1, true);
      // Turn upper light only if the current time is not in "calm time" range.
      if (!is_in_range) setRelay(PIN_RELAY2, true);
      relay_timer.once((float)on_time_s, lights_off);
    }
  }
}

void setupHandler() {
  // Perform this function after Homie connected to Wifi.
  run_reason = AUTO;
  led_timer.detach();

  // Get epoch time from hub
  time_timer.attach(3600.0, request_epoch);
  request_epoch();  // If request succeeded then time_timer will be retriggered for rare updates only.

  // Alert on low free space
  uint32_t freeBytes = read_free_space();
  if (freeBytes <= 1024) {
    powerNode.setProperty("alert").setRetained(false).send( "Low storage space is free on SPIFF.");
  }
}


void setup() {
  SPIFFS.begin();  // Call SPIFFS before Homie awake: the state required ASAP.
  Serial.begin(115200);
  Serial << endl << endl;

  // Operate relay directly: Homie isn't ready yet to treat ESP properly at this stage.
  pinMode(PIN_PIR, INPUT_PULLUP);
  pinMode(PIN_BTN, INPUT_PULLUP);
  pinMode(PIN_RELAY1, OUTPUT);
  pinMode(PIN_RELAY2, OUTPUT);
  // led_timer.attach(0.5, blink_led, PIN_LED);

  Homie_setFirmware("bathroom", FW_VER);
  // Homie_setBrand("shm");  // homie ???
  // Homie.setResetTrigger(PIN_BUTTON, LOW, 5000);
  // Homie.disableResetTrigger();

  // Prohibit the parameters to be set from MQTT broker
  powerNode.advertise("status").settable(statusHandler);
  powerNode.advertise("offtime-end-hhmm").settable(offtimeEndHandler);      // Start Hours schedule
  powerNode.advertise("offtime-start-hhmm").settable(offtimeStartHandler);  // Start Minutes schedule
  powerNode.advertise("on-time-s").settable(ontimeHandler);                 // Time to keep lights on before re-triggering
  powerNode.advertise("factory-reset").settable(factoryResetHandler);
  powerNode.advertise("reset").settable(resetHandler);
  powerNode.advertise("power1").settable(relay1Handler);
  powerNode.advertise("power2").settable(relay2Handler);
  powerNode.advertise("ls").settable(lsHandler);
  powerNode.advertise("cat").settable(catHandler);
  powerNode.advertise("time").settable(timeHandler);                        // Epoch time
  powerNode.advertise("alert");
  
  Homie.setSetupFunction(setupHandler).setLoopFunction(loopHandler);

  // Define explicitly the LED pin for ESP8285. Default pin 2 of ESP8266 causes crash.
  Homie.setLedPin(PIN_LED, 0);
  Homie.setup();
  button.begin();
  button.onSequence(1, 1000, onPress);
  button.onSequence(2, 1000, double_click);
  button.onSequence(3, 1500, reportEssentials);

  on_time_s = restoreVariable("on_time_s", on_time_s);
  offtime_end_h = restoreVariable("offtime_end_h", offtime_end_h);
  offtime_end_m = restoreVariable("offtime_end_m", offtime_end_m);
  offtime_start_h = restoreVariable("offtime_start_h", offtime_start_h);
  offtime_start_m = restoreVariable("offtime_start_m", offtime_start_m);
  storeVariable("on_time_s", on_time_s);
  storeVariable("offtime_end_h", offtime_end_h);
  storeVariable("offtime_end_m", offtime_end_m);
  storeVariable("offtime_start_h", offtime_start_h);
  storeVariable("offtime_start_m", offtime_start_m);
  
}

void loop() {
  Homie.loop();
}