# ATtiny85 pulse detector.

## Prolog
Schieber is constructed/built as self-sufficient device with zero-maintenance. That means it should produce enough electricity for its function.
And it should waste as less as possible to keep the enegy balance above 0.

Initially, it was planned to detect water flow by presence of voltage from inline hydrogenerator. That's simple solution. 
Its lack is: significant water flow needed to detect the voltage. I.e. weak flow (AKA leaks) won't count or be detected.

The water flow detector initially was powered by ESP8266 only for the time when there's significant water flow to measure. 
The reason was: cheap water flow Hall sensors are sensitive, wide flow range measuring and... energy wasting: 4-5mA.

## Problem
With some modification, I considered it as trade off to spend some electricity for precision of water flow detection. **Here the problem caught me.**
Tested 5 types of flow hall sensors. All Hall sensors don't keep known steady state. In no flow it's expected to be logical 1 output due to builtin pullup.
However, the sensor keep random 0 or 1 state in no flow. And rarely, once in few hours, it changes the state for some reason.

It was required:
- To reduce the steady current;
- To detect real water flow but not single state change.

## Solution
Fow steady current reduction it's easy: replace the pullup resistor in the water flow detector with 50kOhm one. This reduces current from 4-5mA to 1-2mA.

The ideal solution for pulse detection would CMOS-based electronic circuit. After dosens of failing experiments I gave up in favor of microcontroller.
ATTiny85 does the job for several uA. Fair enough, does the job.

## How does it work
* Ignore single logical level change.
* When periodic pulse detected (oscillations) drive external LDO.
* Keep itself in sleep mode for power save, wake on pin change interrupt.
* LED signaling is just for visual debug, not must. 3 long blinks on power on.
* Very short blink on edge changed. Very long blink on LDO/output enabled.
* 2 blinks when going to sleep. 3 blinks when awaken.  
