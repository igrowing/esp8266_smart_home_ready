#include <Homie.h>

#define PIN_CSENSE  12
#define PIN_VSENSE  13
#define PIN_RELAY   14
#define PIN_BUTTON  0
#define CLEARED     -10
#define PASS_COUNT  10
const String PREFIX = "/state";

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
volatile uint32_t lastFileIndex = 0;
volatile int lastBtnValue = CLEARED;
volatile bool relayState;
volatile byte vSenseIntCnt = 0;
volatile byte cSenseIntCnt = 0;
Ticker interruptHolder;
Ticker statusTimer;
Ticker writeTimer;
Ticker expTimer;
volatile bool isInterruptEnabled = false;
volatile ulong ists = CLEARED;  // Interrupt Start Time Stamp

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
  ists = millis();
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
  lightNode.setProperty("on").setRetained(false).send(on ? "true" : "false");
  Homie.getLogger() << "Light is " << (on ? "on" : "off") << endl;
  // Prepare counters, make checkStatus reporting change immediately
  vSenseIntCnt = 0;
  cSenseIntCnt = 0;
  lastStatus = CLEARED;
  if (!isInterruptEnabled && relayState) enableInterrupts();  // Abort wait 10sec to enable power measure if load on.
  writeTimer.once(0.5, writeState);  // Write state to SPIFFS
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
  int v = (vSenseIntCnt>PASS_COUNT) ? 2 : 0;
  int c = (cSenseIntCnt>PASS_COUNT) ? 4 : 0;  // Filter noise: can be few inductive pulses.
  if ((v && c) || (millis() - ists) > 5000) {
    disableInterrupts();  // Pause if got measurements or waited too long time (60 sec)
  } else {
    // Don't waste resources if not reached certain reporting level or waiting too long.
    return;
  }

  if (lastStatus != r+v+c) {
    lastStatus = r+v+c;
    lightNode.setProperty("voltage").setRetained(false).send(v ? "true" : "false");
    lightNode.setProperty("current").setRetained(false).send(c ? "true" : "false");
    lightNode.setProperty("status").setRetained(false).send(sw_statuses[lastStatus] + ",c=" + String(cSenseIntCnt) + ",v=" + String(vSenseIntCnt));
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

void debugReport() {
  FSInfo info;
  SPIFFS.info(info);
  uint32_t freeBytes = info.totalBytes - info.usedBytes;
  String str = "";
  uint32_t cnt = 0;
  Dir dir = SPIFFS.openDir(PREFIX);
  while (dir.next()) {
    // str += dir.fileName() + "; ";
    cnt++;
  }
  lightNode.setProperty("status").send("Free=" + String(freeBytes) + ", Files: " + String(cnt));
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
    lightNode.setProperty("status").send("Flash cleaned");
  } else {
    // Increment global file name
    lastFileIndex++;
  }

  // Write global file name. File name format: /stateX_N, where X is 0 or 1, N is sequential number.
  File f = SPIFFS.open(PREFIX + (relayState?"1":"0") + "_" + String(lastFileIndex), "w");
  f.close();
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

void setup() {
  SPIFFS.begin();  // Call SPIFFS before Homie awake: the state required ASAP.
  Serial.begin(115200);
  Serial << endl << endl;

  // Operate relay directly: Homie isn't ready yet to treat ESP properly at this stage.
  int restored = readState();
  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_RELAY, restored);
  relayState = restored == 1;

  pinMode(PIN_VSENSE, INPUT);
  pinMode(PIN_CSENSE, INPUT);
  // pinMode(PIN_BUTTON, INPUT_PULLUP);
  // debouncer.attach(PIN_BUTTON);
  // debouncer.interval(20);
  Homie_setFirmware("awesome-relay", "1.1.2");
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
  expTimer.once(30.0, debugReport);
}

void loop() {
  Homie.loop();
}
