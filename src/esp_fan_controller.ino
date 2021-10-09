/********************************************************************/
#include <OneWire.h>
#include <DallasTemperature.h>

#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>

#include <ESP8266WebServer.h>
#include <WiFiManager.h>
#include <DNSServer.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <LittleFS.h>

/********************************************************************/
// 1w pin pin 2 on the Arduino (phys pin d4)
#define ONE_WIRE_BUS 2

// transistor on pin D2 / arduino pin 4
#define FAN_PWR_TRANSISTOR 4

char deviceName[64] = "esp-fan-controller";

// <= diff between sensors, <= this it turns off
char fanOffTempDiff[4] = "10";

// num of seconds to wait between changes
char fanChangeDelay[6] = "60";

// fan speed (0-1023)
char fanDuty[5] = "300";

/********************************************************************/
// project hame and semantic version
#define PROJECT_NAME "ESP Fan Controller"
#define FW_VERSION "1.1.0"

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature temp_sensors(&oneWire);

// stores time of last change
// will wrap around 50 days
unsigned long lastChange = 0;
bool fanStatus = false;
float currentTriggerTemp = 0;
float currentAmbiantTemp = 0;

// address for two sensors
DeviceAddress ambiantTempSensorAddr;
DeviceAddress triggerTempSensorAddr;

unsigned long nextSensorCheck = 0;
int failedSensorCount = 0;
bool initFanAction = true;
String fanStatusHuman = "off";
int32_t rssi = 0;

int sensorFailCount = 0;

bool sensorNotFound = false;

// script to auto refresh sensor values and status
#define AUTO_REFRESH_SCRIPT "<script>"                                                                                              \
                            "setInterval(function() {"                                                                              \
                            "fetch('/status')"                                                                                      \
                            ".then(function (response) {"                                                                           \
                            "return response.json();"                                                                               \
                            "})"                                                                                                    \
                            ".then(function (jsonData) {"                                                                           \
                            "document.getElementById('temp_sensor_trigger').textContent = jsonData.temp_sensor_trigger.toFixed(2);" \
                            "document.getElementById('temp_sensor_ambiant').textContent = jsonData.temp_sensor_ambiant.toFixed(2);" \
                            "document.getElementById('fan_status_human').textContent = jsonData.fan_status_human;"                  \
                            "document.getElementById('wifi_rssi').textContent = jsonData.wifi_rssi;"                                \
                            "})"                                                                                                    \
                            ".catch(function (error) {"                                                                             \
                            "console.log(' Error : ' + error);"                                                                     \
                            "});"                                                                                                   \
                            "}, 5000);"                                                                                             \
                            "</script>"

/********************************************************************/

ESP8266WebServer webServer(80);

void setup()
{
  Serial.begin(115200);

  Serial.print(PROJECT_NAME);
  Serial.print(" v");
  Serial.println(FW_VERSION);

  temp_sensors.begin();

  // change pwm freq to reduce motor noise
  analogWriteFreq(20000);
  pinMode(FAN_PWR_TRANSISTOR, OUTPUT);

  loadConfig();
  Serial.println("Device name: " + String(deviceName));

  WiFiManager wifiManager;
  wifiManager.autoConnect(deviceName);

  ArduinoOTA.setHostname(deviceName);

  ArduinoOTA.onStart([]()
                     {
                       String type;
                       if (ArduinoOTA.getCommand() == U_FLASH)
                       {
                         type = "sketch";
                       }
                       else
                       { // U_FS
                         type = "filesystem";
                       }

                       // NOTE: if updating FS this would be the place to unmount FS using FS.end()
                       Serial.println("Start updating " + type);
                     });
  ArduinoOTA.onEnd([]()
                   { Serial.println("\nEnd"); });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total)
                        { Serial.printf("Progress: %u%%\r", (progress / (total / 100))); });
  ArduinoOTA.onError([](ota_error_t error)
                     {
                       Serial.printf("Error[%u]: ", error);
                       if (error == OTA_AUTH_ERROR)
                       {
                         Serial.println("Auth Failed");
                       }
                       else if (error == OTA_BEGIN_ERROR)
                       {
                         Serial.println("Begin Failed");
                       }
                       else if (error == OTA_CONNECT_ERROR)
                       {
                         Serial.println("Connect Failed");
                       }
                       else if (error == OTA_RECEIVE_ERROR)
                       {
                         Serial.println("Receive Failed");
                       }
                       else if (error == OTA_END_ERROR)
                       {
                         Serial.println("End Failed");
                       }
                     });
  ArduinoOTA.begin();

  webServer.on("/", webHandleRoot);
  webServer.on("/settings", webHandleSettings);
  webServer.on("/saveConfig", webHandleSaveConfig);
  webServer.on("/reboot", webHandleReboot);
  webServer.on("/status", webHandleStatus);
  webServer.onNotFound(webHandleNotFound);
  webServer.begin();

  if (MDNS.begin("esp8266"))
  {
    Serial.println("MDNS responder started");
  }

  Serial.println("Ready");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  Serial.print("Found ");
  Serial.print(String(temp_sensors.getDeviceCount()));
  Serial.println(" temp sensors");
}

