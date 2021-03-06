#include <Homie.h>
#include <FastLED.h>
#include <cppQueue.h>
#include <EasyButton.h>

#define FW_VER       "1.0.1" 
// #define DEBUG

#define LED_COUNT    1
#define PIN_BTN      0
#define PIN_LED      2
#define PIN_IR_LED   16
#define PIN_IR_DET   A0
#define PIN_PUMP     14
#define PIN_REED_1   4
#define PIN_REED_2   5
#define PIN_REED_3   12
#define PIN_REED_4   13
#define MLPS         35   // Pump throughput in Liters Per Second. ~150L/h = 2.5L/min = 40mL/s - 5mL/s for pet drinking.
#define MARK_L_COEFF 300  // 300 ml per mark
#define CLEARED     -10
#define UCLEARED     0
#define IR_AVERAGING 8

/* Struct for Queueing of postponed MQTT messages. Needed to gather messages while Wifi isn't connected yet. */
typedef struct strRec {
	char	topic[10];
	char	msg[90];
} Rec;
cppQueue msg_q(sizeof(Rec), 7, FIFO, true); 

const String PREFIX_VAR = "/var_";
const long ERR_NUM = -1L;

HomieNode petard_node("pf1", "petard", "fountain");

EasyButton button_ctr(PIN_BTN);
CRGBArray<LED_COUNT> leds;

Ticker led_timer;               // Postpone the LED off
Ticker level_timer;             // Run water level check periodically
Ticker filtered_timer;          // Delays filtered water write
Ticker ir_timer;                // Runs IR distance detector
Ticker pump_timer;
Ticker water_report_lock_timer; // Avoid too many reports about thirsty pet when no water.

/*
IR detector returns raw value of ADC from 0 to 1023. This represents scale up to 1V, downscaled from original 2V max.
The raw signal value depends by 2 factors:
- Ambient luminosity - changes slowly and constantly;
- Returned IR light from object proximity - changes suddenly and for short time.
Therefore pet is close if momentary >> ambient * sensitivity
Ambient is an average of few recent measures.
*/
int ir_avg = CLEARED;
uint16_t sensitivity = 32;  // Multiplier above regular distance detection level, considered to be 'close'. Read/write to EEPROM
// Sensitivity is integer for ability to fine tuning. Threshold = sqrt(ir_avg)*sensitivity.
// Example:
// Average = 300.
// Sensitivity = 32.
// Threshold = sqrt(average) * sensitivity = 554.
// If raw IR reading > expected then pet is close.

uint16_t replace_filter_every_l = 250;   // Read/write to EEPROM
uint32_t pump_start_ms = UCLEARED;       // Register when pump started.
uint8_t prev_level_decoded = 1;  // 1..7. 255 is error
uint32_t filtered_ml = UCLEARED;

uint_fast8_t brightness = 20;       // 0..255  ?
/*
Reed # |     Decoded
0	1	2	3	raw_level	res_level
0	0	0	0	  0	        255
1	0	0	0	  1	        1
1	1	0	0	  3	        2
1	1	1	0	  7	        2
0	1	0	0	  2	        3
0	1	1	0	  6	        4
0	1	1	1	  14	      4
0	0	1	0	  4	        5
0	0	1	1	  12	      6
0	0	0	1	  8	        7
*/
std::map<uint8_t,uint8_t> level_map = {{1,1}, {3,2}, {7,2}, {2,3}, {6,4}, {14,4}, {4,5}, {12,6}, {8,7}};
std::map<uint8_t,uint8_t>::iterator it;
bool is_level_changed = false;
bool is_alert_allowed = true;        // Shoot only 1 alert until acknowledge by button
uint8_t is_filter_new = 1;           // Avoid reporting "Replace filter" when filter just replaced and (total_liters % liters_to_replace) is still 0. Keep in EEPROM.
const String repl_filt_msg = "Replace filter and long click on the button";
bool is_no_water_report_locked = false;

