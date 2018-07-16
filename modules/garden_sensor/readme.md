== Description ==

The garden sensor is used for gathering data from garden about:
- Soil moisture,
- Soil pH,
- Air temperature,
- Air humidity,
- Approximate light conditions.

The sensor is powered off for 2 hours with simple/cheap timer. This is not sleep but complete shutdown for saving the energy.
Once awaken it completes firmware internal setup, gathers sensors data, and sends the data to the hub with MQTT.
Once data sent, the sensor shut itself down and triggers the timer again.


== Instructions ==

1. Load R147 working FW to ESP (WROOM2 has 512MB of flash => ESPEasy_R147_512.bin - good one).
2. Initialize ESP (Wifi + Pass).
3. Connect to it from broser and load settings:
Tools -> Load -> Choose file (config.dat) -> Upload.
Tools -> Flash -> Upload -> choose notification.dat and rules.dat (one file at the time).
4. Fix the unit name (config page) and its Global number.
5. Fix location in both notifications.
6. Tools -> Reboot.

For debug:
Disconnect/remove jumper of GPIO16, this prevents deepsleep and poweroff.


== Implementation explained ==

Based on ESP8266 (ESP13, WROOM2). Any other ESP module is OK too except ESP-01.
As firmware, Esp-easy is used (due to my laziness). Since new Esp-easy mega is not pre-built for 512MB flash, the old release R147 is used. Good enough.

The data gethering is not effective at all. However, made quick and dirty.
The sensor made fully autonomous: it harvests sun light into energy.
Solar panel volrage implicitly hints light condition in the garden area.
Moisture data can be used in hub for watering calculations with PID loop.

Sensor reports Solar, Battery, and internal voltage as health check.
When internal voltage != 3.3V the regulator is broken.
When battery voltage < 3.4V the light charge is not enough.
