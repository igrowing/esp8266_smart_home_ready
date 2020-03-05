# Boilero: another yet "perfect" boiler controller #

## Story ##
My old good mechanical timer for boiler passed away recently. I repaired it about month ago... and this time it's dead completely. 
I could buy ready product "Switcher" dedicated for remote control of boiler. 
However, this type of "smart" devices does not make sense to my [concept of smart home](https://github.com/igrowing/esp8266_smart_home_ready). 
I could use sonoff with 16A relay and I thought it's too close to boiler consumption, so risky... 
Meanwhile I've build a "perfect" boiler controller and I could load Tasmota on it... The time it would take me to learn and set all options and rules to reach required functionality would be approx. the same as write from scratch a program with Homie. 

## Features ##
- 30A relay overrated enough for any boiler.
- EMI and surge protection.
- OLED display, 3 buttons, and multicolor RGB LED as user interface.
- GUI on Node-RED via hub on Raspberry-Pi. Accessible/visible from mobile phone or computer in LAN or outside of home.
- MQTT reported/controlled.
- Controlled by hardware buttons too.
- Programmable network independent timer. The timer is self-adjusting to season, heating more on winter and less in summer. The adjustment rate is configurable too.
- Enhanced LED signaling: 
  - LED becomes 'hotter' while water is heating, incrementally changing the color from blue to red.
  - If the boiler has reached max. temperature and not heating then LED is flashing red meanwhile relay should be on. In this state, the relay is actually off, keeping the electrical wiring of the boiler out of voltage.
  - Rainbow colors while not configured.
  - TBD color while trying to connect to Wifi.
- Advanced measurement:
  - Current
  - Air temperature (used for auto-adjustment of timer)
  - Humidity

## Human interface and operation ##
### Physical UI/UX ###
TBD
### Graphical UI/UX ###
TBD

## MQTT communication ##
TBD
