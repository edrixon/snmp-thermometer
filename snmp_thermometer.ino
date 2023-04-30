/*
    Ed's ESP8266 SNMP thermometer

    Written for Adafruit Feather Huzzah ESP8266

    Uses uSNMP agent


    Reads up to four Dallas DS18B20 one-wie temperature sensors and stores values in degrees centigrade
    Values can be read using SNMP OIDs below .1.3.6.1.4.1.56234.1

    Board also hosts a single web page to show status

    Blue LED flashes until ESP8266 connects to WiFi network
    Red LED flashes as each sensor is read (on for 200ms, every five seconds)

    One-wire bus on GPIO4
    "setup" switch on GPIO5

    Ground GPIO5 on power-up to enter configuration mode
    Board starts in WiFi AP mode with a web page to enter SSID, PSK and community string
    Configuration is stored in FLASH

*/


#include <SnmpAgent.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ESP8266WiFi.h>
#include <EEPROM.h>

#define VER_MAJOR       1
#define VER_MINOR       0

// Delay at end of loop()
#define TICK_MS         100

// How often to read temperature sensors - every (SENSOR_TICKS * TICK_MS) milliseconds
#define SENSOR_TICKS    50

// How long to leave LED on when turned on with ledOn() function
#define LED_TICKS       2

// Pin for blue LED
#define LED_WIFI        2

// Pin for 1-wire sensors
#define ONE_WIRE        4

// Pin for mode switch
#define MODE_SW         5

// 9 bit resolution
#define PRECISION       9

// SNMP agent configuration.
#define RO_COMMUNITY    "public"				  
#define RW_COMMUNITY    "private"  // Not used but needed by SNMP library

// 56234 is allocated by IANA
#define ENTERPRISE_OID  "B.56234"
#define SENSOR_OID      "P.56234.1.2.1"

// Maximum number of DS18B20 sensors
#define MAX_SENSORS     4

// Number of bytes in 1-wire address
#define ADDR_LEN        8

// WiFi defaults
#define DEFAULT_SSID    "ballacurmudgeon"
#define DEFAULT_PSK     "scaramanga"

// EEPROM data valid if it contains this
#define EEPROM_VALID    0x2367

// EEPROM layout
typedef struct
{
  int eepromSize;
  char staSSID[64];
  char staPSK[64];
  char roCommunity[64];
  unsigned short int dataValid;
} eepromType;

// Data stored for each sensor on 1-wire bus
typedef struct
{
    unsigned char addr[ADDR_LEN];    // Device address
    int temperature;                 // Last temperature read
} sensorType;

// HTTP GET request handlers
typedef struct
{
    char *fileName;
    void (*fn)(void);
} getRequestType;

// HTTP parameter setting handlers
typedef struct
{
    char *paramName;
    void (*fn)(char *);
} httpParamType;

void getWifiConfig();
void initMibTree();
int getUptime(MIB *);
int getTemperature(MIB *);
int getSensorAddress(MIB *);
void processLED();
void initSensors();
void processSensors();
void configurationMode();
void httpStartAP();
void httpHeader();
void httpFooter();
void httpConfigPage();
void httpResetConfiguration();
void httpSetSsid(char *ssid);
void httpSetPassword(char *password);
void httpSetRoCommunity(char *community);
void httpParseParam(char *paramName, char *paramValue);
void httpSaveConfiguration();
void httpNotFound();
void httpHandleGetRequest(char *url);
void httpWebServer();
void httpStatusPage(char *url);
void httpStatusGetRequest();

// EEPROM configuration
eepromType eepromData;

// Sensor data
sensorType sensors[MAX_SENSORS] =
{
    { "", 1111 },
    { "", 2222 },
    { "", 3333 },
    { "", 4444 }
};

// Number of sensors on bus
int numSensors;

// Which sensor is being polled in the current loop
int currentSensor;

// Tick timer counts
int ledTicks;
int sensorTicks;

// System description and location SNMP return values
char sysDescr[] =       "Ed's ESP8266 thermometer";
char sysLocation[] =    "Isle of Man";

