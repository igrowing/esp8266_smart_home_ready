#include <Homie.h>

#define PIN_CSENSE  12
#define PIN_VSENSE  13
#define PIN_RELAY   14
#define PIN_BUTTON  0
#define CLEARED     -10

// Get status by index: request * 1 + v_sense * 2 + c_sence * 4
String sw_statuses[] = { // r(elay)on/off _ v(oltage)yes/no _ c(urrent)yes/no 
  "roff_vno_cno: OK, off",
  "ron_vno_cno: Error: Detection circuit problem or serious HW problem",
  "roff_vyes_cno: Error: serious HW problem",
  "ron_vyes_cno: Error: Bulb burnt or C-Detection circuit problem",
  "roff_vno_cyes: Error: serious HW problem",
  "ron_vno_cyes: Error: V-Detection circuit problem",
  "roff_vyes_cyes: Error: serious HW problem",
  "ron_vyes_cyes: OK, on",
};
volatile int lastStatus = CLEARED;

HomieNode lightNode("light", "switch");
// Bounce debouncer = Bounce(); // Bounce is built into Homie, so you can use it without including it first
volatile int lastBtnValue = CLEARED;
volatile bool relayState;
volatile byte vSenseIntCnt = 0;
volatile byte cSenseIntCnt = 0;
Ticker interruptHolder;
Ticker statusTimer;
volatile bool isInterruptEnabled = false;

void handleCsenseInt() {
  cSenseIntCnt++;
}

void handleVsenseInt() {
  vSenseIntCnt++;
}

void enableInterrupts() {
  vSenseIntCnt = 0;
  cSenseIntCnt = 0;
  isInterruptEnabled = true;
  attachInterrupt(digitalPinToInterrupt(PIN_CSENSE), handleCsenseInt, FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_VSENSE), handleVsenseInt, FALLING);
}

void disableInterrupts() {
  detachInterrupt(digitalPinToInterrupt(PIN_CSENSE));
  detachInterrupt(digitalPinToInterrupt(PIN_VSENSE));
  isInterruptEnabled = false;
  interruptHolder.once(10.0, enableInterrupts);
}

// TODO: Relay status isn't sent on boot (however the unit status is sent).
void setRelay(const bool on) {
  relayState = on;
  digitalWrite(PIN_RELAY, on ? HIGH : LOW);
  lightNode.setProperty("on").send(on ? "true" : "false");
  Homie.getLogger() << "Light is " << (on ? "on" : "off") << endl;
  // Prepare counters, make checkStatus reporting change immediately
  vSenseIntCnt = 0;
  cSenseIntCnt = 0;
  lastStatus = CLEARED;
  if (!isInterruptEnabled && relayState) enableInterrupts();  // Abort wait 10sec to enable power measure if load on.
}

// Handle button click to the switch.
void toggleRelay() {
  bool on = digitalRead(PIN_RELAY) == HIGH;
  setRelay(on ? LOW : HIGH);
}

// Handle incoming MQTT message /homie/unit-name/light/on/set <- true/false/toggle
bool lightOnHandler(const HomieRange& range, const String& value) {
  if (value == "toggle") {
    toggleRelay();
    return true;
  }

  if (value != "true" && value != "false") return false;

  setRelay((value == "true"));
  return true;
}

// Define callback for status report
void checkStatus() {
  // Don't waste resources and don't loose report while network is not connected.
  if (!Homie.isConnected()) {
    vSenseIntCnt = 0;
    cSenseIntCnt = 0;
    return;
  }

  // Don't waste resources if no measures ready.
  if (!isInterruptEnabled && lastStatus != CLEARED) return;

  int r = (relayState) ? 1 : 0;
  int v = (vSenseIntCnt) ? 2 : 0;
  int c = (cSenseIntCnt>10) ? 4 : 0;  // Filter noise: can be few inductive pulses.
  if (v+c) {
    disableInterrupts();  // Pause
  }
  
  if (lastStatus != r+v+c) {
    lastStatus = r+v+c;
    lightNode.setProperty("voltage").send(v ? "true" : "false");
    lightNode.setProperty("current").send(c ? "true" : "false");
    lightNode.setProperty("status").send(sw_statuses[lastStatus]);
    Homie.getLogger() << "Status is " << (sw_statuses[lastStatus]) << endl;
  }
}

void loopHandler() {
  // debouncer.update();
  // int value = debouncer.read();
  int value = digitalRead(PIN_BUTTON);  // debouncer not required
  if (value != lastBtnValue) {
    lastBtnValue = value;
    if ( !value ) {  // Ignore action while button is up.
      toggleRelay();
    }
  }
}

void setup() {
  Serial.begin(115200);
  Serial << endl << endl;
  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_RELAY, HIGH);

  relayState = true;
  pinMode(PIN_VSENSE, INPUT);
  pinMode(PIN_CSENSE, INPUT);
  // pinMode(PIN_BUTTON, INPUT_PULLUP);
  // debouncer.attach(PIN_BUTTON);
  // debouncer.interval(20);
  Homie_setFirmware("awesome-relay", "1.0.7");
  // Homie_setBrand("shm");  // homie ???
  Homie.setResetTrigger(PIN_BUTTON, LOW, 5000);

  // Prohibit the parameters to be set from MQTT broker
  lightNode.advertise("voltage");
  lightNode.advertise("current");
  lightNode.advertise("status");
  // Allow the node to publish over MQTT. Allow the parameter "on" to be set from MQTT broker, i.e. allow remote control
  lightNode.advertise("on").settable(lightOnHandler);

  Homie.setLoopFunction(loopHandler);

  Homie.setup();
  // Run following after Homie.setup() since they use Homie infrastructure.
  enableInterrupts();  // Enable once. Rest of enables with be done from "disableInterrupts()"
  statusTimer.attach(1.0, checkStatus);
}

void loop() {
  Homie.loop();
}