/***********************************************************************
 *                   UTILITY / SPECIFIC FUNCTIONS                      *
 **********************************************************************/
uint32_t readFreeSpace() {
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
    petard_node.setProperty("status").setRetained(false).send(">> " + String(name) + ": " + String(val));
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
  uint32_t free_bytes = readFreeSpace();
  if (free_bytes <= 1024) {
    Rec r = {"alert", "Low storage space is free on SPIFF."};
    msg_q.push(&r);
  }

  uint32_t restored = restoreVariable(name);
  if ((restored == val) && (restored != ERR_NUM)) return true;  // Avoid wearout

  File file = SPIFFS.open(PREFIX_VAR + String(name), "w");
  int res = file.print(String(val));
  file.close();
  if (!res) {
    petard_node.setProperty("alert").setRetained(false).send("Failed to write variable to SPIFFS: " + String(name) + "=" + String(val));
    return false;
  } 
  return true;
} 

void writeFilteredML() {
  storeVariable("filtered_ml", filtered_ml);
}

void ls() {
    Dir dir = SPIFFS.openDir("/");
    while (dir.next()) {
      petard_node.setProperty("ls").setRetained(false).send(dir.fileName() + " / " + dir.fileSize());
    }
}

void readFile(File file) {
  String content = "";
  while (file.available()) content += char(file.read());
  file.close();
  petard_node.setProperty("cat").setRetained(false).send(content);
}

void cat(String f_name = "") {
    if (f_name != "") {
        File file = SPIFFS.open(f_name, "r");
        if (!file) {
          petard_node.setProperty("cat").setRetained(false).send("File not found: " + f_name);
          return;
        }
        readFile(file);
        return;
    }
    Dir dir = SPIFFS.openDir("/");
    while (dir.next()) {
      File file = SPIFFS.open(dir.fileName(), "r");
      petard_node.setProperty("cat").setRetained(false).send(dir.fileName());
      readFile(file);
    }
}

/* Wait and let background processes to run. 
wait with millis() works bad:
1. It's not precise as micros();
2. It's too easy to hang the MCU with long delay.*/
void softDelay(int us) {
  ulong end = micros() + us;
  while(micros() < end) ;
} 

String f2str(float_t in, int16_t precision) {
  char res[precision + 4];
  dtostrf(in, precision + 3, precision, res);
  return res;
}

uint16_t getDistanceThreshold() {
  return (ir_avg < 100)?600:(int)(sqrt(ir_avg) * (float)sensitivity);
}


/***********************************************************************
 *                  HARDWARE RELATED FUNCTIONS                         *
 **********************************************************************/
void blinkRedLed(bool on = true) {
  leds[0].setRGB((on)?255:0,0,0);
  LEDS.show();
  level_timer.once_ms(500, blinkRedLed, !on);
}

uint16_t readAdc(int times) {
  // read ADC conversions ar max possible rate and return average.
  ulong sum = 0UL;
  for (int i = 0; i< times; i++) {
    sum += analogRead(A0);
    softDelay(10);  // 5us is max possible rate, taking 10 us for safety.
  }
  return sum/times;
}

void pumpOn(int distance = CLEARED) {
  // Start water for 10 sec and override
  if (! pump_timer.active()) {
    pump_start_ms = millis();  // Start running water count time for filter expiration
    petard_node.setProperty("status").setRetained(false).send("Pet detected at distance:" + String(distance) +
                ", threshold:" + String(getDistanceThreshold()) + ", average IR:" + String(ir_avg)); 
    petard_node.setProperty("pump").setRetained(false).send("1");
    digitalWrite(PIN_PUMP, HIGH);
  }
  pump_timer.once(10.0, pumpOff);
}

