= Description =

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

= Instructions =

1. Load ESPeasy R147 or 2.0 working FW to ESP (v2.0-20180304b - good one).
2. Initialize ESP (Wifi + Pass).
3. Load configration: Tools -> Flash -> Upload -> choose config.dat. Adapt required module settings: NTP + timezone, Globalsync enabled + port + unique number, MQTT collector, etc. Save the config.
4. Load rules: Tools -> Flash -> Upload -> choose rules1.dat.
5. Tools -> Reboot.

