#include <Homie.h>
#include <Adafruit_BME680.h>

/********************************************************************************************
 * Definition, description, explanation.
 ********************************************************************************************/
// Abbreviations:
// u    - unsigned
// ts   - timestamp
// s    - second
// ms   - millisecond
// btn  - button
// trsh - threshold

// HW pins definitons:
// SDA 4
// SCL 5
#define PIN_RELAY   13
#define PIN_BUTTON  0
#define PIN_MY_LED  2
#define PIN_PIR     14

#define CLEARED     -10
#define CLEARED_U   0
#define EVOC_TRSH   30
#define PIR_REP_INTERVAL_MS 60000
#define HPA_COMPARE_INTERVAL_MS 3600000  // 1 hour
const String PREFIX = "/state";  // part of file name

HomieNode roomNode("power", "switch");
// Bounce debouncer = Bounce(); // Bounce is built into Homie, so you can use it without including it first
volatile bool flag_air_alerted = false;
volatile bool flag_error_reported = false;
uint32_t lastStatus_ts = CLEARED_U;
uint32_t forecast_ts = CLEARED_U;
float last_hpa = CLEARED;
volatile uint8_t i_forecast = 0;
const String forecast_sky[] = {"No change", "Storm", "Rain", "Likely rain", "Cloudy, Warmer", "Clearing, Cooler", "Fair"};
volatile uint32_t status_interval_ms = 900000;  // Interval to send reports
volatile uint32_t outlet_timer_s = 900;         // Interval to turn outlet on by long click
volatile uint32_t lastPirStatus_ts = CLEARED_U;
volatile uint32_t lastFileIndex = 0;
volatile int lastBtnValue = CLEARED;
volatile bool relayState;
volatile uint16_t measure_count = CLEARED_U;
volatile float measure_temp = CLEARED;
volatile float measure_humi = CLEARED;
volatile float measure_press = CLEARED;
volatile float measure_air = CLEARED;
volatile float measure_quality = CLEARED;

Ticker measureTimer;
Ticker writeTimer;
Ticker expTimer;
Adafruit_BME680 bme;
#define BME_BURN_IN_TIME 300000  // msec. Reading every 5 sec => 60 readings for calibration.
volatile float gas_base = 0.0;  // 0 -> not calibrated yet. !0 -> calibrated and can be calculated.
const float HUM_BASE = 40.0;    // 40% is optimal indoor humidity.
const uint8_t HUM_WEIGH = 25;   // air_quality_score (25:75, humidity:gas)

#define BTN_UP          0
#define BTN_CLICK       1
#define BTN_CLICK_LONG  2
#define BTN_CLICK_DBL   3
#define BTN_CLICK_TMP   4
#define DEBOUNCE        10
uint_fast32_t btn_dn_time = 0;
// DO NOT READ btn_state from code. Use get_button_state() function for proper results.
uint_fast8_t  btn_state = CLEARED_U;
uint_fast32_t btn_click_time = 0;
Ticker btnTimer;

/********************************************************************************************
 * Hardware functions  -- process inputs and outputs.
 ********************************************************************************************/
void blinkLED() {
  // turn LED on for 0.1 sec.
  uint32_t start = millis();
  digitalWrite(PIN_MY_LED, LOW);
  while (millis() < start + 100);
  digitalWrite(PIN_MY_LED, HIGH);
}

void handlePir() {
  // turn LED on for 1 sec. Send report.
  uint32_t start = millis();
  digitalWrite(PIN_MY_LED, LOW);
  if (millis() > lastPirStatus_ts + PIR_REP_INTERVAL_MS) {
    lastPirStatus_ts = millis();
    roomNode.setProperty("pir").setRetained(false).send("Move detected");
  }
  while (millis() < start + 1000);
  digitalWrite(PIN_MY_LED, HIGH);
}

void set1click() {
  btn_state = BTN_CLICK;
  roomNode.setProperty("status").setRetained(false).send("set1click ended with:" + String(btn_state));
}