OneWire oneWire(ONE_WIRE);
DallasTemperature dallasSensors(&oneWire);

char *httpParams[16];
char httpRequest[255];
boolean httpDone;

// Configuration mode HTML pages
getRequestType getRequests[] =
{
    { "/reset.html", httpResetConfiguration },
    { "/config.html", httpSaveConfiguration },
    { "/", httpConfigPage },
    { NULL, NULL }
};

// Handlers for config.html parameters
httpParamType httpParamHandlers[] =
{
    { "ssid", httpSetSsid },
    { "password", httpSetPassword },
    { "community", httpSetRoCommunity },
    { NULL, NULL }
};

WiFiServer httpServer(80);
WiFiClient httpClient;

void setup()
{
    int ledState;

    // Serial port
    Serial.begin(9600);
    delay(500);
   
    Serial.println("");
    Serial.println("");
    Serial.println("------------------------");
    Serial.println("Ed's ESP8266 thermometer");
    Serial.println("------------------------");
    Serial.println("");

    // Red LED
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, HIGH);
    ledTicks = 0;

    // Read saved wifi configuration
    // Gets data from EEPROM or uses default values if not found
    getConfig();

    // See if configuration mode needed
    pinMode(MODE_SW, INPUT_PULLUP);
    if(digitalRead(MODE_SW) == 0)
    {
        configurationMode();
    }
 
    // Start wifi client  
    Serial.print("  WiFi MAC address: ");
    Serial.println(WiFi.macAddress());
   
  	WiFi.mode(WIFI_STA); 
  	WiFi.begin(eepromData.staSSID, eepromData.staPSK);

  	Serial.print("  Trying to connect to SSID '");
    Serial.print(eepromData.staSSID);
    Serial.print("' . ");

    // Flash blue LED until wifi connects
    pinMode(LED_WIFI, OUTPUT);
    ledState = HIGH;
  	while(WiFi.status() != WL_CONNECTED)
  	{
        Serial.print(". ");
        digitalWrite(LED_WIFI, ledState);
        if(ledState == HIGH)
        {
            ledState = LOW;
        }
        else
        {
            ledState = HIGH;
        }
		    delay(200);
  	}
    Serial.println("OK");

    // Make sure blue LED is on
    digitalWrite(LED_WIFI, LOW);

    // Show wifi IP address
  	Serial.print("  IP address : ");
    Serial.println(WiFi.localIP());
    Serial.println("");

    // Work out how many sensors there are
    // Needs to be done before MIB tree is setup
    initSensors();

    // Initialise SNMP 
  	initSnmpAgent(SNMP_PORT, ENTERPRISE_OID, eepromData.roCommunity, RW_COMMUNITY);
  	initMibTree();

    // Start status page web server
    Serial.println("  Starting webserver...");
    httpServer.begin();

    Serial.println("Ready");
    Serial.println("");
}

void loop()
{	
    // Handle SNMP requests
	  processSNMP();

    // Read temperatures
    processSensors();

    // Check if LED needs turning off
    processLED();
  
    // If someone's connected to webserver, show the status page
    httpClient = httpServer.available();
    if(httpClient.connected())
    {
        httpStatusGetRequest();
        httpClient.flush(); 
        httpClient.stop();
    }

    // Wait one tick
    delay(TICK_MS);
}

