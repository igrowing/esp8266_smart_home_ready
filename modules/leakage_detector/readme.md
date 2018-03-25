The universal single-shot module is developed initiall as water leakage detector: after I had 2 floods in my home I decided to build leakage detector and alerter.
IMHO, water flood detector must be in every home. And better it will never work :)

This is tiny box (about 1x2") on the floor in bathroom or/and kitchen.
When flood is detected the alert is sent to home owner email. Optionally MQTT topic is published. Optionally message "Shut off" is sent to central water input valve in the house.

It works on 2xCR2032 batteries, which should be enough for 2 years in this design.
When the battery goes critical low, an appropriate message sent to home owner's email and to MQTT.

The detector can be used as:
- Window break detector (add vibration sensor), or 
- Door opening alerter (add vibration of reed + larger battery), or
- PIR movement detector (add PIR + larger battery), or
- anything else :) Dream big.

Future TODOs:
- HW: Add capacitive sensor to measure amount of water: this is to avoid false alarm from occasional drop or from wet floor.
- SW: Add nice user activation/reactivation interface.
