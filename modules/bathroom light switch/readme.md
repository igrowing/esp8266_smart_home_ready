#Bathroom light

## Why?
- Why turn lights on and off manually? Why care of light at all? It must be automatic.
- At night time: 
  * sometimes hard to find the switch :)
  * the strong upper light dazzles;
  * the strong upper light reduces melatonin level and causes awakeness;

## What?
What could be done:
- Use simple time relay over the PIR. Daylight saving changes require manual update. PIR needs light-sensing mod.
- 2 separate PIRs with 2 separate relays. Too much wires, too expensive. Not fun.
- Sonoff Dual + modded PIR + Homie-based firmware + Node-RED based UI = full control and fun.


## How?
How to solder, connect the hardware, compile and flash the firmware - the easy part.

How to control it over MQTT - the fun part!

homie/dual_r2/bathroom/time 3567
homie/dual_r2/bathroom/time/set 1676326906216

homie/dual_r2/bathroom/status/set -m true
homie/dual_r2/bathroom/offtime-start-hhmm 2335
homie/dual_r2/bathroom/offtime-end-hhmm 555
homie/dual_r2/bathroom/power1 false
homie/dual_r2/bathroom/power2 false
homie/dual_r2/bathroom/on-time-s 120
homie/dual_r2/bathroom/status time: 22:21
homie/dual_r2/bathroom/status last run reason: 0

homie/dual_r2/bathroom/power1/set -m toggle
homie/dual_r2/bathroom/power1 true
homie/dual_r2/bathroom/status 0

homie/dual_r2/bathroom/offtime-start-hhmm/set -m 2335
homie/dual_r2/bathroom/offtime-end-hhmm/set -m 555
homie/dual_r2/bathroom/on-time-s/set -m 120