void initMibTree()
{
	MIB *thisMib;
  char sensorOid[32];
  int c;
  int setVal;
  int oidIndex;

	// System MIB

	// System description
	thisMib = miblistadd(mibTree, "B.1.1.0", OCTET_STRING, RD_ONLY,
		sysDescr, strlen(sysDescr));

	// System up time
	thisMib = miblistadd(mibTree, "B.1.3.0", TIMETICKS, RD_ONLY, NULL, 0);
	setVal = 0;
	mibsetvalue(thisMib, &setVal, 0);
  mibsetcallback(thisMib, getUptime, NULL);

	// System location
  thisMib = miblistadd(mibTree, "B.1.6.0", OCTET_STRING, RD_ONLY,
  sysLocation, strlen(sysLocation));

  // Enterprise MIB
  for(c = 0; c < numSensors; c++)
  {
    oidIndex = c + 1;
    
    // Sensor bus address
    sprintf(sensorOid, "%s.2.%d", SENSOR_OID, oidIndex);
    thisMib = miblistadd(mibTree, sensorOid, OCTET_STRING, RD_ONLY,
      sensors[c].addr, ADDR_LEN);
    mibsetcallback(thisMib, getSensorAddress, NULL);

    // Temperature
    sprintf(sensorOid, "%s.3.%d", SENSOR_OID, oidIndex);
    thisMib = miblistadd(mibTree, sensorOid, INTEGER, RD_ONLY, NULL, 0);
    setVal = 0;
    mibsetvalue(thisMib, &setVal, 0);
    mibsetcallback(thisMib, getTemperature, NULL);
  }
}

// Gets system up time
int getUptime(MIB *thisMib)
{
  // Value in 100ths of a second
  thisMib -> u.intval = millis() / 10;
  
	return SUCCESS;
}

// Last temperature read by specified sensor
int getTemperature(MIB *thisMib)
{
  int c;
  
  c = (thisMib -> oid.array[thisMib -> oid.len - 1]) - 1; 
  thisMib -> u.intval = sensors[c].temperature;

  return SUCCESS;
}

// 1-wire address for specified sensor
int getSensorAddress(MIB *thisMib)
{
  int c;
  
  c = (thisMib -> oid.array[thisMib -> oid.len - 1]) - 1; 
  thisMib -> u.octetstring = sensors[c].addr;

  return SUCCESS;
}

// Turn LED off after LED_TICKS
void processLED()
{
  if(ledTicks)
  {
    ledTicks--;
    if(ledTicks == 0)
    {
      ledOff();
    }
  }
}

void ledOff()
{  
  digitalWrite(LED_BUILTIN, HIGH);
}

// Turn LED on and set timer for turning it off again
// "0" ticks means leave LED on forever
void ledOn(int ticks)
{
  ledTicks = ticks;
  digitalWrite(LED_BUILTIN, LOW);
}

// Print specified sensor address 
// Return 1 if address is a DS18B20, 0 if not
int printSensorAddress(unsigned char *devAddr)
{
  int c;
  int isDs18b20;

  if(*devAddr == 0x28)
  {
    isDs18b20 = 1;
  }
  else
  {
    isDs18b20 = 0;
  }

  c = 8;
  while(c)
  {
    if(*devAddr < 16)
    {
      Serial.print("0");
    }
    Serial.print(*devAddr, 16);
    Serial.print(" ");
    devAddr++;
    c--;
  }

  return isDs18b20;
}

// Count sensors and store addresses
void initSensors()
{
  int c;
  unsigned char *devAddr;
  
  sensorTicks = SENSOR_TICKS;
  currentSensor = 0;

  // Find out how many sensors are on the bus
  dallasSensors.begin();
  numSensors = dallasSensors.getDeviceCount();
  Serial.println("Locating temperature sensors... ");
  Serial.print("  Support for up to ");
  Serial.print(MAX_SENSORS, DEC);
  Serial.println(" devices");
  Serial.print("  Found ");
  Serial.print(numSensors, DEC);
  Serial.println(" devices");

  // Can only use "MAX_SENSORS" sensors
  // So ignore any extra ones...
  if(numSensors > MAX_SENSORS)
  {
    numSensors = MAX_SENSORS;
    Serial.print("  Using only first ");
    Serial.print(numSensors, DEC);
    Serial.println(" devices found");    
  }

  // Get and store sensor addresses
  for(c = 0; c < numSensors; c++)
  {
    devAddr = sensors[c].addr;
    dallasSensors.getAddress(devAddr, c);
    Serial.print("    ");
    Serial.print(c + 1, DEC);
    Serial.print(" - ");
    if(printSensorAddress(devAddr) == 1)
    {
      Serial.println("  (DS18B20)");
    }
    else
    {
      Serial.println("  (Unknown device type)");
    }

    dallasSensors.setResolution(devAddr, PRECISION);
  }
}