void pumpOff() {
  pump_timer.detach();  // Workaround for bug in Ticker: after once() completed the timer is still active.
  digitalWrite(PIN_PUMP, LOW);
  uint32_t last_filtered_ml = MLPS * (millis() - pump_start_ms) / 1000;
  filtered_ml += last_filtered_ml;
  filtered_timer.once(3600.0, writeFilteredML);
  if ((replace_filter_every_l < (filtered_ml / 1000)) && (is_filter_new == 1)) {
    is_filter_new = 0;
    storeVariable("is_filter_new", is_filter_new);
    singleAlertUntilBtnAck(repl_filt_msg);
  }
  petard_node.setProperty("status").setRetained(false).send("Filtered " + String(last_filtered_ml) + " ml");
  petard_node.setProperty("pump").setRetained(false).send("0");
  reportStatus();
}

bool resetFilter() {
    // Protect filter reset counter in occasional long button press.
  if (is_filter_new == 0) {
    is_filter_new = 1;
    storeVariable("is_filter_new", is_filter_new);
    filtered_ml = 0;
    writeFilteredML();
    petard_node.setProperty("status").setRetained(false).send("Filter replacement accepted");
    return true;
  }
  return false;
}

// Use for accept filter replacement
void onLongPressBtn() {
  leds[0] = CRGB::WhiteSmoke;
  LEDS.show(); 
  is_alert_allowed = true;
  led_timer.once(3.0, measureWaterLevel);
  resetFilter();
}

void onPressBtn() {
  brightness += 85;
  LEDS.setBrightness(brightness);
  LEDS.show(); 
  is_alert_allowed = true;
  // Toggle pump
  if (pump_timer.active()) pumpOff();
  else pumpOn();
}

ICACHE_RAM_ATTR void measureWaterLevel() {
  // Stop blinking if water level changed.
  if (level_timer.active()) level_timer.detach();
  uint8_t level_raw = !digitalRead(PIN_REED_1);
  level_raw += !digitalRead(PIN_REED_2) << 1;
  level_raw += !digitalRead(PIN_REED_3) << 2;
  level_raw += !digitalRead(PIN_REED_4) << 3;

  uint8_t level_decoded = 255;
  it = level_map.find(level_raw);
  if (it != level_map.end()) level_decoded = it->second;
  else {
    singleAlertUntilBtnAck("Unknown reed switch reading");
    level_timer.once_ms(500, blinkRedLed, true);
  }

  // Level changed => update global variable.
  is_level_changed = (level_decoded != prev_level_decoded);
  if (is_level_changed) reportStatus();

  switch (level_decoded)
  {
  case 1:
    leds[0] = CRGB::Red;
    singleAlertUntilLevelChange("Add water now");
    break;
  
  case 2:
    leds[0] = CRGB::Orange;
    // Alert only when water goes down
    if ((level_decoded < prev_level_decoded) && (prev_level_decoded != 3)) singleAlertUntilLevelChange("Add water soon");
    break;

  case 3:
    leds[0] = CRGB::Yellow;
    // Alert only when water goes down
    if (level_decoded < prev_level_decoded) singleAlertUntilLevelChange("Add water soon");
    break;
  
  case 4:
    leds[0] = CRGB::Green;
    break;

  case 5:
    leds[0] = CRGB::Turquoise;
    break;
  
  case 6:
  case 7:
    leds[0] = CRGB::Blue;
    break;
  
  default:
    return;  // Treat this in blinkLed function.
    break;
  } 
  prev_level_decoded = level_decoded; 
  LEDS.show(); 
}

void unlockWaterReportLock() {
  is_no_water_report_locked = false;
  water_report_lock_timer.detach();
}

void reportPetThirsty() {
  if (is_no_water_report_locked == false) {
    petard_node.setProperty("alert").setRetained(false).send("Pet thirsty");
    is_no_water_report_locked = true;
    water_report_lock_timer.once(600.0, unlockWaterReportLock);
  }
}

uint16_t readIR() {
  // Measure the light return when IR LED is on.
  digitalWrite(PIN_IR_LED, HIGH);
  softDelay(5);
  int max = readAdc(5);  // Use int because in certain scenario dark measure is greater than IR lit one.
  // Measure the light return when IR LED is off - ambient light.
  digitalWrite(PIN_IR_LED, LOW);
  softDelay(5);
  int min = readAdc(5);
  return (uint16_t)abs(max-min);
}


