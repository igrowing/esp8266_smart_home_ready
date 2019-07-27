/******************************************************************************************
 * She-bear: automated water flow meter and control
 * (2019) iGrowing
 ******************************************************************************************/
#include <Homie.h>
#include <cppQueue.h>
#include "FaBoMotor_DRV8830.h"

// #define DEBUG

//    -- Globals --
/* Struct for Queueing of postponed MQTT messages. Needed to gather messages while Wifi isn't connected yet. */
typedef struct strRec {
	char	topic[10];
	char	msg[90];
} Rec;
Queue msg_q(sizeof(Rec), 5, FIFO, true);

#define RESET_UINT        0
#define PIN_LDO           14
#define SLEEP_US          3600000000
uint32_t boot_reason      = RESET_UINT;  // 6 = power on, 5 = awake
uint32_t boot_time        = RESET_UINT; 
volatile short vbat_raw   = RESET_UINT;
#define VBAT_LOW_TRSH     790            // 3.28V
Ticker power_timer;                      // Use for powering off or sleep

/*******  Valve related variables    *******/
bool valve_state;
const String PREFIX = "/state";
volatile uint32_t lastFileIndex = RESET_UINT;
Ticker valve_timer;
#define DRV8830_SLAVE_ADDRESS  0xC8      // A0 open, A1 open
FaBoMotor valve;

/*******  Flow related variables    *******/
#define PIN_FLOW              12
#define POLL_PERIOD_S         2.0
#define FLOW_COEFF            57.0       //  450/7.8 from orchids. Use as liters=ticks/FLOW_COEFF
#define FLOW_MIN_VALID_TICKS  1          // Filter out noise
uint_fast32_t flow_ticks = RESET_UINT;
uint32_t flow_last_ticks = RESET_UINT;
uint32_t flow_started_ts = RESET_UINT;
uint32_t flow_report_ts = RESET_UINT;
uint32_t ticks_denom = FLOW_COEFF;
uint32_t flow_last_blink_l = RESET_UINT;
uint32_t blink_liters = 5;
uint32_t flow_max_liters = 5000;         // 5000 liters
uint32_t flow_max_duration_ms = 7200000; // 2 hours
bool flow_half_alert_reported = false;
bool flow_full_alert_reported = false;
Ticker flow_timer;

/******  Button related variables    *******/
#define PIN_BUTTON  0
Bounce debouncer = Bounce(); // Bounce is built into Homie, so you can use it without including it first

/**** LED related variables  *****/
#define PIN_LED     2
// #define LED_ACTIVE_HIGH
#define LED_ACITVE_LOW
Ticker delay_timer; // Controls led_timer (supervisor)
Ticker led_timer;   // Controls LED
#ifdef LED_ACTIVE_HIGH
bool led_state = LOW;
#else
bool led_state = HIGH;
#endif
uint_fast32_t led_off_ms = 0;
uint_fast32_t led_on_ms = 0;

HomieNode shiberNode("shiber", "valve");  // name, type

/******************************************************************************************
 *     HELPERS
 ******************************************************************************************/
const String PREFIX_VAR = "/var_";
const long ERR_NUM = -1;
uint32_t restoreVariable(char *name) {
  // Return numeric value stored in file named as: /var_varname.
  // Return -1 if no file found or file is empty (stores nothing).
  File file = SPIFFS.open(PREFIX_VAR + String(name), "r");
  if (file) {
    String val = file.readString();
#ifdef DEBUG
    Serial.println(">> " + String(name) + ": " + val);
    Serial.flush();
#endif
    return (val != "")?atol(val.c_str()):ERR_NUM;
  } else {
    Homie.getLogger() << "Can't restore: " << name << endl;
    return ERR_NUM;
  }
}

/* Return true if value stored in file named as: /var_varname.
*  Return false if writing problem.
*/
bool storeVariable(char *name, uint32_t val) {
  File file = SPIFFS.open(PREFIX_VAR + String(name), "w");
  // TODO: Add free space validation like in writeState
  if (!file.print(String(val))) {
    shiberNode.setProperty("alert").setRetained(false).send("Failed to write variable to SPIFFS: " + String(name) + "=" + String(val));
    return false;
  } else {
    return true;
  }
}
 
String f2str(float_t in, int16_t precision) {
  char res[precision + 4];
  dtostrf(in, precision + 3, precision, res);
  return res;
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
#ifdef DEBUG
  Serial.println(">> readState " + String(index));
  Serial.flush();
#endif
  // Return 1 (on) if no files found (Light on when very first boot and not toggled yet)
  if (index == 0) return 1;
  // Return state of largest file name
  return (int)SPIFFS.exists(PREFIX + "1_" + String(lastFileIndex));
}

