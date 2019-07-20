// ATtiny85 pulse detector.
// Consumes 5-6mA while active, <0.5mA while sleeps (measured).
// Ignore single logical level change.
// When periodic pulse detected (oscillations) drive external LDO.
// Keep itself in sleep mode for power save, wake on pin change interrupt.
// ESP signals to ATtiny about power off or sleep mode. 
// When Sleep signal sent, keep LDO on and ATtiny off. 
// When shutdown signal sent, power off the LDO and itself.
// Serial prints for text debug.
//
// Written by: iGrowing, 2019
//
// ATMEL ATTINY x5 Pin Usage:
//                   +-\/-+
//             PB5  1|    |8  Vcc
//             PB3  2|    |7  PB2 - OUT LDO control
// ESP + Btn - PB4  3|    |6  PB1 - LED
//             GND  4|    |5  PB0 - Water sense
//                   +----+

#include <avr/sleep.h>
#include <avr/power.h>
//#define DEBUG              // enables prints via serial 

#include <TinyPinChange.h>
#ifdef DEBUG
#include <SoftSerial.h>     /* Allows Pin Change Interrupt Vector Sharing */
#define DEBUG_TX_RX_PIN     3
SoftSerial sSerial(DEBUG_TX_RX_PIN, DEBUG_TX_RX_PIN, false);
#endif
//#define F_CPU 1000000  // For non-USB IC only

#define TIMEOUT_MAX       30000   //
#define TIMEOUT_MID       3000    // Timeout for false-awake
#define TIMEOUT_MIN       1000    // Sliding window
#define PULSE_DETECT_CNT  3       // # of valid edges to consider pulses
#define LED               1       // Active HIGH
#define LDO_EN            2       
#define ESP_14            4       // has zener
#define WTR_IN            0       // PB0, PCINT0
uint32_t last_ts = 0;             // timestamp
volatile uint16_t edge_cnt = 0;   // count edges
bool last_in = false;
bool snap_water = false;
bool snap_ctrl = true;

// Commented out code conflicts with TinyPinChange lib. The TinyPinChange needed for debug with SoftSerial.
// ISR (PCINT0_vect) {
// // do something
// }

void setup()  {
  pinMode(WTR_IN, INPUT);
  pinMode(ESP_14, INPUT);
  pinMode(LDO_EN, OUTPUT);  // turn to output when needed only
  digitalWrite(LDO_EN, HIGH);
  
  // PCMSK  |= bit (PCINT0);  // awake on D0
  // PCMSK  |= bit (PCINT4);  // awake on D4
  // GIFR   |= bit (PCIF);    // clear any outstanding interrupts
  // GIMSK  |= bit (PCIE);    // enable pin change interrupts 
  // blink(3, 1000);

  TinyPinChange_Init();

  TinyPinChange_RegisterIsr(ESP_14, InterruptFunc);
  TinyPinChange_RegisterIsr(WTR_IN, InterruptFunc);
  /* Enable/disable Interrupt Pin Change for each pin in doSleep function */

#ifdef DEBUG
  sSerial.begin(115200);
  sSerial.txMode();
  sSerial.println(">> booted");
#endif
}  // end of setup

void loop() {
  while (true) {
    print("while begin");
    bool control = digitalRead(ESP_14);
    if (control || is_water_running()) {
      if (control != snap_ctrl) {
        print("Control not changed => ESP was sleeping => power down it first");
        digitalWrite(LDO_EN, LOW);
        delay(1000);
      }
      print("power on for btn or water");
      poweron();
      if (wait_power_off()) poweroff();
      else {
        /* Sleep the ATTiny, Leave LDO on. Good for ESP sleep. */
        print("just sleep");
        doSleep();
      }
      continue;
    }

    // Suspect of false awake if reached here
    if (! is_water_running()) {  // Double check... not really needed
      print("awaken not by btn but by wtr not running");
      poweroff();
    }
  }
}  // end of loop
  
void doSleep() {
  print(">> sleep");
  // blink(2, 300);
  delay(10);          // Let signals set
  // Keep current status to find which of pins caused wake later
  snap_water = digitalRead(WTR_IN);
  snap_ctrl = digitalRead(ESP_14);
  // Enable PCINT for awake
  TinyPinChange_EnablePin(ESP_14);
  TinyPinChange_EnablePin(WTR_IN);
  
  // Sleeping
  ADCSRA = 0;           // turn off ADC
  set_sleep_mode(SLEEP_MODE_PWR_DOWN);
  power_all_disable();  // power off ADC, Timer 0 and 1, serial interface
  sleep_enable();
  sleep_cpu();                             
  
  // Awaken
  sleep_disable();   
  power_all_enable();  // power everything back on
  TinyPinChange_DisablePin(ESP_14);
  TinyPinChange_DisablePin(WTR_IN);
  print(">> awaken");
}  // end of doSleep

//void blink(int times, int on_ms) {
//  pinMode(LED, OUTPUT);
//  for (int i = 0; i < times; i++) {
//    digitalWrite(LED, HIGH);
//    delay(on_ms); 
//    digitalWrite(LED, LOW);  
//    delay(100); 
//  }
//  pinMode(LED, INPUT);  // Don't push/pull, save power
//}

/* Power off the ESP and ATTiny. */
void poweroff() {
  print(">> poweroff");
  // is_esp_sleeping = false;
  digitalWrite(LDO_EN, LOW);
  doSleep();
}

/* Turn on the ESP and let it time to hold IO14 up. */
void poweron() {
  print(">> poweron");
  digitalWrite(LDO_EN, HIGH);
  delay(TIMEOUT_MIN);
}

void InterruptFunc() {
}

void print(const char * arr) {
#ifdef DEBUG
  sSerial.println(arr);
#endif
}

void print2(const char * arr, uint32_t num) {
#ifdef DEBUG
  sSerial.print(arr);
  sSerial.println(num);
#endif
}

/* pulseIn doesn't work. Manual re-write. */
uint16_t pulse_in(uint16_t timeout_ms) {
  uint32_t start_ts = millis();
  uint16_t tmp = 0;
  while(digitalRead(ESP_14) == LOW) {
    tmp = millis() - start_ts;
    if (tmp > timeout_ms) {return 0;}
  }
  return tmp;
}

/* Retrun true as fast as there are changes >= PULSE_DETECT_CNT. Return false on timeout. */
bool is_water_running() {
  uint32_t start_ts = millis();
  edge_cnt = 0;
  while(millis() - start_ts < TIMEOUT_MID) {
    update_edge();
    if (edge_cnt >= PULSE_DETECT_CNT) return true;
  }
  return false; 
}

void update_edge() {
  bool in = digitalRead(WTR_IN);
  if (last_in != in) {
    last_in = in;
    edge_cnt++;
  }
}

bool wait_power_off() {
  print(">> waiting power off signals or timeout");
  last_ts = millis();
  while (true) {
    uint16_t signal = pulse_in(15);
    update_edge();
    if (edge_cnt > PULSE_DETECT_CNT) {
      edge_cnt = 0;
      last_ts = millis();
    }
    if ((edge_cnt <= PULSE_DETECT_CNT && millis() - last_ts > TIMEOUT_MAX) || (0 == signal && digitalRead(ESP_14) == LOW)) return true;
    else if (signal > 0) return false;
  }
}

