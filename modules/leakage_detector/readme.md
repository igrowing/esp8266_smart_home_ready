# Description

The universal single-shot module is developed initiall as water leakage detector: after I had 2 floods in my home I decided to build leakage detector and alerter.
IMHO, water flood detector must be in every home. And better it will never work :)

This is tiny box (about 1x2") on the floor in bathroom or/and kitchen.
When flood is detected the alert is sent to home owner email. Optionally MQTT topic is published. Optionally message "Shut off" is sent to central water input valve in the house.

It works on 350mAh Li-Ion battery, which should be enough for 2+ years in this design.
When the battery goes critical low, an appropriate message sent to home owner's email and to MQTT.

The detector can be used as:
- Window break detector (add vibration sensor), or 
- Door opening alerter (add vibration of reed + larger battery), or
- PIR movement detector (add PIR + larger battery), or
- anything else :) Dream big.

The firmware is based on ESPEasy: that's generic enough and not required special software to be designed (unless you go mass production :).

# Usage
1. Perform Flashing instructions below.
2. Place the unit under sink, near pipes or wherever you described in Flashing #3.
3. When the unit detects water leackage, it sends MQTT message and e-mail once in 70 minutes. Fix the leakage and remove the unit from the water. In order to switch the unit back to watching mode off-on the switch. Hint: After leakage event it's recommended to recharge the battery.
4. When the unit detects low voltage on the battery, it sends MQTT message and e-mail once in 70 minutes. Recharge the battery. In order to switch the unit back to watching mode off-on the switch.

# Flashing Instructions

The flashing is recommended to be performed when battery is off. Use power of external converter.

0. Shorten GPIO5 to GND in dedicated header. This is to prefent automatic power off in case the unit is preconfigured.
1. Load 2.0 working FW to ESP (v2.0-20180304b - good one).
2. Initialize ESP (Wifi + Pass).
3. Load configration: Tools -> Flash -> Upload -> choose config.dat. Adapt required module settings: NTP + timezone, Globalsync enabled + port + unique number, MQTT collector, etc. Save the config.
4. Load rules: Tools -> Flash -> Upload -> choose rules1.dat.
5. Configure both notifications: 1st for leakage alert, 2nd for Low battery alert. I use SMTP2GO.
6. Tools -> Reboot.
7. Remove the short from GPIO5.
8. Power cycle the unit with the switch.

## For debug:
Connect GPIO5 to GND, this prevents deepsleep and poweroff.

# Future TODOs:
- HW: Add capacitive sensor to measure amount of water: this is to avoid false alarm from occasional drop or from wet floor.
- SW: Add nice user activation/reactivation interface.
- System: Add publish to central water valve to shut water off.