// Periodically read and store temperatures
void processSensors()
{
  float t;
  
  sensorTicks--;
  if(sensorTicks == 0)
  {
    sensorTicks = SENSOR_TICKS;
    ledOn(LED_TICKS);

    if(numSensors == 0)
    {
      Serial.println("No temperature sensors!!!!");
      return;
    }

    if(currentSensor == 0)
    {
      Serial.println("New polling cycle");
    }

    dallasSensors.requestTemperaturesByAddress(sensors[currentSensor].addr);

    Serial.print("  Sensor ");
    Serial.print(currentSensor + 1, DEC);
    Serial.print(" - ");
    
    // Read current sensor and store temperature
    t = dallasSensors.getTempC(sensors[currentSensor].addr);
    if(t == DEVICE_DISCONNECTED_C)
    {
      Serial.println("Error reading temperature");
    }
    else
    {
      Serial.print(t);
      Serial.println(" C");

      sensors[currentSensor].temperature = (int)(t * 10);
    }

    // Next sensor to be polled
    currentSensor++;
    if(currentSensor == numSensors)
    {
      currentSensor = 0;
      Serial.println("");
    }
  }
}

// Read saved SSID and password from EEPROM
// Use default values if nothing in EEPROM
void getConfig()
{
  unsigned char *dPtr;
  int eepromSize;
  int eepromAddr;
  
  eepromSize = sizeof(eepromData);
  EEPROM.begin(eepromSize);

  Serial.print("Reading ");
  Serial.print(eepromSize, DEC);
  Serial.println(" bytes from EEPROM");

  eepromAddr = 0;
  dPtr = (unsigned char *)&eepromData;
  while(eepromAddr < eepromSize)
  {
    *dPtr = EEPROM.read(eepromAddr);
    dPtr++;
    eepromAddr++;
  }

  if(eepromData.eepromSize == eepromSize && eepromData.dataValid == EEPROM_VALID)
  {
    Serial.println("  Found valid configuration");
  }
  else
  {
    Serial.println("  No configuration found in EEPROM");
    Serial.println("  Using default configuration");

    eepromData.eepromSize = eepromSize;
    strcpy(eepromData.staSSID, DEFAULT_SSID);
    strcpy(eepromData.staPSK, DEFAULT_PSK);
    strcpy(eepromData.roCommunity, RO_COMMUNITY);
    eepromData.dataValid = EEPROM_VALID;
  }
}

// Save configuration in EEPROM
void saveConfig()
{
  unsigned char *dPtr;
  int eepromAddr;

  Serial.print("Writing ");
  Serial.print(eepromData.eepromSize, DEC);
  Serial.println(" bytes to EEPROM");

  dPtr = (unsigned char *)&eepromData;
  eepromAddr = 0;
  while(eepromAddr < eepromData.eepromSize)
  {
    EEPROM.write(eepromAddr, *dPtr);
    dPtr++;
    eepromAddr++;
  }

  EEPROM.commit();
}

// "Setup" mode to edit EEPROM data through web page
void configurationMode()
{
  Serial.println("");
  Serial.println("Configuration mode");
  Serial.println("");
  
  ledOn(0);
  ESP.wdtDisable();
  
  httpStartAP();
  httpWebServer();
  
  while(1);
}

// Start wifi in "access point" mode
void httpStartAP()
{
  IPAddress tmpIPAddr(192, 168, 69, 99);
  IPAddress tmpGw(192, 168, 69, 1);
  IPAddress tmpNetmask(255, 255, 255, 0);
  char tmpSSID[64];
  unsigned char macAddr[6];

  Serial.println("Starting WiFi in access point mode");

  WiFi.macAddress(macAddr);
  sprintf(tmpSSID, "ESP8266-%02x-%02x-%02x", macAddr[3], macAddr[4], macAddr[5]);
  Serial.print("  SSID: ");
  Serial.println(tmpSSID);
  WiFi.softAPConfig(tmpIPAddr, tmpGw, tmpNetmask);
  WiFi.softAP(tmpSSID);
  Serial.print("  IP address: ");
  Serial.println(WiFi.softAPIP());
}

