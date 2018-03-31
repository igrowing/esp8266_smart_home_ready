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

= Instructions =