void loop()
{
  ArduinoOTA.handle();

  webServer.handleClient();
  MDNS.update();

  if (millis() >= nextSensorCheck)
  {
    rssi = WiFi.RSSI();

    temp_sensors.requestTemperatures(); // Send the command to get temperature readings

    currentAmbiantTemp = temp_sensors.getTempC(ambiantTempSensorAddr);
    currentTriggerTemp = temp_sensors.getTempC(triggerTempSensorAddr);

    if ((currentTriggerTemp == -127 || currentAmbiantTemp == -127) && !sensorNotFound)
    {
      sensorFailCount++;
    }
    else
    {
      sensorFailCount = 0;
    }

    // reset if sensor isn't giving good data
    if (sensorFailCount > 10)
    {
      Serial.println("Resetting due to bad sensor data");
      espReset();
    }

    int fanValue = 0;

    // turn on fan if trigger temp is x degrees above ambiant temp (and sensors have valid data)
    if ((currentTriggerTemp != -127 && currentAmbiantTemp != -127) && (currentTriggerTemp - currentAmbiantTemp > atoi(fanOffTempDiff)))
    {
      fanValue = atoi(fanDuty);
    }

    // initFanAction is used to prevent a startup delay
    if (bool(fanValue) != fanStatus && (initFanAction || millis() - lastChange > (atoi(fanChangeDelay) * 1000)))
    {
      if (bool(fanValue))
      {
        // fan should be on
        setFanValue(fanValue);
        fanStatusHuman = "on";
      }
      else
      {
        // fan should be off
        setFanValue(fanValue);
        fanStatusHuman = "off";
      }
    }

    // only check every 5s to prevent too much blocking.
    nextSensorCheck = millis() + 5000;
  }
}

void setFanValue(int fanValue)
{
  initFanAction = false;
  analogWrite(FAN_PWR_TRANSISTOR, fanValue);
  fanStatus = bool(fanValue);
  lastChange = millis();
}

/********************************************************************/
void webHandleNotFound()
{ // webServer 404
  String httpMessage = "File Not Found\n\n";
  httpMessage += "URI: ";
  httpMessage += webServer.uri();
  httpMessage += "\nMethod: ";
  httpMessage += (webServer.method() == HTTP_GET) ? "GET" : "POST";
  httpMessage += "\nArguments: ";
  httpMessage += webServer.args();
  httpMessage += "\n";
  for (uint8_t i = 0; i < webServer.args(); i++)
  {
    httpMessage += " " + webServer.argName(i) + ": " + webServer.arg(i) + "\n";
  }
  webServer.send(404, "text/plain", httpMessage);
}