void handleBtnDownInt() {
  attachInterrupt(digitalPinToInterrupt(PIN_BUTTON), handleBtnUpInt, RISING);
  btn_dn_time = millis();
  digitalWrite(PIN_MY_LED, LOW);
}

void handleBtnUpInt() {
  digitalWrite(PIN_MY_LED, HIGH);
  uint_fast32_t delta_ms = millis() - btn_dn_time;
  attachInterrupt(digitalPinToInterrupt(PIN_BUTTON), handleBtnDownInt, FALLING);
  if (delta_ms < DEBOUNCE) ;
  else if (delta_ms <= 500) {
    if (btn_state == BTN_CLICK_TMP) {
      btn_state = (millis() - btn_click_time > 1000)?BTN_CLICK:BTN_CLICK_DBL;
      btnTimer.detach();
    } else {
      btn_state = BTN_CLICK_TMP;
      btn_click_time = millis();
      btnTimer.once(0.6, set1click);
    }
  }
  else if (delta_ms > 500) btn_state = BTN_CLICK_LONG;
  else btn_state = BTN_UP;

  roomNode.setProperty("status").send("Btn ended with:" + String(btn_state));

}

uint_fast8_t get_button_state() {
  if (btn_state != BTN_CLICK_TMP) {
    uint_fast8_t res = btn_state;
    btn_state = BTN_UP;
    return res; 
  } else {
    return BTN_UP;
  }
}

// TODO: Relay status isn't sent on boot (however the unit status is sent).
void setRelay(const bool on) {
  relayState = on;
  digitalWrite(PIN_RELAY, on ? HIGH : LOW);  // Change control here if needed. Now it's active high.
  roomNode.setProperty("on").setRetained(false).send(on ? "true" : "false");
  Homie.getLogger() << "Outlet is " << (on ? "on" : "off") << endl;
  writeTimer.once(0.5, writeState);  // Write state to SPIFFS
}

// Handle button click to the switch.
void toggleRelay() {
  setRelay(digitalRead(PIN_RELAY) != HIGH);                 // Change control here if needed. Now it's active high.
}

// Calibrate gas sensor before any measurement.
void calibrate() {
  measure();
  if (millis() > BME_BURN_IN_TIME) {  // Got enough samples for calibration
    gas_base = measure_air / measure_count;
    measureTimer.detach();
    measureTimer.attach(5.0, measure);
  }
}

// Add measurements to aggregative variables. Calculate average in report time.
void measure() {
  if (bme.performReading()) {
    if (measure_air == CLEARED) {
      measure_air = bme.gas_resistance / 1000.0;
    } else {
      measure_air += bme.gas_resistance / 1000.0;
    }
    if (measure_temp == CLEARED) {
      measure_temp = bme.temperature - 7.0;
    } else {
      measure_temp += bme.temperature - 7.0;
    }
    if (measure_humi == CLEARED) {
      measure_humi = bme.humidity;
    } else {
      measure_humi += bme.humidity;
    }
    if (measure_press == CLEARED) {
      measure_press = bme.pressure / 100.0;
    } else {
      measure_press += bme.pressure / 100.0;
    }
    measure_count++;
  } else if (!flag_error_reported) {  // Report only once till next power cycle
    roomNode.setProperty("status").send("Failed to read BME680");
    Homie.getLogger() << "Failed to read BME680" << endl;
    flag_error_reported = true;
  }

  if (gas_base != 0.0) {
    float hum = bme.humidity;
    float hum_offset = hum - HUM_BASE;
    //  Calculate hum_score as the distance from the hum_baseline.
    float hum_score = HUM_BASE + hum_offset;  // Calc score for negative offset first. Then overwrite if needed.
    hum_score /= HUM_BASE;
    if (hum_offset > 0) {
      hum_score = 100 - HUM_BASE - hum_offset;
      hum_score /= (100 - HUM_BASE);
    }
    hum_score *= HUM_WEIGH;

    float gas = bme.gas_resistance / 1000.0;
    float gas_offset = gas_base - gas;
    //  Calculate gas_score as the distance from the gas_baseline.
    float gas_score = 100 - HUM_WEIGH;
    if (gas_offset > 0) {
        gas_score = gas / gas_base;
        gas_score *= (100 - HUM_WEIGH);
    }

    // Calculate air_quality_score.
    if (measure_quality == CLEARED) {
      measure_quality = hum_score + gas_score;
    } else {
      measure_quality += hum_score + gas_score;
    }
  }
}

