# = Description =

Room sensor is built on basis of Sonoff S20 outlet and ESPeasy.

Room sensor monitors the room for:
- Temperature,
- Light,
- Humidity,
- Movements,
- Open fire or air chemical pollution (in future version).

The room sensor is used to heat my bed: it's programmed by rules to turn the electric blanket at evening, based on room temperature.
As well, it can be controlled from central hub via MQTT, from any browser, and by button:
- Short click - turn outlet on/off.
- Long click - turn outlet with timer for 15 minutes.

Sensors used:
- Tiny PIR inside of original button (GPIO14),
- BH1750 luminosity sensor (I2C),
- SI7021 - temperature/humidity sensor (I2C),
- More sensors to be added (GPIO16).

SI7021 to be replaced with BME680: it provides barometric pressure and VOC air pollution in addition to temperature and humidity.

# = Deployment Instructions =

1. Load ESPeasy R147 or 2.0 working FW to ESP (v2.0-20180304b - good one).
2. Initialize ESP (Wifi + Pass).
3. Load configration: Tools -> Flash -> Upload -> choose config.dat. Adapt required module settings: NTP + timezone, Globalsync enabled + port + unique number, MQTT collector, etc. Save the config.
4. Load rules: Tools -> Flash -> Upload -> choose rules1.dat.
5. Tools -> Reboot.

# = Usage =
## MQTT
The outlet publishes measurement every 10 minutes:
```
shm/<system_name>/Light/Lux 222
shm/<system_name>/Env/Temperature 19.3
shm/<system_name>/Env/Humidity 48
shm/<system_name>/sys/uptime 2300
```

To turn outlet on/off use following MQTT commands:
```
mosquitto_pub -t shm/<system_name>/cmd -m "event,toggle=1"
mosquitto_pub -t shm/<system_name>/cmd -m "event,toggle=0"
```
The outlet will respond accordingly:
```
shm/<system_name>/relay 1
shm/<system_name>/relay 0
```

To enable/disable global scheduler of the outlet use following MQTT commands:
```
mosquitto_pub -t shm/<system_name>/cmd -m "event,enable=1"
mosquitto_pub -t shm/<system_name>/cmd -m "event,enable=0"
```

## Physical
- One click: toggle the outlet on/off. Blue LED light only.
- Long click: turn outlet on for 15 minutes. Blue-Green LED light indicates timer on.
- If it's already on, then any click turns it off.