void writeState() {
  // Get space remaining.
  FSInfo info;
  SPIFFS.info(info);
  uint32_t freeBytes = info.totalBytes - info.usedBytes;

  // If less than 1k: set global file name to 0 and remove all files.
  if (freeBytes <= 1024) {
    lastFileIndex = 0;
    Dir dir = SPIFFS.openDir(PREFIX);
    while (dir.next()) {
      SPIFFS.remove(dir.fileName());
    }
    shiberNode.setProperty("status").setRetained(false).send("Flash cleaned");
  } else {
    // Increment global file name
    lastFileIndex++;
  }

  // Write global file name. File name format: /stateX_N, where X is 0 or 1, N is sequential number.
  File f = SPIFFS.open(PREFIX + (valve_state?"1":"0") + "_" + String(lastFileIndex), "w");
  f.close();
}

/* Calculate volume with consideration of time the shiber was booting and not counting.
Assume constant water flow. */
float get_liters() {
  uint32_t uptime = millis();
  float uptime_liters = (float)flow_last_ticks / (float)ticks_denom;     // measured volume
  return (float)(uptime + boot_time) * uptime_liters / (float) uptime;   // extrapolated volume
}

/******************************************************************************************
 *     HARDWARE HANDLING FUNCTIONS
 ******************************************************************************************/
void get_boot_reason() {
  if (! boot_reason) {
    rst_info *myResetInfo;
    myResetInfo = ESP.getResetInfoPtr();
    boot_reason = myResetInfo->reason;
    boot_time = millis();
  }

#ifdef DEBUG
  Serial.printf(">> myResetInfo->reason %x \n", boot_reason); // reason is uint32
  Serial.println(">> getResetReason " + String(ESP.getResetReason()));
  Serial.flush();
  // String m = "boot reason " + boot_reason;
  // char m1 [m.length()];
  // m.toCharArray(m1, m.length());
  // itoa(boot_reason, m1, 10); 
  // Rec r = {"status", m1};
  // String("boot reason " + boot_reason).toCharArray(r.msg, 50);
  // msg_q.push(&r);
#endif
}

/* Send to ATTiny the short 10ms signal to go sleep mode. */
void stop_sleep_signal() {
  digitalWrite(PIN_LDO, HIGH);
  Homie.prepareToSleep();
  Homie.doDeepSleep(SLEEP_US);
  delay(10);  
}

void deep_sleep() {
  digitalWrite(PIN_LDO, LOW);
  power_timer.once(0.005, stop_sleep_signal);
}

void shutdown() {
  digitalWrite(PIN_LDO, LOW);
  delay(10);
}

void deep_sleep_or_off() {
  String msg = "{\"uptime\":" + String(millis()/1000) + ",\"liters\":" + f2str(get_liters(), 1) + ",\"br\":" + String(boot_reason) + 
                ",\"ticks\":" + String(flow_last_ticks)+ ",\"valve\":" + (valve_state ? "true" : "false");
  // No water flow and valve closed => sleep, wait for mqtt commands.
  if (!valve_state) {
    shiberNode.setProperty("status").setRetained(false).send(msg + ",\"action\":\"sleep\"}");
    power_timer.once(0.5, deep_sleep);
  } else {
    // Valve open or water flown => get ready to be awaken by another flow.
    shiberNode.setProperty("status").setRetained(false).send(msg + ",\"action\":\"shutdown\"}");
    power_timer.attach(0.5, shutdown);
  }
}

/****************   FLOW   METER   FUNCTIONS   *******************/
void flowInterrupt() {
  flow_ticks++;
}

/* Blink LED fast 10 seconds if human awaken the Shiber, i.e. no flow detected. 
*  Start flow periodic check. Get boot reason. Init. valve. Read Vbat.
*/
void delayed_init() {
  vbat_raw = analogRead(A0);
  get_boot_reason();

  if (!valve.begin()) {  // TODO: Consider moving after Homie.isConnected()
    Rec r = {"alert", "Valve not found on I2C"};
    msg_q.push(&r);
#ifdef DEBUG
    Homie.getLogger() << ">> Valve not found on I2C" << endl;
#endif
  }

  // LED on till ready for user interaction
  if (flow_ticks <= FLOW_MIN_VALID_TICKS) {
    // Not actual timer needed. Just lock the timer to signal to check_flow "the human activated state"
    power_timer.once(15.0, deep_sleep_or_off);  
#ifdef DEBUG
    Rec r = {"status", "awaken by human or after sleep, led on"};
    msg_q.push(&r);
    Homie.getLogger() << ">> Awaken by human" << endl;
#endif
  }

#ifdef DEBUG
  Rec r = {"status", "flow check start"};
  msg_q.push(&r);
#endif
  flow_timer.attach(POLL_PERIOD_S, check_flow);
}

