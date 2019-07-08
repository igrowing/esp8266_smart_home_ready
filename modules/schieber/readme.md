# Schieber: sentry of your water bill

## Why?
* Did you encounter the case when pipe is broken in the house when you are at work or on vacation? Flooding in the house followed with huge water bill and renovation bill.
* Did you exit your garden at morning and found that at night one of irrigation pipes diconnected or split? All the garden ruined with water, accompanied with bills.
* Do you know who's in your famility/guests wasting the most of water in your utility bill?
* You have 2 or more toilets or water-using appliances. Which of them OK and which is spending too much water (can be optimized)?
* Did your kid forget to close the tap? Is this repeating?
* Do you care about green future for next generations? Shouldn't people reduce waste of water... starting from yourself?

That's why you need the water control unit AKA Schieber (in German), שיבר (in Hebrew), She-bear (pronunciation). 

## Description
The she-bear consists of:
- Latching Valve - Hydrogenerator - Water flow Hall sensor, connected sequentially.
- MCU ESP8266 + some electronics.
- Solar panel as spare energy source.
- Li-Ion cell as energy storage.

### Features
* Easy to install.
* Zero-maintenance. Fills the battery from water flow and from solar energy.
* Counts and reports water consumption via MQTT.
* Checks configurable amount of water and duration of single run.
* Alerts when amount or duration consumed by half.
* Shuts water off when amount or duration exceeded configured threshold.
* Configurable LED blink period while water is running.
* 2 button control: one button to awake the Schieber, other button to toggle the valve.
* MQTT control of the valve: open/close/toggle.
* When automatically shut off checks the MQTT messages every hour. Will open the valve if instructed via MQTT.
* Future support: Can accept emergency shut off command from ["leakage detector"](https://github.com/igrowing/esp8266_smart_home_ready/tree/master/modules/leakage_detector).

## How does it work
TBD

## How to install it
TBD

## How to control it
### Via MQTT
TBD

### With buttons
If water is flowing, LED is rarely blinking:
* Press Valve button to toggle the valve. Expect LED stopped blinking.
  
If No water flow, LED is off:
* Press Power button to awake the She-bear.
* Wait few seconds: LED will blink slowly, then fast.
* While LED is blinking fast (15 seconds) press the Valve button to toggle the valve.

