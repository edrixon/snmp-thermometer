An SNMP accessible thermometer for Dallas DS18B20 sensors and the Adafruit ESP8266 Huzzah Feather 
  Uses uSNMP agent

Supports up to 4 sensors, number is configurable.

Can be booted with wifi interface in AP mode to allow configuration with web page.  WiFi configuration
is stored on FLASH.
Reads each sensor in turn with a configurable cycle period, one sensor is read per cycle
Values are stored and the stored values can be read via SNMP

Has a very basic status page web server but it is intended that the data is read solely via SNMP.