/********************************************************************************************
 * MQTT functions  -- communication and data exchange.
 ********************************************************************************************/
// Handle incoming MQTT message /homie/unit-name/power/on/set <- true/false/toggle
bool lightOnHandler(const HomieRange& range, const String& value) {
  if (value == "toggle") {
    toggleRelay();
    return true;
  }

  if (value != "true" && value != "false") return false;

  setRelay((value == "true"));
  return true;
}

// Handle incoming MQTT message /homie/unit-name/power/status-interval-s/set <- int
bool statusIntervalHandler(const HomieRange& range, const String& value) {
  int interval = value.toInt();
  if (interval == 0) return false;
  status_interval_ms = interval * 1000;
  storeVariable("status_interval_ms", status_interval_ms);
  return true;
}

// Handle incoming MQTT message /homie/unit-name/power/timer-interval-s/set <- int
bool timerIntervalHandler(const HomieRange& range, const String& value) {
  int interval = value.toInt();
  if (interval == 0) return false;
  outlet_timer_s = interval;
  storeVariable("outlet_timer_s", outlet_timer_s);
  return true;
}

// Define callback for status report
void reportStatus() {
  // Don't waste resources and don't loose report while network is not connected.
  if (!Homie.isConnected()) {
    return;
  }

  lastStatus_ts = millis();

  float hpa = measure_press / measure_count;
  roomNode.setProperty("temperature").setRetained(false).send(String(measure_temp / measure_count) + " *C");
  roomNode.setProperty("humidity").setRetained(false).send(String(measure_humi / measure_count) + " %");
  roomNode.setProperty("pressure").setRetained(false).send(String(hpa) + " hPa");
  roomNode.setProperty("air").setRetained(false).send(String(measure_air / measure_count) + " kOhm");
  if (CLEARED != measure_quality) {
    uint8_t q = (uint8_t) (measure_quality / measure_count);
    roomNode.setProperty("quality").setRetained(false).send(String(q) + " %");
    if (q < EVOC_TRSH) {
      // LED blinking and additional report
      if (!flag_air_alerted) {
        roomNode.setProperty("status").setRetained(false).send("Alert: bad air quality " + String(q) + " %");
        flag_air_alerted = true;
      }
      expTimer.attach(0.5, blinkLED);
    } else {
      if (expTimer.active()) expTimer.detach();
      flag_air_alerted = false;
    }
  }

  if (CLEARED_U == forecast_ts) {
    forecast_ts = millis();
    last_hpa = hpa;
  } else if (forecast_ts + HPA_COMPARE_INTERVAL_MS > millis()) {  // Compare only when enough time passed
    /*
    Tendency                  <               1006-1020 or          >
                                              1009-1023
    Steady or rising   Clearing, Cooler       Same               Fair
    Slowly falling     Rain                   Same               Fair
    Rapidly falling    Storm                  Likely rain        Cloudy, Warmer
    http://www.barometricpressureheadache.com/barometric-pressure-and-weather-conditions/
    {"No change", "Storm", "Rain", "Likely rain", "Cloudy, Warmer", "Clearing, Cooler", "Fair"}
      // 0.22 hpa/hour = slow rate
      // 0.17 hpa/hour = steady
      // 0.6 hpa/hour = fast
      or
      0.75 = fast
      0.25 = slow
    */
    forecast_ts = millis();
    float fcst_diff = hpa - last_hpa;
    last_hpa = hpa;
    if (fcst_diff < -0.75) {  // Rapidly falling
      if (hpa < 1006.0) i_forecast = 1;
      else if (hpa > 1020.0) i_forecast = 4;
      else i_forecast = 3;
    } else if (fcst_diff < -0.25) {  // Slowly falling
      if (hpa < 1006.0) i_forecast = 2;
      else i_forecast = (hpa > 1020.0)?6:0;
    } else { // Steady or rising
      if (hpa < 1006.0) i_forecast = 5;
      else i_forecast = (hpa > 1020.0)?6:0;
    }

    roomNode.setProperty("forecast").setRetained(false).send(forecast_sky[i_forecast]);
    // roomNode.setProperty("status").setRetained(false).send("fcst_diff=" + String(fcst_diff)+",last_hpa=" + String(last_hpa)+",hpa=" + String(hpa));
  }
  
  measure_count = CLEARED_U;
  measure_temp = CLEARED;
  measure_humi = CLEARED;
  measure_press = CLEARED;
  measure_air = CLEARED;
  measure_quality = CLEARED;
}

