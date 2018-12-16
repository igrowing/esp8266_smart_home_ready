# Sewage pump controller

## Purpose

Couple of times my basement had suffered from ... well... sewage overflow due to different breaks in sewage pump. That's nasty and this must be terminated.

## Features

- Monitors level of water. When the level rises to the limit and not flushed by the pump, it sends me alert. Then i have day or two to replace the pump.
- Monitors pump work. If it works too long time (>5min in a row), it disables the pump for cooling for 10 minutes and sends me alert: this means the pump doesn't really flush.
- The Water level, the Pump work and the Controller status are published over MQTT.
- The water limit lever can be adjusted via MQTT.
- The pump can be disabled remotely: this is handy for maintenance.
- The pump can be disabled by local button click: this is handy for maintenance.
- The UI is made over Node-RED (works with no UI too).
- The circuit is equipped with surge protection, OVP, OCP, EMI features where needed.
- the sketch is implemented with ESP8266/ESP8285 over [Homie framework](https://github.com/marvinroger/homie-esp8266).

## Initialization
Once the firmware is uploaded and the module is powered on, the Homie-xxxxxx AP will appear. It might take a minute or two if you use brand new ESP8266 with unwritten Flash. Homie will format the SPIFFS first.

Open Homie init portal: http://marvinroger.github.io/homie-esp8266/configurators/v2/
Switch your computer or cellphone to that Homie-AP.
Look in the portal when the new device will be recognized. This takes 5-25 secs.
Follow step-by-step on-screen instructions. DO NOT change the MQTT base name, leave it blank to avoid a bug in Homie. Allow OTA.
Once the setup finished, switch back to your regular network: now you'll Homie reports in your MQTT broker.

## MQTT usage
### Reporting from the Room sensor to MQTT broker
When the room sensor is booted it publishes following to MQTT broker:

```
homie/pump/$homie 2.0.0
homie/pump/$mac 5C:CF:7F:1A:9B:FF
homie/pump/$name pump comtroller
homie/pump/$localip ip.ip.ip.ip
homie/pump/$stats/interval 0
homie/pump/$fw/name pump-control
homie/pump/$fw/version 1.0.2
homie/pump/$fw/checksum bd66326fd3aa80de77b99b21cb22a840
homie/pump/$implementation esp8266
homie/pump/$implementation/config {"wifi":{"ssid":"SSID"},"mqtt":{"host":"ip.ip.ip.ip","port":1883,"auth":false},"name":"pump controller","ota":{"enabled":true},"device_id":"pump"}
homie/pump/$implementation/version 2.0.0
homie/pump/$implementation/ota/enabled true
homie/pump/pump/$type controller
homie/pump/pump/$properties alert,pump,relay:settable,distance-threshold:settable,distance
homie/pump/$online true
homie/pump/pump/distance-threshold 23
```

Scenario of messages when pump works for long time. It covers alert example and distance measure:
```
homie/pump/pump/distance 24 cm
homie/pump/pump/pump on. Current = 3.1 A
...
homie/pump/pump/relay true
homie/pump/pump/alert Pump is on for long time
homie/pump/pump/pump off
...
homie/pump/pump/relay false
```

### Commands from the MQTT broker to Room sensor
All the examples use mosquitto package in Linux. Feel free use your own broker. Topics and messages: that's what MQTT examples about.

You can disable and enable the pump by following MQTT commands:
```
mosquitto_pub -t hhomie/pump/pump/relay/set/set -m 'true'
mosquitto_pub -t hhomie/pump/pump/relay/set/set -m 'false'
```

You can set a minimal distance of water level (limit) to trigger the alert:
```
mosquitto_pub -t homie/pump/pump/distance-threshold/set -m '60'
```