/********************************************************************/
// from https://lastminuteengineers.com/multiple-ds18b20-arduino-tutorial/
String sensorAddrToStr(DeviceAddress deviceAddress)
// convert sensor address in DeviceAddress format to string
{
  String devAddrStr;
  for (uint8_t i = 0; i < 8; i++)
  {
    if (deviceAddress[i] < 0x10)
      devAddrStr += "0";
    devAddrStr += String(deviceAddress[i], HEX);
    if (i < 7)
      devAddrStr += ":";
  }
  return devAddrStr;
}

/********************************************************************/

void sensorStrToDeviceAddress(DeviceAddress convertedDevAddr, String devAddrStr)
// convert sensor address in string format to DeviceAddress
{
  // based on https://forum.arduino.cc/t/ds18b20-address-as-string-convert-to-hex-so-usable-as-address/200089/5
  const char *devAddrChar = devAddrStr.c_str();

  int addrv[8];
  sscanf(devAddrChar, "%x:%x:%x:%x:%x:%x:%x:%x",
         &addrv[0], &addrv[1], &addrv[2], &addrv[3],
         &addrv[4], &addrv[5], &addrv[6], &addrv[7]); // parse the 8 ascii hex bytes in 8 ints

  for (uint8_t i = 0; i < 8; i++)
  {
    convertedDevAddr[i] = (__typeof__(convertedDevAddr[0]))addrv[i]; //fill in device address bytes using a cast
  }
}

/********************************************************************/
String generateStartHtml(String titleSuffix = "", bool includeRefreshScript = false, String redirectToUrl = "")
{
  // generate everything up to and including <body> tag

  String httpContent = "<html><head><title>";
  httpContent += PROJECT_NAME;

  if (titleSuffix != "")
  {
    httpContent += " - " + titleSuffix;
  }
  httpContent += "</title>";

  if (includeRefreshScript)
  {
    httpContent += AUTO_REFRESH_SCRIPT;
  }

  if (redirectToUrl != "")
  {
    httpContent += "<meta http-equiv=\"refresh\" content=\"5; url=" + redirectToUrl + "\" />";
  }

  httpContent += "<meta http-equiv='Content-Type' content='text/html; charset=utf-8'/>";
  httpContent += "</head><body>";

  return httpContent;
}

/********************************************************************/
void webHandleRoot()
{

  String httpMessage = generateStartHtml("", true);
  if (sensorNotFound)
  {
    httpMessage += "<h3><bold>NOTICE:<bold> one or more sensors is not configured or is configure but missing from the bus. Please configure sensor values under settings or check connection.</h3><br>";
  }
  httpMessage += "<h3>" + String(deviceName) + "</h3>";
  httpMessage += "Trigger Sensor Temp: <span id='temp_sensor_trigger'>";
  httpMessage += currentTriggerTemp;
  httpMessage += "</span> <br>";
  httpMessage += "Ambiant Sensor Temp: <span id='temp_sensor_ambiant'>";
  httpMessage += currentAmbiantTemp;
  httpMessage += "</span> <br>";
  httpMessage += "Fan Status:  <span id='fan_status_human'>";
  httpMessage += fanStatusHuman;
  httpMessage += "</span><br>";
  httpMessage += "Wifi Signal Strength:  <span id='wifi_rssi'>";
  httpMessage += rssi;
  httpMessage += "</span><br>";
  httpMessage += "<br><hr><i>";
  httpMessage += PROJECT_NAME;
  httpMessage += " v";
  httpMessage += FW_VERSION;
  httpMessage += "</i><br><br><button onclick=\"window.location.href='/settings';\">Settings</button>";

  httpMessage += "</body></html>";

  webServer.send(200, "text/html", httpMessage);
}