// Send header for configuration web pages
void httpHeader()
{
    httpClient.println("HTTP/1.1 200 OK");
    httpClient.println("Content-type:text/html");
    httpClient.println();
  
    httpClient.println("<html>");
    httpClient.println("<body>");
    
    httpClient.println("<h2>ESP8266 thermometer configuration</h2>");  
}

// Send footer for configuration web pages
void httpFooter()
{
    httpClient.println("<p><a href=\"/\">Back</a> to main page</p>");
    httpClient.println("</body>");
    httpClient.println("</html>");  
}

// Send main body for configuration page
void httpConfigPage()
{
    unsigned char macAddr[6];
    char macAddrStr[32];

    WiFi.macAddress(macAddr);
    
    httpHeader();

    sprintf(macAddrStr, "%02X:%02X:%02X:%02X:%02X:%02X", macAddr[0], macAddr[1], macAddr[2], macAddr[3], macAddr[4], macAddr[5]);
    
    httpClient.println("<p>MAC address:</p>");
    httpClient.print(macAddrStr);
    httpClient.println("<p></p>");
    
    httpClient.println("<form action=\"/config.html\">");
    httpClient.println("<label for=\"ssid\">WiFi SSID:</label><br>");
    httpClient.print("<input type=\"text\" id=\"ssid\" name=\"ssid\" value=\"");
    httpClient.print(eepromData.staSSID);
    httpClient.println("\"><br><br>");
    httpClient.println("<label for=\"password\">WiFi Password:</label><br>");
    httpClient.print("<input type=\"text\" id=\"password\" name=\"password\" value=\"");
    httpClient.print(eepromData.staPSK);
    httpClient.println("\"><br><br>");
    httpClient.println("<label for=\"community\">SNMP community:</label><br>");
    httpClient.print("<input type=\"text\" id=\"community\" name=\"community\" value=\"");
    httpClient.print(eepromData.roCommunity);
    httpClient.println("\"><br><br>");
    httpClient.println("<input type=\"submit\" value=\"Save settings\">");
    httpClient.println("</form>");

    httpClient.println("<br>");

    httpClient.println("<form action=\"/reset.html\">");
    httpClient.println("<input type=\"submit\" value=\"Default settings\">");
    httpClient.println("</form>");

    httpClient.println("</body>");
    httpClient.println("</html>");
}

// Send main body of "reset" page
// Load default configuration data
void httpResetConfiguration()
{
    httpHeader();
    httpClient.println("<p>Load default configuration</p>");
    httpFooter();

    strcpy(eepromData.staSSID, DEFAULT_SSID);
    strcpy(eepromData.staPSK, DEFAULT_PSK);
    strcpy(eepromData.roCommunity, RO_COMMUNITY);
}

// Load SSID from web page into configuration
void httpSetSsid(char *ssid)
{
    strcpy(eepromData.staSSID, ssid);
}

// Load wifi password from web page into configation
void httpSetPassword(char *password)
{
    strcpy(eepromData.staPSK, password);
}

// Load SNMP community string from web page into configuration
void httpSetRoCommunity(char *community)
{
    strcpy(eepromData.roCommunity, community);
}

// Check and action parameter passed to configuration web page
void httpParseParam(char *paramName, char *paramValue)
{
    int c;

    Serial.print("Trying to set ");
    Serial.print(paramName);
    Serial.print(" to ");
    Serial.println(paramValue);

    c = 0;
    while(httpParamHandlers[c].paramName != NULL && strcmp(httpParamHandlers[c].paramName, paramName) != 0)
    {
        c++;
    }

    if(httpParamHandlers[c].paramName != NULL)
    {
        httpParamHandlers[c].fn(paramValue);
    }
    else
    {
        Serial.print("Bad parameter");
    }
}