void check_flow() {
  // Update ticks if water is still running. Report if stopped.
  if (flow_ticks == flow_last_ticks) {  // no more ticks in 2 last seconds
    if (!power_timer.active()) deep_sleep_or_off();
#ifdef DEBUG
    shiberNode.setProperty("status").setRetained(false).send("ticks not changed:"+String(flow_last_ticks));
#endif
    return;
  }

  // Was more ticks => water flows. Let counter increase. It will set power off when stops.
  power_timer.detach();
  flow_last_ticks = flow_ticks;
  // Initialise flow TS
  if (RESET_UINT == flow_started_ts && flow_last_ticks > FLOW_MIN_VALID_TICKS) flow_started_ts = millis(); 
  
  // Calculate current status
  uint32_t liters = (uint32_t)get_liters();
  uint32_t flow_time = millis() - flow_started_ts;

  // Blink LED periodically on water flow
  if (liters >= flow_last_blink_l + blink_liters) {
    flow_last_blink_l = liters;
    blink_led(10, 10);
    blink_times(1);
#ifdef DEBUG
    String msg = f2str(liters, 1) + "L in " + String(millis()/1000) + "sec";
    shiberNode.setProperty("status").setRetained(false).send(msg);
    Homie.getLogger() << msg << endl;
#endif
  }

  // Shut off on max consumption
  if (((liters > flow_max_liters) || (flow_time > flow_max_duration_ms)) && !flow_full_alert_reported) {
    reportFlowAlert("{\"reason\":\"Water is running, shutting off\",\"duration\":\"" + String(flow_time / 1000) +
                    "\",\"liters\":" + String(liters) + "\"}");
    flow_full_alert_reported = true;
    set_valve(false);
    return;
  }
  // Alert on half consumption
  if (((liters > flow_max_liters / 2.0) || (flow_time > flow_max_duration_ms / 2)) && !flow_half_alert_reported) {
    reportFlowAlert("{\"reason\":\"Water is running\",\"duration\":\"" + String(flow_time / 1000) +
                    "\",\"liters\":" + f2str(liters, 1) + "\"}");
    flow_half_alert_reported = true;
  }
}

/****************   VALVE   CONTROL   FUNCTIONS   *******************/
/* Do not call this, finish control of valve */
void _valve_stop() {
  valve.drive(DRV8830_STANBY, 0);
  writeState();  // Write state to SPIFFS
  reportValveStatus();
}

/* Start-stop-report valve status change */
void set_valve(const bool is_fwd) {
  if (valve_state == is_fwd) {
    reportValveStatus();
    return;
  }
  valve_state = is_fwd;
  valve.drive((is_fwd)?DRV8830_FORWARD:DRV8830_REVERSE, DRV8830_SPEED_MAX);
  valve_timer.once(0.3, _valve_stop);
}

// Handle button click to the switch.
void toggleValve() {
  set_valve(!valve_state); 
}

/****************   LED   CONTROL   FUNCTIONS   *******************/
/* Do not call this, direct control of LED */
void _set_led_state(bool state) {
  led_state = state;
  uint32_t time_ms = (led_state?led_on_ms:led_off_ms);
#ifdef LED_ACTIVE_HIGH
  digitalWrite(PIN_LED, led_state);
#else
  digitalWrite(PIN_LED, led_state?LOW:HIGH);
#endif
  led_timer.once_ms(time_ms, _set_led_state, !led_state);

}

/* Start blinking with on and off times in ms*/
void blink_led(int on_ms, int off_ms) {
  stop_led();
  led_off_ms = off_ms;
  led_on_ms = on_ms;
  _set_led_state(HIGH);
  led_timer.once_ms(on_ms, _set_led_state, (bool)LOW);
}

/* Stop blinking immediately */
void stop_led() {
  led_timer.detach();
#ifdef LED_ACTIVE_HIGH
  led_state = LOW;
#else
  led_state = HIGH;
#endif
  digitalWrite(PIN_LED, led_state);
}

/* Stop blinking after X ms. Call this after blink_led(). */
void blink_for(uint32_t time_ms) {
  delay_timer.once_ms(time_ms, stop_led);
}

