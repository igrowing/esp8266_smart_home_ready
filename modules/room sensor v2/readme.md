# Another room sensor.

Changes from [first version](https://github.com/igrowing/esp8266_smart_home/tree/master/modules/room_sensor):
- Based on separate HW, old broken outlet, used as mechanical basis and its PSU. Tiny size.
- No more Easy-ESP. Moved to [Homie v2.0.0](https://github.com/marvinroger/homie-esp8266). More flexible in features, more stable, works faster.
- BME sensor in use: air quality, temperature, humidity, air pressure. Average measurements by continuous reads. Send report periodically (interval is set via MQTT, default 15 min).
- Send verbal forecast for 24 hours in around 30 km of measurement location.
- Send "Bad air quality" report if the air quality goes below 30%.
- More clicks :)
  - Single click - toggle.
  - Long click - set timer (timer is adjustible via MQTT, default 15 min).
  - Double click - send status immediately.
  - Very long click (10sec) - reset to factory defaults, on courtesy of Homie.
- When PIR detects movement the LED is lit for 1 second. Aggregative report to MQTT once in a minute to avoid flooding.
- The room sensor now remembers its state (on or off) and all customised settings. After power failure it restores all the statuses and values as they was before the black out. Special SPIFFS writing is used to avoid fast wearout. 

## Initialization
Once the firmware is uploaded and the module is powered on, the Homie-xxxxxx AP will appear. It might take a minute or two if you use brand new ESP8266 with unwritten Flash. Homie will format the SPIFFS first.
- Open Homie init portal: http://marvinroger.github.io/homie-esp8266/configurators/v2/
- Switch your computer or cellphone to that Homie-AP.
- Look in the portal when the new device will be recognized. This takes 5-25 secs.
- Follow step-by-step on-screen instructions. DO NOT change the MQTT base name, leave it blank to avoid a bug in Homie. Allow OTA.
- Once the setup finished, switch back to your regular network: now you'll Homie reports in your MQTT broker.

## Node-RED intergation example
Please refer to the json code in src folder.

![The flow](room_sensor_nr_flow.jpg)

The UI example is partial, just for illustration.

![The Node-RED UI](room_sensor_nr_ui.jpg)


## MQTT usage
### Reporting from the Room sensor to MQTT broker
When the room sensor is booted it publishes following to MQTT broker:
```
homie/awesome-room/$homie 2.0.0
homie/awesome-room/$mac 68:C6:3A:DA:C7:E0
homie/awesome-room/$name awesome room outlet
homie/awesome-room/$localip ip.ip.ip.ip
homie/awesome-room/$stats/interval 0
homie/awesome-room/$fw/name room-sensor
homie/awesome-room/$fw/version 1.0.6
homie/awesome-room/$fw/checksum b95f2078c322fb698d8ade042cecdec0
homie/awesome-room/$implementation esp8266
homie/awesome-room/$implementation/config {"wifi":{"ssid":"yourwifi"},"mqtt":{"host":"mqtt_addr","port":1883,"auth":false},"name":"Awesome room outlet","ota":{"enabled":true},"device_id":"awsome-room"}
homie/awesome-room/$implementation/version 2.0.0
homie/awesome-room/$implementation/ota/enabled true
homie/awesome-room/power/$type switch
homie/awesome-room/power/$properties pir,temperature,humidity,pressure,air,status,on:settable,status-interval-s:settable,timer-interval-s:settable
homie/awesome-room/$online true
homie/awesome-room/power/status Free=2923899, Files: 99
```
When motion detected:
```
homie/awesome-room/power/pir Move detected
```

Periodic report looks like: (similar report on double click)
```
homie/awesome-room/power/temperature 21.49 *C
homie/awesome-room/power/humidity 46.76 %
homie/awesome-room/power/pressure 1012.65 hPa
homie/awesome-room/power/air 126.67 kOhm
homie/awesome-room/power/quality 99.7 %
homie/awesome-room/power/forecast Fair
```

When power is turned on or off, locally by button or by timer or remotely it looks like:
```
homie/awesome-room/power/on true
homie/awesome-room/power/on false
```

### Commands from the MQTT broker to Room sensor
All the examples use `mosquitto` package in Linux. Feel free use your own broker. Topics and messages: that's what MQTT examples about.

You can control the power. Use `true` or `false` or `toggle` as message:
```
mosquitto_pub -t homie/awesome-room/power/on/set -m 'true'
```

You can control report interval. This is timeout between status reports.
```
mosquitto_pub -t homie/awesome-room/power/status-interval-s/set -m '600'
```

You can control timer interval. This is timeout to shut power off after long-click.
```
mosquitto_pub -t homie/awesome-room/power/timer-interval-s/set -m '1800'
```

## What's next (aka TODO:)
- Add light sensor like in the first room sensor... Doubtful need.
- Add more hardware: IR transmitter to control TV, AC, etc. Flame detector to alert fire. Accelerometer to register earthquakes :) Microphone for voice actions.
- Publish schematics.
- Add internal scheduling. Doubtful need since MQTT works well.
