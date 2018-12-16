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

