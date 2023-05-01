An SNMP accessible thermometer for Dallas DS18B20 sensors for Arduino
Made for the Adafruit Feather HUZZAH with ESP8266 but probably works with any WiFi Arduino 
  Uses uSNMP agent

Default values as #define's in .ino file
  pin 4 - GPIO for 1-wire interface
  pin 5 - GPIO for "setup mode" button
  
Uses red led to show polling activity and blue led to show WiFi status

Supports up to 4 sensors, number is configurable.

Can be booted with wifi interface in AP mode to allow configuration with web page.  WiFi configuration
is stored on FLASH.

hold "setup mode" button when powering up to enter setup mode.  Connect to WiFi AP, config url is http://192.168.69.99

Reads each sensor in turn with a configurable cycle period, one sensor is read per cycle
Values are stored and the stored values can be read via SNMP

Has a very basic status page web server but it is intended that the data is read solely via SNMP.

Uses DHCP for IP address, if you want static IP then set it up in DHCP server
Doesn't support alarms etc, if you want those, use your SNMP manager
