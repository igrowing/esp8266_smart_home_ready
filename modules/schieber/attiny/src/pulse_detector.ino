// ATtiny85 pulse detector.
// Ignore single logical level change.
// When perioc pulse detected (oscillations) drive external LDO.
// Keep itself in sleep mode for power save, wake on pin change interrupt.
// LED signaling is just for visual debug, not must. 3 long blinks on power on.
// Very short blink on edge changed. Very long blink on LDO/output enabled.
// 2 blinks when going to sleep. 3 blinks when awaken. 
//
// Written by: iGrowing, 2019
// Inspired by: Nick Gammon
//
// ATMEL ATTINY x5
//                        +-\/-+
//       Ain0 (D 5) PB5  1|    |8  Vcc
//       Ain3 (D 3) PB3  2|    |7  PB2 (D 2) Ain1  - OUT
// INP - Ain2 (D 4) PB4  3|    |6  PB1 (D 1) pwm1  - LED
//                  GND  4|    |5  PB0 (D 0) pwm0
//                        +----+

#include <avr/sleep.h>
#include <avr/power.h>

#define TIMEOUT_MAX       3000    // Don't wait longer
#define PULSE_DETECT_CNT  3       // # of valid edges to consider pulses
#define LED               1       // Active HIGH
#define LDO_EN            2       // Active HIGH
#define DETECT_IN         4       // PB4, PCINT4
#define POWERON_MS        2000    // How long to hold LDO enabled
short timeout_cnt = 0;            // count time: millis() doesn't work well with deep sleep
short edge_cnt = 0;               // count edges
byte last_in = 0;
bool is_fired = false;

ISR (PCINT0_vect) {
// do something
}

void setup ()  {
  pinMode(DETECT_IN, INPUT);
  pinMode(LDO_EN, OUTPUT);
  
  PCMSK  |= bit (PCINT4);  // want pin D4 / pin 3
  GIFR   |= bit (PCIF);    // clear any outstanding interrupts
  GIMSK  |= bit (PCIE);    // enable pin change interrupts 
  blink(3, 1000);
}  // end of setup

void loop () {
  // Real loop, run until timeout reached
  while (timeout_cnt < TIMEOUT_MAX) {
    byte in = digitalRead(DETECT_IN);
    // Update edge counter if state changed. (blink led too).
    if (last_in != in) {
      last_in = in;
      edge_cnt++;
//      blink(1, 10);  // TODO: remove due to performance considerations
    } else timeout_cnt++;
    
    // Enable LDO for known time
    if (edge_cnt >= PULSE_DETECT_CNT && !is_fired) {
      is_fired = true;
      pinMode(LDO_EN, OUTPUT);
      digitalWrite(LDO_EN, HIGH);
      blink(1, POWERON_MS);  // Enable power
//      digitalWrite(LDO_EN, LOW);
      // Do not set LOW to GPIO: let the main MCU software to lead.
      pinMode(LDO_EN, INPUT);      
    }
    delay(1);
  }
  blink(2, 300);
  doSleep ();
}  // end of loop
  
void doSleep () {
  // Prepare to next awake round
  edge_cnt = 0;
  timeout_cnt = 0;
  is_fired = false;
  
  // Sleeping
  ADCSRA = 0;           // turn off ADC
  set_sleep_mode(SLEEP_MODE_PWR_DOWN);
  power_all_disable();  // power off ADC, Timer 0 and 1, serial interface
  sleep_enable();
  sleep_cpu();                             
  
  // Awaken
  sleep_disable();   
  power_all_enable();  // power everything back on
//  blink(3, 100);   // TODO: remove due to performance considerations
}  // end of doSleep

void blink(int times, int on_ms) {
  pinMode(LED, OUTPUT);
  for (int i = 0; i < times; i++) {
    digitalWrite(LED, HIGH);
    delay(on_ms); 
    digitalWrite(LED, LOW);  
    delay(100); 
  }
  pinMode(LED, INPUT);  // Don't push/pull, save power
}