void debugReport() {
  FSInfo info;
  SPIFFS.info(info);
  uint32_t freeBytes = info.totalBytes - info.usedBytes;
  String str = "";
  uint32_t cnt = 0;
  Dir dir = SPIFFS.openDir(PREFIX);
  while (dir.next()) { cnt++; }
  roomNode.setProperty("status").send("Free=" + String(freeBytes) + ", Files: " + String(cnt));
}

/********************************************************************************************
 * SPIFFS functions  -- store/restore data.
 ********************************************************************************************/
bool is_enough_storage() {
  // Get space remaining.
  FSInfo info;
  SPIFFS.info(info);
  uint32_t freeBytes = info.totalBytes - info.usedBytes;

  // If less than 1k: set global file name to 0 and remove all files.
  return freeBytes > 1024;
}

void clean_storage() {
    lastFileIndex = 0;
    Dir dir = SPIFFS.openDir(PREFIX);
    while (dir.next()) {
      SPIFFS.remove(dir.fileName());
    }
    roomNode.setProperty("status").send("Flash cleaned");
}

void writeState() {
  if (!is_enough_storage()) {
    clean_storage();
  } else {
    // Increment global file name
    lastFileIndex++;
  }

  // Write global file name. File name format: /stateX_N, where X is 0 or 1, N is sequential number.
  File f = SPIFFS.open(PREFIX + (relayState?"1":"0") + "_" + String(lastFileIndex), "w");
  f.close();
  // debugReport();
}

int readState() {
  // list of all files. File name format: /stateX_N, where X is 0 or 1, N is sequential number.
  uint32_t index = 0;
  Dir dir = SPIFFS.openDir(PREFIX);
  while (dir.next()) {
      index = dir.fileName().substring(8).toInt();
      // Extract largest number
      if (index > lastFileIndex) lastFileIndex = index;
  }
  // Return 1 (on) if no files found (Light on when very first boot and not toggled yet)
  if (index == 0) return 1;
  // Return state of largest file name
  return (int)SPIFFS.exists(PREFIX + "1_" + String(lastFileIndex));
}

const String PREFIX_VAR = "/var_";
const long ERR_NUM = -1;
uint32_t restoreVariable(char *name) {
  // Return numeric value stored in file named as: /var_varname.
  // Return -1 if no file found or file is empty (stores nothing).
  File file = SPIFFS.open(PREFIX_VAR + String(name), "r");
  if (file) {
    String val = file.readString();
    return (val != "")?atol(val.c_str()):ERR_NUM;
  } else {
    return ERR_NUM;
  }
}