// TODO: remove
uint16_t readDarkness() {
  // Read low levels of IR sensor without IR LED for ling time. Seek for range of fluctuations.
  uint16_t min = 1024;
  uint16_t max = 0;
  for(int i=0; i<70; i++) {
    uint16_t r = analogRead(PIN_IR_DET);
    if (r > max) max = r;
    if (r < min) min = r;
    softDelay(5);  // Wait for ADC refresh
  }
  return max-min;
}

void drive_ir() {
  uint16_t ir_reading = readIR();
  // Update average or set the very first reading ad initial average
  ir_avg = (ir_avg != CLEARED)?(ir_avg * IR_AVERAGING + ir_reading) / (IR_AVERAGING + 1):ir_reading;

  // pet close => run pump if exceeded expected threshold and minimal sanity level and skip ir_avg update
  if ((ir_reading > getDistanceThreshold()) && (ir_reading > 300) && (ir_avg != CLEARED)) {   
    // If was not water and close distance => user fills water or pet came for drink...
    // Check if water level changing.
    // If changing in short time, don't shoot "Thirsty" alert and postpone next IR reading by 20 sec.
    // If no water even after 20 sec of delay, send “Pet wants water”. Return.
    if ((prev_level_decoded > 1) && (water_report_lock_timer.active())) {
      unlockWaterReportLock();
    }
    if (prev_level_decoded == 1) {
      if (!water_report_lock_timer.active()) {
        water_report_lock_timer.once(20.0, reportPetThirsty);
      }
      return;
    }

    ir_timer.once(9.0, drive_ir);   // Reschedule next reading later.
    pumpOn(ir_reading);
  } else {
    ir_timer.once_ms(500, drive_ir);   // Reschedule next reading fast.
  }
}


/***********************************************************************
 *                     MQTT RELATED FUNCTIONS                          *
 **********************************************************************/

// homie/petard/pf1/factory-reset/set true - set all settings to factory defaults
bool factoryResetHandler(const HomieRange& range, const String& value) {
  if (value != "true") return false;

  bool res = true;    
  Dir dir = SPIFFS.openDir("/");
  while (dir.next()) {
    bool r = SPIFFS.remove(dir.fileName());
    if (res && !r) res = r;  // Keep any negative result
  }
  petard_node.setProperty("status").setRetained(false).send("Configuration was" + String((res)?"":" not") + " removed");

  Homie.reboot();
  return true; 
}

// homie/petard/pf1/reset/set true - reboot remotely
bool resetHandler(const HomieRange& range, const String& value) {
  if (value != "true") return false;

  Homie.reboot();
  return true; 
}

// homie/petard/pf1/ls/set true - list all files to MQTT
bool lsHandler(const HomieRange& range, const String& value) {
  if (value != "true") return false;

  ls();
  return true; 
}

// homie/petard/pf1/cat/set true - print all files to MQTT
bool catHandler(const HomieRange& range, const String& value) {
  if (value == "true") cat();
  else cat(value);
  return true; 
}

void reportSensitivity() {
  petard_node.setProperty("sensitivity").setRetained(true).send(String(sensitivity));
}

void reportFilteredWater() {
  petard_node.setProperty("filter-service-l").setRetained(true).send(String(replace_filter_every_l));
}

void reportStatus() {
  /* {
      filtered_l: x,
      remaining_filter_service_l: x,
      water_level_l: x,
      water_level_t: x,
      replace_filter_every_l: x,
      ir_avg: x,
      distanceThreshold: x,
  }
  */
  reportSensitivity();
  reportFilteredWater();
  petard_node.setProperty("status").setRetained(false).send(
    "{\"filtered_l\":" + String(filtered_ml / 1000) + 
    ",\"remaining_filter_service_l\":" + String(replace_filter_every_l - filtered_ml / 1000) + 
    ",\"water_level_l\":" + f2str((float)(prev_level_decoded) * MARK_L_COEFF / 1000.0, 1) + 
    ",\"water_level_t\":" + String(prev_level_decoded) + 
    ",\"replace_filter_every_l\":" + String(replace_filter_every_l) + 
    ",\"ir_avg\":" + String(ir_avg) + 
    ",\"distanceThreshold\":" + String(getDistanceThreshold()) + 
    "}");
}