/********************************************************************/
void webHandleSettings()
{

  String httpMessage = generateStartHtml("Settings");

  httpMessage += "<form method='POST' action='saveConfig'>";

  httpMessage += "<label for='deviceName'>Name of this device. Must be valid DNS name. Requires a restart to take effect.</label> <br>";
  httpMessage += "<input id='deviceName' name='deviceName' maxlength=64 type='text' value='" + String(deviceName) + "'><br>";

  httpMessage += "<br> <label for='fanOffTempDiff'>Diff threshold between two sensors. Fan will turn on when the trigger sensor is this many degrees above the ambiant sensor.</label> <br>";
  httpMessage += "<input id='fanOffTempDiff' name='fanOffTempDiff' maxlength=2 type='number' min='0' value='" + String(fanOffTempDiff) + "'><br>";

  httpMessage += "<br> <label for='fanChangeDelay'>Time in seconds between fan state changes.</label> <br>";
  httpMessage += "<input id='fanChangeDelay' name='fanChangeDelay' maxlength=4 min='0'type='number' value='" + String(fanChangeDelay) + "'><br>";

  httpMessage += "<br> <label for='fanDuty'>Fan speed (pwm range from 0-1023).</label> <br>";
  httpMessage += "<input id='fanDuty' name='fanDuty' maxlength=4 type='number' min='0' max='1023' value='" + String(fanDuty) + "'><br>";

  httpMessage += "<br> <label for='triggerSensor'>Trigger Temperature Sensor.</label> <br>";
  httpMessage += "<select name='triggerSensor' id='triggerSensor'>";

  bool sensor_found_in_cfg = false;
  for (int i = 0; i < temp_sensors.getDeviceCount(); ++i)
  {
    DeviceAddress currentDeviceAddr;
    float tempC;
    temp_sensors.getAddress(currentDeviceAddr, i);
    tempC = temp_sensors.getTempCByIndex(i);

    httpMessage += "<option value='" + sensorAddrToStr(currentDeviceAddr) + "'";

    if (sensorAddrToStr(currentDeviceAddr) == sensorAddrToStr(triggerTempSensorAddr))
    {
      sensor_found_in_cfg = true;
      httpMessage += "selected='selected'";
    }
    httpMessage += ">" + sensorAddrToStr(currentDeviceAddr) + " (" + String(tempC) + "&#176;)</option>";
  }

  if (!sensor_found_in_cfg)
  {
    httpMessage += "<option value='' selected='selected'> - </option>";
  }
  else
  {
    httpMessage += "<option value=''> - </option>";
  }

  httpMessage += "</select><br>";

  httpMessage += "<br> <label for='ambiantSensor'> Room Temperature Sensor</label> <br>";
  httpMessage += "<select name='ambiantSensor' id='ambiantSensor'>";

  sensor_found_in_cfg = false;
  for (int i = 0; i < temp_sensors.getDeviceCount(); ++i)
  {
    DeviceAddress currentDeviceAddr;
    float tempC;
    temp_sensors.getAddress(currentDeviceAddr, i);
    tempC = temp_sensors.getTempCByIndex(i);

    httpMessage += "<option value='" + sensorAddrToStr(currentDeviceAddr) + "'";

    if (sensorAddrToStr(currentDeviceAddr) == sensorAddrToStr(ambiantTempSensorAddr))
    {
      sensor_found_in_cfg = true;
      httpMessage += "selected='selected'";
    }
    httpMessage += ">" + sensorAddrToStr(currentDeviceAddr) + " (" + String(tempC) + "&#176;)</option>";
  }

  if (!sensor_found_in_cfg)
  {
    httpMessage += "<option value='' selected='selected'> - </option>";
  }
  else
  {
    httpMessage += "<option value=''> - </option>";
  }

  httpMessage += "</select><br>";

  httpMessage += "<br><button type='submit'>Save Settings</button></form>";
  httpMessage += "<button onclick=\"window.location.href='/';\">Cancel</button><br>";

  httpMessage += "<br><hr></br>";

  httpMessage += "<button onclick=\"window.location.href='/reboot';\">Reboot</button>";
  httpMessage += "</body></html>";

  webServer.send(200, "text/html", httpMessage);
}