/* Stop blinking after X blinks. Call this after blink_led(). */
void blink_times(int times) {
  blink_for((led_on_ms + led_off_ms) * ( times + 1));
}

/******************************************************************************************
 *     COMMUNICATION FUNCTIONS and HANDLERS
 *   DO NOT use underscore _ in MQTT topics and properties. (Can use in messages thus).
 ******************************************************************************************/
void reportValveStatus() {
  shiberNode.setProperty("valve").setRetained(false).send(valve_state ? "true" : "false");
#ifdef DEBUG
  Homie.getLogger() << ">> Valve is " << (valve_state ? "on" : "off") << endl;
#endif
}

void reportBattery() {
  shiberNode.setProperty("battery").setRetained(false).send(f2str((float) vbat_raw * 0.0041, 1) + " V");
#ifdef DEBUG
  Homie.getLogger() << ">> Vbat = " << f2str((float) vbat_raw * 0.0041, 1) << endl;
#endif
}

void reportFlowAlert(String msg) {
  shiberNode.setProperty("alert").setRetained(false).send(msg);
#ifdef DEBUG
  Homie.getLogger() << ">> Alert: " << msg << endl;
#endif
}

// homie/shiber-01/shiber/power/set false
/* For debug: Force power down the shiber*/
bool powerHandler(const HomieRange& range, const String& value) {
  if (value != "false") return false;

  shutdown();
  return true;
}

// homie/shiber-01/shiber/valve/set true
bool valveHandler(const HomieRange& range, const String& value) {
  if (value == "toggle") {
    toggleValve();
    return true;
  }

  if (value != "true" && value != "false") return false;

  set_valve(value == "true");
  return true;
}

// homie/shiber-01/shiber/max-liters/set 123
bool maxLitersHandler(const HomieRange& range, const String& value) {
  uint32_t val = atoi(value.c_str());
  if (RESET_UINT == val) {
    shiberNode.setProperty("alert").setRetained(false).send("Invalid value for max-liters:" + String(value) + ". Valid: Numbers only.");
    return false;
  }

  if (flow_max_liters != val) {  // Avoid Flash wear out
    flow_max_liters = val;
    storeVariable("flow_max_liters", flow_max_liters); 
  }
  
  shiberNode.setProperty("max-liters").setRetained(false).send(String(flow_max_liters));  // ack back
  return true;
}
 
// homie/shiber-01/shiber/max-seconds/set 123
bool maxSecondsHandler(const HomieRange& range, const String& value) {
  uint32_t val = atoi(value.c_str()) * 1000U;
  if (RESET_UINT == val) {
    shiberNode.setProperty("alert").setRetained(false).send("Invalid value for max-seconds:" + String(value) + ". Valid: Numbers only.");
    return false;
  }

  if (flow_max_duration_ms != val) {  // Avoid Flash wear out
    flow_max_duration_ms = val;
    storeVariable("flow_max_duration_ms", flow_max_duration_ms); 
    }
    
  shiberNode.setProperty("max-seconds").setRetained(false).send(String(flow_max_duration_ms / 1000));  // ack back
  return true;
}

// homie/shiber-01/shiber/ticks-denom/set 123
bool ticksDenomHandler(const HomieRange& range, const String& value) {
  uint32_t val = atoi(value.c_str());
  if (RESET_UINT == val) {
    shiberNode.setProperty("alert").setRetained(false).send("Invalid value for ticks-denom:" + String(value) + ". Valid: Numbers only.");
    return false;
  }

  if (ticks_denom != val) {  // Avoid Flash wear out
    ticks_denom = val;
    storeVariable("ticks_denom", ticks_denom); 
  }
  shiberNode.setProperty("ticks-denom").setRetained(false).send(String(ticks_denom));  // ack back
  return true;
}

// homie/shiber-01/shiber/blink-liters/set 123
bool blinkLitersHandler(const HomieRange& range, const String& value) {
  uint32_t val = atoi(value.c_str());
  if (RESET_UINT == val) {
    shiberNode.setProperty("alert").setRetained(false).send("Invalid value for blink-liters:" + String(value) + ". Valid: Numbers only.");
    return false;
  }

  if (blink_liters != val) {  // Avoid Flash wear out
    blink_liters = val;
    storeVariable("blink_liters", blink_liters); 
  }
  shiberNode.setProperty("blink-liters").setRetained(false).send(String(blink_liters));  // ack back
  return true;
}

/******************************************************************************************
 *     HOMIE ESSENTIALS: SETUP AND LOOP
 ******************************************************************************************/