// Action "save configuration" button on webpage
void httpSaveConfiguration()
{
    int c;
    char *paramName;
    char *paramValue;
    
    httpHeader();
    httpClient.println("<p>Save configuration:</p><br>");

    for(c = 1; c < 4; c++)
    {
        paramName = strtok(httpParams[c], "=");
        paramValue = strtok(NULL, "=");

        httpClient.print("<p>Set '");
        httpClient.print(paramName);
        httpClient.print("' to '");
        httpClient.print(paramValue);
        httpClient.println("'</p><br>");
        
        httpParseParam(paramName, paramValue);
    }
    
    httpFooter();

    saveConfig();
}

// Handle unknown HTTP request
void httpNotFound()
{
    httpClient.println("HTTP/1.1 404 Not Found");
}

// HTTP GET handler
void httpHandleGetRequest(char *url)
{
    char *tokPtr;
    char *filename;
    int c;
  
    c = 0;

    tokPtr = strtok(url, "?& ");
    while(tokPtr != NULL)
    {
        httpParams[c] = tokPtr;
        Serial.print("Param ");
        Serial.print(c);
        Serial.print(": ");
        Serial.println(tokPtr);
        c++;

        tokPtr = strtok(NULL, "?& ");
    }

    c = 0;
    while(getRequests[c].fileName != NULL && strcmp(getRequests[c].fileName, httpParams[0]) != 0)
    {
        c++;
    }

    if(getRequests[c].fileName == NULL)
    {
        httpNotFound();
    }
    else
    {
        getRequests[c].fn();
    }
}

// Configuration web server
void httpWebServer()
{
    char c;
    char *urlPtr;
    unsigned long int ms;

    Serial.println("  Starting webserver...");
  
    // Start webserver
    httpServer.begin();

    httpDone = false;
    ms = millis();
    while(httpDone == false)
    {
        // If there's a client
        httpClient = httpServer.available();
        if(httpClient)
        {
            urlPtr = httpRequest;
            while(httpClient.connected())
            {
                // Read HTML request with timeout
                if(httpClient.available())
                {
                    ms = millis();
                    c = httpClient.read();
                    if(c == '\n')
                    {
                        *urlPtr = '\0';

                        Serial.println(httpRequest);
                        
                        if(strncmp(httpRequest, "GET", 3) == 0)
                        {
                            // Do the request
                            httpHandleGetRequest(&httpRequest[4]);
                        }

                        httpClient.flush();
                        httpClient.stop();
                    }
                    else
                    {
                        if(c != '\r')
                        {
                            *urlPtr = c;
                            urlPtr++;
                        }
                    }
                }
                else
                {
                    if(ms - millis() > 2000)
                    {
                        Serial.println("HTTP idle timeout");
                        httpClient.stop();
                    }
                }
            }

            Serial.println("Disconnected");
        }
    }
}

