# ESP32 Relay (Garage Door) Example

Sample code that does the following:

- Controls a single Relay (when triggered, closes then opens the relay)
- SoftAP mode (you can connect directly to the ESP32)
- ClientAP mode (will also connect to your home LAN)

## Reference Hardware

This software was developed using the following two pieces of hardware:

- Adafruit HUZZAH32 – ESP32 Feather Board (https://www.adafruit.com/product/3405)
- Adafruit Non-Latching Mini Relay FeatherWing (https://www.adafruit.com/product/2895)

## Building

- Set up ESP32 build environment (see: http://esp-idf.readthedocs.io/en/latest/get-started/)
- Check out the code under your esp development area.
```
cd ~/esp && git clone https://github.com/paul-blankenbaker/esp32-iot-relay.git
```
- Configure
```
cd ~/esp/esp32-iot-relay && make menuconfig
```
- Build, flash and monitor
```
make all flash monitor
```

## Verifying

### Verify SoftAP Mode

- Use WIFI or smart phone to connect to ESP32 Access Point (look for SSID that you configured and use the password you set for AP Mode in the configuration).
- After connecting to ESP32 Access Point, open: http://192.168.4.1/ in browser
- Press the button in web browser, you should hear the relay click on and then off.

### Verify ClientAP Mode

- Plug ESP32 into your system and open up serial console output via:
```
cd ~/esp/esp32-iot-relay && make monitor
```
- ESP32 will attempt to connect to SSID configured and will accept a DHCP IP address.
- The address assigned on your network should be echoed to the console.
- Verify you can open a browser to the reported IP address.
- Press the button in web browser, you should hear the relay click on and then off.
- Update your DHCP server to assign a specific IP address to the ESP32 and reboot the ESP32.
- Verify that the expected address was assigned and that you can still control the relay
