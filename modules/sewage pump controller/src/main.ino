#include <Homie.h>

//   -- Constants --
const int PIN_BUTTON  = 0;  // and LED. Do not light the LED for longer than 4 sec.
// const int PIN_POWER   = 4;  // TODO: Remove. No need battery sensing.
const int PIN_RELAY   = 12;
const int PIN_TRIGGER = 14;
const int PIN_ECHO    = 13;

#define RESET_DIST      0
#define RESET_ADC       0
#define SAMPLES_ADC     250
#define SAMPLES_DIST    10
#define RMS_ON_TRSH     1000   // mA.
#define PUMP_ON_TIMEOUT 300.0  // 5 min = 5 * 60sec
#define PUMP_COOLTIME   600.0  // 10 min = 10 * 60sec
#define ADC_READ_INTERVAL 7  // No less than 3 ms: ADC kills ESP on frequent use. 

//    -- Globals --
volatile uint16_t dist_trsh             = 25;  // cm.
volatile int lastPwrState               = -1;
volatile int lastBtnValue               = -1;
volatile uint_fast32_t adc_agg          = RESET_ADC;
volatile uint8_t adc_cnt                = RESET_ADC;
volatile uint_fast32_t dist_agg         = RESET_DIST;
volatile uint8_t dist_cnt               = RESET_DIST;
volatile uint_fast32_t pump_on_time     = 0;
volatile bool pump_off_reported         = false;
volatile bool pump_on_reported          = false;
// volatile bool pump_alert_reported       = false;
volatile bool distance_alert_reported   = false;

Ticker adcTimer;
Ticker pumpAlertTimer;
Ticker distanceAlertTimer;
Ticker distanceTimer;
Ticker relayTimer;

HomieNode pumpControlNode("pump", "controller");

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
    return (val != "")?atol(val.c_str()):ERR_NUM;
  } else {
    Homie.getLogger() << "Can't restore: " << name << endl;
    return ERR_NUM;
  }
}