bool storeVariable(char *name, uint32_t val) {
  // Return true if value stored in file named as: /var_varname.
  // Return false if writing problem.
  if (!is_enough_storage()) clean_storage;
  File file = SPIFFS.open(PREFIX_VAR + String(name), "w");
  // Write to file and check result
  if (!file.print(String(val))) {
    roomNode.setProperty("status").send("Failed to write variable to SPIFFS: " + String(name) + ":" + String(val));
    return false;
  } else {
    return true;
  }
}


/********************************************************************************************
 * Setup and preparation functions.
 ********************************************************************************************/
void loopHandler() {
  // Check button actions
  switch (get_button_state()) {
    case BTN_CLICK: toggleRelay(); break;
    case BTN_CLICK_LONG: setRelay(true); expTimer.once(outlet_timer_s, setRelay, false); break;  // TODO: Export interval 900
    case BTN_CLICK_DBL: reportStatus(); break;
    default: break;
  }

  // Report status if just booted or timeout is ended and gas sensor is calibrated
  if ((0 >= lastStatus_ts || millis() > lastStatus_ts + status_interval_ms) && gas_base > 0.0) {
    reportStatus();
  }
}

void setup() {
  SPIFFS.begin();  // Call SPIFFS before Homie awake: the state required ASAP.
  Serial.begin(115200);
  Serial << endl << endl;

  // Operate relay directly: Homie isn't ready yet to treat ESP properly at this stage.
  int restored = readState();
  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_RELAY, restored);
  relayState = restored == 1;

  pinMode(PIN_MY_LED, OUTPUT);
  pinMode(PIN_PIR, INPUT);
  // pinMode(PIN_BUTTON, INPUT_PULLUP);
  // debouncer.attach(PIN_BUTTON);
  // debouncer.interval(20);
  attachInterrupt(digitalPinToInterrupt(PIN_BUTTON), handleBtnDownInt, FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_PIR), handlePir, RISING);

  Homie_setFirmware("room-sensor", "1.0.7");
  // Homie_setBrand("shm");  // homie ???
  // Homie.setResetTrigger(PIN_BUTTON, LOW, 5000);  // Standard 10sec

  // Prohibit the parameters to be set from MQTT broker
  roomNode.advertise("pir");
  roomNode.advertise("temperature");
  roomNode.advertise("humidity");
  roomNode.advertise("pressure");
  roomNode.advertise("air");
  roomNode.advertise("quality");
  roomNode.advertise("forecast");
  roomNode.advertise("status");
  // Allow the node to publish over MQTT. Allow the parameter "on" to be set from MQTT broker, i.e. allow remote control
  roomNode.advertise("on").settable(lightOnHandler);
  roomNode.advertise("status-interval-s").settable(statusIntervalHandler);
  roomNode.advertise("timer-interval-s").settable(timerIntervalHandler);
  
  Homie.setLoopFunction(loopHandler);

  Homie.setup();
  // If no interval stored yet, keep a default value. Else restore from Flash.
  status_interval_ms = (restoreVariable("status_interval_ms") == ERR_NUM)?status_interval_ms:restoreVariable("status_interval_ms");
  Homie.getLogger() << "status_interval_ms: " << String(status_interval_ms) << endl;
  outlet_timer_s = (restoreVariable("outlet_timer_s") == ERR_NUM)?outlet_timer_s:restoreVariable("outlet_timer_s");
  Homie.getLogger() << "outlet_timer_s: " << String(outlet_timer_s) << endl;

  // Run following after Homie.setup() since they use Homie infrastructure.
  if (!bme.begin()) {
    Serial.println("Could not find a valid BME680 sensor, check wiring!");
  }

  // // Set up oversampling and filter initialization - not needed, done on begin()
  // bme.setTemperatureOversampling(BME680_OS_8X);
  // bme.setHumidityOversampling(BME680_OS_2X);
  // bme.setPressureOversampling(BME680_OS_4X);
  // bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
  // bme.setGasHeater(320, 150); // 320*C for 150 ms

  measureTimer.attach(5.0, calibrate);
  expTimer.once(30.0, debugReport);
}

void loop() {
  Homie.loop();
}