// homie/petard/pf1/status/set anything - ask how long to wait till next run. Response with MQTT
bool litersToReplaceHandler(const HomieRange& range, const String& value) { 
  reportStatus();
  return true;
}

// homie/petard/pf1/filter-reset/set true - reset filter counter to MQTT
bool filterResetHandler(const HomieRange& range, const String& value) {
  if (value != "true") return false;
  resetFilter();
  reportStatus();
  return true; 
}

// homie/petard/pf1/filter-service-l/set Liters. How much refilled water can the filter treat.
bool filterServiceLHandler(const HomieRange& range, const String& value) {
  long val = value.toInt();
  if (val <= 0) return false;
  storeVariable("replace_filter_every_l", val);
  replace_filter_every_l = val;
  reportStatus();
  return true;
}

void singleAlertUntilBtnAck(String msg) {
  // Report how many liters are remaining to replace the filter
  if (is_alert_allowed) {
    if (petard_node.setProperty("alert").setRetained(false).send(msg) != 0) is_alert_allowed = false;
  }
}

void singleAlertUntilLevelChange(String msg) {
  // Report to fill the water
  if (is_level_changed) {
    petard_node.setProperty("alert").setRetained(false).send(msg);
  }
}

// homie/petard/pf1/pump/set true/false/toggle
bool pumpHandler(const HomieRange& range, const String& value) {
  // DEBUG FEATURE, DO NOT USE.
  if (value == "toggle") {
    digitalWrite(PIN_PUMP, ! digitalRead(PIN_PUMP));
    return true;
  }

  if (value != "true" && value != "false") return false;

  digitalWrite(PIN_PUMP, (value == "true"));
  return true;
}

// homie/petard/pf1/read-ir/set anything
bool irHandler(const HomieRange& range, const String& value) {
  ir_timer.detach();
  softDelay(500);
  uint16_t ir_reading = readIR();
  ir_avg = (ir_avg != CLEARED)?(ir_avg * IR_AVERAGING + ir_reading) / (IR_AVERAGING + 1):ir_reading;
  petard_node.setProperty("read-ir").setRetained(false).send(String(ir_reading)+","+String(ir_avg)+","+String(readDarkness()));

  ir_timer.once_ms(500, drive_ir);
  return true;
}

// homie/petard/pf1/rm/set file_name
bool removeHandler(const HomieRange& range, const String& value) {
  String res = (SPIFFS.remove(value))?"":" not";
  petard_node.setProperty("rm").setRetained(false).send("File " + value + " was" + res + " removed");
}

// // homie/petard/pf1/debug/set true/false/toggle
// bool debugHandler(const HomieRange& range, const String& value) {
//   if (value != "true" && value != "false") return false;
//   debug = (value == "true");
//   return true;
// }

// homie/petard/pf1/sensitivity/set <number>
bool sensitivityHandler(const HomieRange& range, const String& value) {
  long val = value.toInt();
  if (val <= 0) return false;
  storeVariable("sensitivity", val);
  sensitivity = val;
  reportStatus();
  return true;
}



/***********************************************************************
 *                      SKETCH BASE FUNCTIONS                          *
 **********************************************************************/
void loopHandler() {
  button_ctr.read();

  while (!msg_q.isEmpty() && Homie.isConnected()) {
    Rec r;
    msg_q.pop(&r);
    petard_node.setProperty(r.topic).setRetained(false).send(r.msg);
#ifdef DEBUG
    Homie.getLogger() << r.topic << ' >> ' << r.msg << endl;
#endif  
  }  

  // TODO: Add alert if pet don't drinks too long
}