bool storeVariable(char *name, uint32_t val) {
  // Return true if value stored in file named as: /var_varname.
  // Return false if writing problem.
  File file = SPIFFS.open(PREFIX_VAR + String(name), "w");
  // TODO: Add free space validation like in writeState
  if (!file.print(String(val))) {
    pumpControlNode.setProperty("alert").send("Failed to write variable to SPIFFS: " + String(name) + ":" + String(val));
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

/******************************************************************************************
 *     HARDWARE HANDLING FUNCTIONS
 ******************************************************************************************/
void read_adc() {
  short val = analogRead(A0) - 512; // 2^10 = 1024 => 1024/2 - 1 = 511 => 0 level of ADC
  adc_agg += val*val;
  adc_cnt++;
}

uint32_t measure_current() {
  // ADC isn't too much stable. It drifts measurements. As result SQRT at no current is 8-60.
  // Max values for 8A current is about 250. So formula for RMS current in given circuit:
  // (sqrt(aggeraged_powered_two / total_measures) - shift(50) no less than 0) * upscale(1.25) * factor(32)
  // TODO: improve measure by fitting measure to drift. Sharp changes are real.
  int adj = (int)sqrt(adc_agg / adc_cnt) - 50;
  adj = (adj < 0) ? 0 : adj;
  uint32_t current = (uint32_t)(adj * 42);  // TODO: Calibrate here.
  // Reset counters after reading, start from beginning.
  adc_agg = RESET_ADC;
  adc_cnt = RESET_ADC;
  Homie.getLogger() << "Current: " << String(current) << "mA" << endl;
  return current;
}

void runAdc() {
  // TODO: optimize: just attach_ms should be called with 2 args, 1 is possible... or not?
  adcTimer.attach_ms(ADC_READ_INTERVAL, read_adc);
}

void read_distance() {
  digitalWrite(PIN_TRIGGER, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIGGER, LOW);

  long duration = pulseIn(PIN_ECHO, HIGH, 30000);
  float distance = duration / 58.2;   // calibrate here
  dist_agg += (uint_fast32_t)distance;
  pumpControlNode.setProperty("distance-temp").send(String(distance));
  // Don't increment counter if no mains power: SR-04 doesn't work on battery.
  // TODO: Remove. No need battery sensing.
  // if (lastPwrState) dist_cnt++;
  dist_cnt++;
}

void setRelay(const bool on) {
  // Active low relay requires logic inversion.
  bool r = !on;
  digitalWrite(PIN_RELAY, r ? LOW : HIGH);
  pumpControlNode.setProperty("relay").send(r ? "true" : "false");
  Homie.getLogger() << "Relay is " << (r ? "on" : "off") << endl;
}

// Handle button click to the switch.
void toggleRelay() {
  bool on = digitalRead(PIN_RELAY) == LOW;
  setRelay(on ? HIGH : LOW); 
}

/******************************************************************************************
 *     COMMUNICATION FUNCTIONS and HANDLERS
 *   DO NOT use underscore _ in MQTT topics and properties. (Can use in messages thus).
 ******************************************************************************************/
bool relayHandler(const HomieRange& range, const String& value) {
  if (value == "toggle") {
    toggleRelay();
    return true;
  }

  if (value != "true" && value != "false") return false;

  setRelay((value != "true"));
  return true;
}
 
bool distanceHandler(const HomieRange& range, const String& value) {
  int val = atoi(value.c_str());
  if (val < 20 || val > 200) {
    return false;
  }

  dist_trsh = val;
  storeVariable("dist_trsh", dist_trsh); 
  pumpControlNode.setProperty("distance-threshold").send(String(dist_trsh));  // ack back
  return true;
}
 
// Report MQTT alert asserted and deasserted
void alertPump() {
  // If alert triggered (not detached) then we're in trouble. Treat the pump first.
  setRelay(false);  // Turn pump off
  // Turn pump on after cooling. If still broken it will start current-alerting flow again.
  relayTimer.once(PUMP_COOLTIME, setRelay, true);
  if (Homie.isConnected()) {
    pumpControlNode.setProperty("alert").send("Pump is on for long time");
  } else pumpAlertTimer.once(60.0, alertPump);
}

void alertDistance() {
  if (Homie.isConnected() && !distance_alert_reported) {
    pumpControlNode.setProperty("alert").send("Distance is closer than 25cm");
    distance_alert_reported = true;
  } else {
    distance_alert_reported = false;
    distanceAlertTimer.once(60.0, alertDistance);
  }
}

/******************************************************************************************
 *     HOMIE ESSENTIALS: SETUP AND LOOP
 ******************************************************************************************/
void setupHandler() {
  // Serial.println();
  // If no interval stored yet, keep a default value. Else restore from Flash.
  dist_trsh = (restoreVariable("dist_trsh") == ERR_NUM)?dist_trsh:restoreVariable("dist_trsh");
  Homie.getLogger() << "dist_trsh: " << String(dist_trsh) << endl; 
  pumpControlNode.setProperty("distance-threshold").send(String(dist_trsh));  // ack back

  distanceTimer.attach(10.0, read_distance);
  adcTimer.once(30.0, runAdc);  // Lel some time to calm down after boot.
}

void loopHandler() {
  // TODO: disable polling of button when LED is on
  int value = digitalRead(PIN_BUTTON);  // debouncer not required
  if (value != lastBtnValue) {
    lastBtnValue = value;
    if ( !value ) {  // Ignore action while button is up.
      toggleRelay();
    }
  }

  // Detect power failure and restore. Not critical => so it's not important if MQTT isn't sent.
  // TODO: Remove. No need battery sensing.
  // int pwr = digitalRead(PIN_POWER);
  // if (pwr != lastPwrState) {
  //   lastPwrState = pwr;
  //   pumpControlNode.setProperty("power").send(pwr ? "mains" : "battery");
  //   Homie.getLogger() << "Powered by " << (pwr ? "mains" : "battery") << endl;
  // }

  // if (Homie.isConfigured()) {
  //   if (!adcTimer.active()) {
  //     Homie.getLogger() << "ADC dead " << String(adc_cnt) << endl;
  //     // adcTimer.attach_ms(4, read_adc);  // 20 ms for period / 4 ms = 5 samples per period = 250 samples/second. No faster than 3ms!
  //   } else {
  //     Homie.getLogger() << String(adc_agg) << String(adc_cnt) << endl;
  //   }
  // }

  // Care about pump even when no network available.
  if (adc_cnt > SAMPLES_ADC) {
    uint32_t current = measure_current();
    // After measurement pause ADC for 3 sec: give time to other processes.
    adcTimer.detach();
    adcTimer.once(3.0, runAdc);

    // Send status only once on pump change. Set timer to alert after timeout if pump is not off.
    if (current > RMS_ON_TRSH) {
      if (!pump_on_reported) {
        pump_on_time = millis();
        pumpControlNode.setProperty("pump").send("on. Current =" + f2str((float)(current / 1000.0), 1) + " A");
        pump_on_reported = true;
        pump_off_reported = false;
        pumpAlertTimer.once(PUMP_ON_TIMEOUT, alertPump);  // Either send alert or timer will off by pump off.
      }
    } else {
      if (!pump_off_reported) {
        pumpControlNode.setProperty("pump").send("off");
        pump_on_reported = false;
        pump_off_reported = true;
        pumpAlertTimer.detach();
      }
    }
  }

  if (dist_cnt > SAMPLES_DIST) {
    uint32_t distance = (uint32_t)(dist_agg / dist_cnt);  // TODO: Calibrate here.
    // Reset counters after reading, start from beginning.
    dist_agg = RESET_DIST;
    dist_cnt = RESET_DIST;
    pumpControlNode.setProperty("distance").send(String(distance) + " cm");
    if (distance < dist_trsh) { 
      if (!distance_alert_reported || distanceAlertTimer.active()) {
        alertDistance();
      }
    } else {
      distance_alert_reported = false;
    }
  }

  // delay(1);
}

void setup() {
  Serial.begin(115200);
  Serial << endl << endl;
  // pinMode(PIN_POWER, INPUT);    // TODO: Remove. No need battery sensing.
  pinMode(PIN_RELAY, OUTPUT);
  pinMode(PIN_TRIGGER, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  digitalWrite(PIN_RELAY, HIGH);

  Homie_setFirmware("pump-control", "1.0.2");

  Homie.setSetupFunction(setupHandler).setLoopFunction(loopHandler);
  // Homie.setResetTrigger(PIN_BUTTON, LOW, 5000);

  pumpControlNode.advertise("alert");  // Report problem asserted and deasserted
  pumpControlNode.advertise("pump");   // Report pump is on and off
  // pumpControlNode.advertise("power");  // Report power is available or not    // TODO: Remove. No need battery sensing.
  pumpControlNode.advertise("relay").settable(relayHandler);  // Report relay is on and off
  pumpControlNode.advertise("distance-threshold").settable(distanceHandler);  // Report relay is on and off
  pumpControlNode.advertise("distance");  // Report distance to water

  Homie.setup();
}

void loop() {
  Homie.loop();
}
