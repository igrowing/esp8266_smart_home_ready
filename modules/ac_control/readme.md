# Air Conditioner control

## Purpose
Control A/C unit via Infrared (IR) from ESP8266.

## Implementation
UI in Node-Red --> Command generation in script --> Send command via MQTT --> Transmit the IR command from ESP8266
- A/C commands are gathered into a Database in build_ac_cmd.py. Add your own commands and algorithms.
- From UI or internal logic the build_ac_cmd.py generates proper command to be send to required A/C model.
- The MQTT transfers the command to proper ESP8266 module.
- The ESP8266, running Tasmota in this case (however, Homie is my favorite, running in other cases), transmits the command with IR. 

All this is possible to do directly on RasPi without ESP8266 if your RasPi is in direct visibility from A/C remote receiver and has free IO.

## Hardware
- The ESP8266 and RasPi are able to drive IR LEDs @ current of about 10mA. This is enough in ~60cm distance of IR receiver.
- For longer distances the current [amplification is needed](http://www.learningaboutelectronics.com/Articles/LED-driver-circuit.php).
- Calculate the current limiting resistor to ~50mA. The IR LED won't burn due to very short use time. The LED drop voltage is about 1.3V. Transistor drop voltage is about 0.8V. 
- You can put 2-3 LEDs in series, depending of your VCC, recalculate the resistor.