// The "status page"
// This is the only thing served by the web server in normal use
void httpStatusPage(char *url)
{
    char c;
    char *filename;
    char txtBuff[80];
    unsigned char macAddr[6];

    filename = strtok(url, "?& ");
    if(strcmp(filename, "/") != 0)
    {
        httpNotFound();
    }
    else
    {
        httpClient.println("HTTP/1.1 200 OK");
        httpClient.println("Content-type:text/html\r\n");
  
        httpClient.println("<html>");

        httpClient.println("<head>");
        httpClient.println("<style>");
        httpClient.println("table, th, td {");
        httpClient.println("  border: 1px solid black;");
        httpClient.println("  border-collapse: collapse;");
        httpClient.println("  padding: 5px;");
        httpClient.println("  text-align: left;");
        httpClient.println("}");
        httpClient.println("</style>");
        httpClient.println("</head>");

        httpClient.println("<body>");
    
        httpClient.println("<h2>ESP8266 Thermometer Status</h2>");
          
        httpClient.println("<br>");
        
        httpClient.println("<table>");
        httpClient.println("  <tr>");
        httpClient.println("    <th>WiFi SSID</th>");
        sprintf(txtBuff, "    <td>%s</td>", eepromData.staSSID);
        httpClient.println(txtBuff);
        httpClient.println("  </tr>");
        httpClient.println("  <tr>");
        httpClient.println("    <th>WiFi RSSI</th>");
        sprintf(txtBuff, "    <td>%d dBm</td>", WiFi.RSSI());
        httpClient.println(txtBuff);
        httpClient.println("  </tr>");
        httpClient.println("  <tr>");
        httpClient.println("    <th>MAC address</th>");
        WiFi.macAddress(macAddr);
        sprintf(txtBuff, "<td>%02X:%02X:%02X:%02X:%02X:%02X</td>", macAddr[0], macAddr[1], macAddr[2], macAddr[3], macAddr[4], macAddr[5]);
        httpClient.println(txtBuff);
        httpClient.println("  </tr>");
        httpClient.println("</table>");

        httpClient.println("<br>");
        
        httpClient.println("<table>");
        httpClient.println("  <tr>");
        httpClient.println("    <th>Number of sensors</th>");
        sprintf(txtBuff, "    <td>%d</td>", numSensors);
        httpClient.println(txtBuff);
        httpClient.println("  </tr>");
        for(c = 0; c < numSensors; c++)
        {
            httpClient.println("  <tr>");
            sprintf(txtBuff, "    <th>%d</th>", c + 1);
            httpClient.println(txtBuff);
            sprintf(txtBuff, "    <td>%0.1f degrees</td>", sensors[c].temperature / 10.0);
            httpClient.println(txtBuff);
            sprintf(txtBuff, "    <td>%02X-%02X-%02X-%02X-%02X-%02X-%02X-%02X</td>",
                sensors[c].addr[0],
                sensors[c].addr[1],
                sensors[c].addr[2],
                sensors[c].addr[3],
                sensors[c].addr[4],
                sensors[c].addr[5],
                sensors[c].addr[6],
                sensors[c].addr[7]);
            httpClient.println(txtBuff);
            if(sensors[c].addr[0] == 0x28)
            {
              httpClient.println("    <td>DS18B20 family</td>");
            }
            else
            {
              httpClient.println("    <td>Unknown device type</td>");
            }
            httpClient.println("  </tr>");
        }
        httpClient.println("</table>");

        httpClient.println("<br>");
        
        httpClient.println("<p>");
        httpClient.println("Power up holding 'setup' button to enter configuration mode");
        httpClient.println("</p>");
        httpClient.println("<p>");
        httpClient.println("Thermometer will start WiFi access point with SSID 'ESP8266-XXXX'<br>");
        httpClient.println("Connect to that to access configuration page at http://192.168.69.99");

        httpClient.println("<br><br><p>");
        sprintf(txtBuff, "Firmware version %d.%d, Ed Rixon", VER_MAJOR, VER_MINOR);
        httpClient.println(txtBuff);
        httpClient.println("</p>");

        httpClient.println("</body>");
        httpClient.println("</html>");  
    }
}

void httpStatusGetRequest()
{
    char c;
    char *urlPtr;
    unsigned long int ms;

    Serial.println("Client connected to webserver");
  
    urlPtr = httpRequest;
    ms = millis();
    while(httpClient.connected())
    {
        if(httpClient.available())
        {
            ms = millis();
            c = httpClient.read();
            if(c == '\n')
            {
                *urlPtr = '\0';

                Serial.println(httpRequest);
                        
                if(strncmp(httpRequest, "GET", 3) == 0)
                {
                    httpStatusPage(&httpRequest[4]);
                }

                httpClient.flush();
                httpClient.stop();
            }
            else
            {
                if(c != '\r')
                {
                    *urlPtr = c;
                    urlPtr++;
                }
            }
        }
        else
        {
            if(millis() - ms > 2000)
            {
                Serial.println("HTTP idle timeout");
                httpClient.stop();
            }
        }
    }
    
    Serial.println("Disconnected");
}