/********************************************************************/
void webHandleReboot()
{

  String httpMessage = generateStartHtml("Reboot", false, "/");
  httpMessage += "Rebooting. Please wait... ";
  httpMessage += "</body></html>";
  webServer.send(200, "text/html", httpMessage);
  delay(1000);
  espReset();
}

/********************************************************************/
void webHandleSaveConfig()
{
  bool shouldSaveConfig = false;

  if (webServer.arg("deviceName") != "" && webServer.arg("deviceName") != String(deviceName))
  {
    shouldSaveConfig = true;
    webServer.arg("deviceName").toCharArray(deviceName, 64);
  }

  if (webServer.arg("fanOffTempDiff") != "" && webServer.arg("fanOffTempDiff") != String(fanOffTempDiff))
  {
    shouldSaveConfig = true;
    webServer.arg("fanOffTempDiff").toCharArray(fanOffTempDiff, 4);

    if (atoi(fanOffTempDiff) < 0)
    {
      String('0').toCharArray(fanOffTempDiff, 4);
    }
  }

  if (webServer.arg("fanChangeDelay") != "" && webServer.arg("fanChangeDelay") != String(fanChangeDelay))
  {
    shouldSaveConfig = true;
    webServer.arg("fanChangeDelay").toCharArray(fanChangeDelay, 6);

    if (atoi(fanChangeDelay) < 0)
    {
      String('0').toCharArray(fanChangeDelay, 6);
    }
  }

  if (webServer.arg("fanDuty") != "" && webServer.arg("fanDuty") != String(fanDuty))
  {
    shouldSaveConfig = true;
    webServer.arg("fanDuty").toCharArray(fanDuty, 5);

    if (atoi(fanDuty) < 0)
    {
      String("0").toCharArray(fanDuty, 5);
    }
    else if (atoi(fanDuty) > 1023)
    {
      String("1023").toCharArray(fanDuty, 5);
    }
  }

  // handle both sensor ids at the same time.
  if ((webServer.arg("triggerSensor") != "" && webServer.arg("triggerSensor")) && (webServer.arg("ambiantSensor") != "" && webServer.arg("ambiantSensor")))
  {
    shouldSaveConfig = true;

    if (webServer.arg("triggerSensor") == webServer.arg("ambiantSensor"))
    {
      // abort if sensors are put to the same value
      return webSendError("Error: cannot save config. You cannot set both the trigger and ambiant sensors to the same id!", "/settings");
    }

    sensorStrToDeviceAddress(triggerTempSensorAddr, webServer.arg("triggerSensor"));
    sensorStrToDeviceAddress(ambiantTempSensorAddr, webServer.arg("ambiantSensor"));

    // check if the sensors are configured or not. if not, print a warning on the webui
    if (!temp_sensors.isConnected(triggerTempSensorAddr) || !temp_sensors.isConnected(ambiantTempSensorAddr))
    {
      sensorNotFound = true;
    }
    else
    {
      sensorNotFound = false;
    }
  }
  else
  {
    return webSendError("Error: cannot save config. Both sensors must be set to a device address!", "/settings");
  }

  if (shouldSaveConfig)
  {
    saveConfig();
  }

  String httpMessage = generateStartHtml("Settings", false, "/");
  httpMessage += "Config saved <br><br><button onclick=\"window.location.href='/';\">Continue</button>";
  httpMessage += "</body></html>";
  webServer.send(200, "text/html", httpMessage);
}

/********************************************************************/
void webSendError(String errorHTML, String redirectTo)
{

  String httpMessage = generateStartHtml("Error");
  httpMessage += errorHTML;
  httpMessage += "<br><br><button onclick=\"window.location.href='" + redirectTo + "';\">Back</button>";
  httpMessage += "</body></html>";

  webServer.send(400, "text/html", httpMessage);
}