void setupHandler() {
  // If no values stored yet, keep a default value and store it for next boot. Else restore from Flash.
  uint32_t val = restoreVariable("flow_max_duration_ms");
  if (ERR_NUM == val) {
    val = flow_max_duration_ms;
    storeVariable("flow_max_duration_ms", flow_max_duration_ms);
  }
  flow_max_duration_ms = val;
  shiberNode.setProperty("max-seconds").setRetained(false).send(String(flow_max_duration_ms / 1000));

  val = restoreVariable("flow_max_liters");
  if (ERR_NUM == val) {
    val = flow_max_liters;
    storeVariable("flow_max_liters", flow_max_liters);
  }
  flow_max_liters = val;
  shiberNode.setProperty("max-liters").setRetained(false).send(String(flow_max_liters));

  val = restoreVariable("ticks_denom");
  if (ERR_NUM == val) {
    val = ticks_denom;
    storeVariable("ticks_denom", ticks_denom);
  }
  ticks_denom = val;
  shiberNode.setProperty("ticks-denom").setRetained(false).send(String(ticks_denom));

  val = restoreVariable("blink_liters");
  if (ERR_NUM == val) {
    val = blink_liters;
    storeVariable("blink_liters", blink_liters);
  }
  blink_liters = val;
  shiberNode.setProperty("blink-liters").setRetained(false).send(String(blink_liters));

  reportBattery();
  reportValveStatus();
  if (vbat_raw <= VBAT_LOW_TRSH) {
    shiberNode.setProperty("alert").setRetained(false).send("Low battery");
  }

  // Blink as ready for user interaction
  if (flow_ticks <= FLOW_MIN_VALID_TICKS) {
    blink_led(50, 250);  
    blink_for(15000);
    power_timer.once(15.0, deep_sleep_or_off);
  }

}

void loopHandler() {
  debouncer.update();
  if (debouncer.read() == LOW) {  // LED on while button pressed 
    blink_led(200, 5);  
    blink_for(300);    
  };
  if (debouncer.rose()) {  // toggle valve and postpone power off on button release
    shiberNode.setProperty("status").setRetained(false).send("Change valve by button");
    toggleValve();
    // Confirm manual action Visually
    blink_led(100, 300);  
    blink_times(3);
    power_timer.once(15.0, deep_sleep_or_off);
  }

  while (!msg_q.isEmpty()) {
    Rec r;
    msg_q.pop(&r);
    shiberNode.setProperty(r.topic).setRetained(false).send(r.msg);
  } 
}

void setup() {
  // Hold power on
  pinMode(PIN_LDO, OUTPUT);
  digitalWrite(PIN_LDO, HIGH);
  SPIFFS.begin();  // Call SPIFFS before Homie awake: the state required ASAP.
  Serial.begin(115200);
  Serial << endl << endl;
  
  valve_state = readState();

  pinMode(PIN_BUTTON, INPUT_PULLUP);
  debouncer.attach(PIN_BUTTON);
  debouncer.interval(10);

  pinMode(PIN_FLOW, INPUT_PULLUP);
  attachInterrupt(PIN_FLOW, flowInterrupt, RISING);

  Homie_setFirmware("shiber", "1.0.4");

  Homie.setSetupFunction(setupHandler).setLoopFunction(loopHandler);
  Homie.setResetTrigger(PIN_BUTTON, LOW, 10000);
  // Homie.disableLedFeedback();
  Homie.setLedPin(PIN_LED, LOW);
  shiberNode.advertise("alert");  // Report problem asserted and deasserted
  shiberNode.advertise("status"); // json: {"uptime":32,"liters":5.3,"br":6,"ticks":362,"action":"shutdown","valve":true}
  shiberNode.advertise("battery");
  shiberNode.advertise("valve").settable(valveHandler);  // Report relay is on and off
  shiberNode.advertise("max-liters").settable(maxLitersHandler);  // Set threshold for 'close water' alert
  shiberNode.advertise("max-seconds").settable(maxSecondsHandler);  // Set threshold for 'close water' alert
  shiberNode.advertise("ticks-denom").settable(ticksDenomHandler);  // Set threshold for 'close water' alert
  shiberNode.advertise("blink-liters").settable(blinkLitersHandler);  // Set threshold for 'close water' alert
  shiberNode.advertise("power").settable(powerHandler);  // Force power down. 'false' arg only.

  flow_timer.once(2.0, delayed_init);
  Homie.setup();

}

void loop() {
  Homie.loop();
}