void setupHandler() {
  measureWaterLevel();  // Update the LED status
  reportStatus();
  // Remind to replace filter if needed after reboot.
  if (replace_filter_every_l < (filtered_ml / 1000)) {
    singleAlertUntilBtnAck(repl_filt_msg);
  }
}

/***********************************************************************
 *                      PRINCIPLE OF OPERATION                         *
 * 
 * On boot read from EEPROM:
 *    - filter_service_l
 *    - filtered_ml
 *    - last_kept_reed_mark
 * In loop:
 *  - Read prev_level_decoded
 *  - Set LED color by prev_level_decoded
 *  - If prev_level_decoded > last_kept_reed_mark then start fill_timer. Update the added amount of water after the progress stops. Write to EEPROM + report ???
 *  - If prev_level_decoded < last_kept_reed_mark then EEPROM + report
 *  - If filtered_ml/1000 > filter_service_l then send alert once
 *  - If prev_level_decoded == Low then send alert once
 **********************************************************************/
void setup() {
  // SPIFFS.begin();  // Call SPIFFS before Homie awake: the state required ASAP.
  Serial.begin(115200);
  Serial << endl << endl;
  SPIFFS.begin();

  LEDS.setBrightness(brightness);
  LEDS.addLeds<WS2811, PIN_LED, GRB>(leds, LED_COUNT);
  leds[0].setRGB(100,100,100);
  LEDS.show(); 

  button_ctr.begin();
  button_ctr.onPressedFor(400, onLongPressBtn);
  button_ctr.onPressed(onPressBtn);

  pinMode(PIN_BTN, INPUT_PULLUP);
  pinMode(PIN_REED_1, INPUT_PULLUP);
  pinMode(PIN_REED_2, INPUT_PULLUP);
  pinMode(PIN_REED_3, INPUT_PULLUP);
  pinMode(PIN_REED_4, INPUT_PULLUP);
  pinMode(PIN_IR_LED, OUTPUT);
  pinMode(PIN_PUMP, OUTPUT);

  attachInterrupt(digitalPinToInterrupt(PIN_REED_1), measureWaterLevel, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_REED_2), measureWaterLevel, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_REED_3), measureWaterLevel, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_REED_4), measureWaterLevel, CHANGE);

  Homie_setFirmware("petard", FW_VER);
  // Homie_setBrand("shm");  // homie ???
  // Homie.setResetTrigger(PIN_BUTTON, LOW, 5000);
  // Homie.disableResetTrigger();
  Homie.disableLedFeedback();  // Workaround for 'Cannot mount filesystem' bug.

  // Prohibit the parameters to be set from MQTT broker
  petard_node.advertise("status").settable(litersToReplaceHandler);         // Ask how long is till filter replacement.
  petard_node.advertise("factory-reset").settable(factoryResetHandler);
  petard_node.advertise("reset").settable(resetHandler);
  petard_node.advertise("cat").settable(catHandler);
  petard_node.advertise("ls").settable(lsHandler);
  petard_node.advertise("rm").settable(removeHandler);
  petard_node.advertise("sensitivity").settable(sensitivityHandler);
  petard_node.advertise("pump").settable(pumpHandler);
  petard_node.advertise("read-ir").settable(irHandler);
  petard_node.advertise("alert");
  petard_node.advertise("filter-service-l").settable(filterServiceLHandler);  // Set the filter service time.
  petard_node.advertise("filter-reset").settable(filterResetHandler); 
  
  Homie.setSetupFunction(setupHandler).setLoopFunction(loopHandler);
  Homie.setup();

  replace_filter_every_l = restoreVariable("replace_filter_every_l", replace_filter_every_l);
  sensitivity = restoreVariable("sensitivity", sensitivity);
  is_filter_new = restoreVariable("is_filter_new", is_filter_new);
  filtered_ml = restoreVariable("filtered_ml", filtered_ml);
  ir_timer.once(3.0, drive_ir);

  // digitalWrite(PIN_IR_LED, HIGH);
}

void loop() {
  Homie.loop();
}