/********************************************************************/
void webHandleStatus()
{

  DynamicJsonDocument doc(1024);
  doc["fan_status_human"] = fanStatusHuman;
  doc["fan_status"] = fanStatus;
  doc["temp_sensor_ambiant"] = currentAmbiantTemp;
  doc["temp_sensor_trigger"] = currentTriggerTemp;
  doc["wifi_rssi"] = rssi;

  String httpMessage;
  serializeJson(doc, httpMessage);

  webServer.send(200, "application/json", httpMessage);
}

/********************************************************************/
void espReset()
{
  ESP.reset();
  delay(5000);
}

/********************************************************************/
void saveConfig()
{
  Serial.println("Saving config");
  DynamicJsonDocument jsonConfigValues(1024);
  jsonConfigValues["deviceName"] = deviceName;
  jsonConfigValues["fanOffTempDiff"] = fanOffTempDiff;
  jsonConfigValues["fanChangeDelay"] = fanChangeDelay;
  jsonConfigValues["fanDuty"] = fanDuty;
  jsonConfigValues["triggerSensor"] = sensorAddrToStr(triggerTempSensorAddr);
  jsonConfigValues["ambiantSensor"] = sensorAddrToStr(ambiantTempSensorAddr);

  File configFile = LittleFS.open("/config.json", "w");
  if (!configFile)
  {
    Serial.println("Failed to open config file for writing");
  }
  else
  {
    serializeJson(jsonConfigValues, configFile);
    configFile.close();
  }

  String configJsonStr;
  serializeJson(jsonConfigValues, configJsonStr);
  Serial.println(String("LittleFS: parsed json:") + configJsonStr);
}

/********************************************************************/
void loadConfig()
{
  Serial.println("LittleFS: mounting LittleFS");
  if (LittleFS.begin())
  {
    if (LittleFS.exists("/config.json"))
    { // File exists, reading and loading
      File configFile = LittleFS.open("/config.json", "r");
      if (configFile)
      {
        size_t configFileSize = configFile.size(); // Allocate a buffer to store contents of the file.
        std::unique_ptr<char[]> buf(new char[configFileSize]);
        configFile.readBytes(buf.get(), configFileSize);

        DynamicJsonDocument configJson(1024);
        DeserializationError jsonError = deserializeJson(configJson, buf.get());
        if (jsonError)
        { // Couldn't parse the saved config
          Serial.println(String("JSON: [ERROR] Failed to parse /config.json: ") + String(jsonError.c_str()));
        }
        else
        {
          if (!configJson["deviceName"].isNull())
          {
            strcpy(deviceName, configJson["deviceName"]);
          }
          if (!configJson["fanOffTempDiff"].isNull())
          {
            strcpy(fanOffTempDiff, configJson["fanOffTempDiff"]);
          }
          if (!configJson["fanChangeDelay"].isNull())
          {
            strcpy(fanChangeDelay, configJson["fanChangeDelay"]);
          }
          if (!configJson["fanDuty"].isNull())
          {
            strcpy(fanDuty, configJson["fanDuty"]);
          }

          if (!configJson["triggerSensor"].isNull())
          {
            sensorStrToDeviceAddress(triggerTempSensorAddr, configJson["triggerSensor"]);
          }
          if (!configJson["ambiantSensor"].isNull())
          {
            sensorStrToDeviceAddress(ambiantTempSensorAddr, configJson["ambiantSensor"]);
          }

          String configJsonStr;
          serializeJson(configJson, configJsonStr);
          Serial.println(String("LittleFS: parsed json:") + configJsonStr);
        }
      }
      else
      {
        Serial.println("[ERROR] Failed to read /config.json");
      }
    }
    else
    {
      Serial.println("[WARNING] /config.json not found, will be created on first config save");
    }
  }
  else
  {
    Serial.println("LittleFS: [ERROR] Failed to mount FS");
  }
  // check if the sensors are configured or not. if not, print a warning on the webui
  if (!temp_sensors.isConnected(triggerTempSensorAddr) || !temp_sensors.isConnected(ambiantTempSensorAddr))
  {
    sensorNotFound = true;
  }
}

/********************************************************************/
