# ESP8266 based Smart Home System
## Principle
### Types of smart home
- Centralized - there is one smart hub in the house and bunch of perfieral devices just perform its commands.
- Decentralized - every edge device is smart and self-contained.
- Cloud - the same as centralized, just the hub is not in your house... with all pros and cons.
- Hybrid - every edge device is smart enough to perform on its own, plus it communicates with the hub in your house or in cloud when/if needed.

We develop the **Hybrid Smart Home**, as the most resilient, safe, and serviceable system.

### What is Smart?

Today every relay connected to Bluetooth called "smart". De facto, this type of "smart" is actually "controlled" device. It is controlled by humans via phone/app/cloud/vocal assistant. So human is smart in this case, not the device.

Smart home consists devices (edge peripherals, hub(s), and cloud connection) which are able to learn habitants behavior, preferences, rules, and perform accordingly to them automatically.

**Smart home must serve people**, not people should care about smart home.

The main concept of our smart home is:
### Home must serve you. You should enjoy ###

This is what this project about.

## Active parts
- [Room sensor](https://github.com/igrowing/esp8266_smart_home_ready/tree/master/modules/room_sensor) based on ESP-easy.
- [Room sensor](https://github.com/igrowing/esp8266_smart_home_ready/tree/master/modules/room%20sensor%20v2) based on Homie IoT.
- [Leakage detector, door/window sensor, remote bell button, etc.](https://github.com/igrowing/esp8266_smart_home_ready/tree/master/modules/leakage_detector) based on ESP-easy.
- [Remote light switch](https://github.com/igrowing/esp8266_smart_home_ready/tree/master/modules/remote%20light%20switch) based on Homie IoT.
- [Sewage pump controller](https://github.com/igrowing/esp8266_smart_home_ready/tree/master/modules/sewage%20pump%20controller) based on Homie IoT.
- [Garden sensor](https://github.com/igrowing/esp8266_smart_home_ready/tree/master/modules/garden_sensor) based on ESP-easy.
- [Water monitor and valve, Schieber](https://github.com/igrowing/esp8266_smart_home_ready/tree/master/modules/schieber) based on Homie IoT.
- [Boilero: the really smart boiler](https://github.com/igrowing/esp8266_smart_home_ready/tree/master/modules/boilero) based on Homie IoT.

## Abandoned part

Spanish proverb says: "Wise man changes his mind, a fool never".
We admit that struggling over the same problems as other developers is not effective way for progress.
Therefore we picked 2 the most usable frameworks (EspEasy and Homie) to implement ESP8266 based part of the smart home.

This impacts a little the fully autonomous OTA idea.
However, this allows us to move faster to end-to-end solution.

## The goal
Make fully automated OTA for ESP8266 under NodeMCU.

The software upload and update to the ESP8266 is trivial via UART. This way is not applicable for mass-deployed modules.

There are geek-driven solutions for OTA: the human (engineer or skilled person) should initiate file upload one-by-one to every particular module. This is not applicable too: the human is unable to do that for hundreds and thousands of modules, tens of files per module.

Therefore, module should initiate itself on the server (register) and request updates on periodic basis. Once developer releases the update to the server, every single ESP8266 module should update itself asynchronously.

### Implementation
**ESP8266 side:** Lua files to be uploaded to the module during "manufacturing" process.

**Server side:** The server files to be installed (copied) to the local server during initial installation process.

## Initial configuration and use
On the server run `smart-home-server.py file_repository`.

The ESP8266 module once powered up will run as AP (access point named SHMxxxxxx, where x-es stand for ID number of the module). Connect ot the module via any browser to address 192.168.4.1. Fill mandatory (and optional if needed) fields. Click Save button. That's it.

The ESP8266 module will restart and connect the server looking for the update. Once new files placed into the file repository the module will download them and run the code.
