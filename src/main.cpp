// #define DEBUG_SSL
// #define DEBUGV

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>

#include "secure.h"

//periodic status reports
const unsigned int stats_interval = 60; // Update statistics and measure every 60 seconds

#define ALTITUDE 123 //Altitude of your location (m above sea level)

Adafruit_BME280 bme; // I2C
ESP8266WebServer server(80);

// Temperature functions
const float cToKOffset = 273.15;
float absoluteHumidity(float temperature, float humidity);
float saturationVaporPressure(float temperature);
float dewPoint(float temperature, float humidity);

#define USE_MQTT
#ifdef USE_MQTT
#endif

os_timer_t Timer1;
bool sendStats = true;

// WiFiFlientSecure for SSL/TLS support
// WiFiClientSecure client;
WiFiClientSecure espClient;

// Callback function for the timer
void timerCallback(void *arg)
{
  sendStats = true;
}

// Convert an IP address to a String
void ipToString(const IPAddress &ip, char *str)
{
  //@author Marvin Roger - https://github.com/marvinroger/homie-esp8266/blob/ad876b2cd0aaddc7bc30f1c76bfc22cd815730d9/src/Homie/Utils/Helpers.cpp#L82
  snprintf(str, 16, "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
}

// Sending and printing the latest values
void sendStatsInterval(void)
{
  char buf[16]; //v4 only atm
  ipToString(WiFi.localIP(), buf);

  float temperature = bme.readTemperature();
  float humidity_r = bme.readHumidity();
  float humidity = absoluteHumidity(temperature, humidity_r);
  float pressure = bme.readPressure() / 100.0F;
  float pressure_r = bme.seaLevelForAltitude(ALTITUDE, pressure);
  float dew = dewPoint(temperature, humidity_r);

  Serial.print(F("T: "));
  Serial.print((String)temperature);
  Serial.print(F(" *C\nDP: "));
  Serial.print((String)dew);
  Serial.print(F(" *C\nH: "));
  Serial.print((String)humidity_r);
  Serial.print(F(" %\nAH: "));
  Serial.print((String)humidity);
  Serial.print(F(" g/m3\nRP: "));
  Serial.print((String)pressure_r);
  Serial.print(F(" hPa\nP: "));
  Serial.print((String)pressure);
  Serial.println(F(" hPa"));
  Serial.flush();

  yield();

}

// Convert the WIFI rssi to a percentage
uint8_t rssiToPercentage(int32_t rssi)
{
  //@author Marvin Roger - https://github.com/marvinroger/homie-esp8266/blob/ad876b2cd0aaddc7bc30f1c76bfc22cd815730d9/src/Homie/Utils/Helpers.cpp#L12
  uint8_t quality;
  if (rssi <= -100)
  {
    quality = 0;
  }
  else if (rssi >= -50)
  {
    quality = 100;
  }
  else
  {
    quality = 2 * (rssi + 100);
  }

  return quality;
}

// Setup of the Wifi
void setup_wifi()
{
  delay(10);
  // We start by connecting to a WiFi network
  Serial.println();
  Serial.print(F("Connecting to "));
  Serial.println(_WLAN_SSID);

  WiFi.mode(WIFI_STA); // Disable the built-in WiFi access point.
  WiFi.begin(_WLAN_SSID, _WLAN_PASS);

  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print('.');
  }

  Serial.println();
  Serial.println(F("WiFi connected"));
  Serial.println(F("IP address: "));
  Serial.println(WiFi.localIP());

  long rssi = WiFi.RSSI();
  Serial.print("RSSI: ");
  Serial.print(rssi);
  Serial.print(" %: ");
  Serial.println(rssiToPercentage(rssi));

}

// Webserver is showing the latest information
void handleRoot()
{
  float temperature = bme.readTemperature();
  float humidity_r = bme.readHumidity();
  float pressure = bme.readPressure() / 100.0F;

  String out = "Temperature: ";
  out += temperature;
  out += "*C\nDew point: ";
  out += dewPoint(temperature, humidity_r);
  out += "*C\nRelative Humidity: ";
  out += humidity_r;
  out += "%\nAbsolute Humidity: ";
  out += absoluteHumidity(temperature, humidity_r);
  out += "g/m3\nRelative Pressure: ";
  out += bme.seaLevelForAltitude(ALTITUDE, pressure);
  out += "hPa\nAbsolute Pressure: ";
  out += pressure;
  out += "hPa";
  server.send(200, "text/plain", out);
}

// Webserver is activating the OTA handling for 2 Minutes
void handleOTA()
{
  server.send(200, "text/plain", "OTA START (Ready for maximum 2 minutes)");
  ArduinoOTA.begin();

  unsigned long timeout = millis() + (120 * 1000); // Max 2 minutes
  os_timer_disarm(&Timer1);

  while (true)
  {
    yield();
    ArduinoOTA.handle();
    if (millis() > timeout)
      break;
  }

  os_timer_arm(&Timer1, stats_interval * 1000, true);
  server.send(200, "text/plain", "OTA session closed");
  return;
}

// Webserver is showing a message, that there is no information on the site
void handleNotFound()
{
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint8_t i = 0; i < server.args(); i++)
  {
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
  server.send(404, "text/plain", message);
}

void setup()
{
  Serial.begin(115200);
  Serial.println(F("BME280 HTTP-POST,HTTP-SRV,MQTT"));

  bool status;

  // default settings
  // (you can also pass in a Wire library object like &Wire2)
  status = bme.begin(0x76);
  if (!status)
  {
    Serial.println(F("Could not find a valid BME280 sensor, check wiring!"));
    // while (1); // TODO
  }

  if (MDNS.begin(((String)WiFi.macAddress()).c_str()))
  {
    Serial.println(F("MDNS responder started"));
  }

  server.on("/", handleRoot);
  server.on("/ota", handleOTA);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println(F("HTTP-SRV started"));

  setup_wifi();

  // Send boot info
  Serial.println(F("Announcing boot..."));

  os_timer_setfn(&Timer1, timerCallback, (void *)0);
  os_timer_arm(&Timer1, stats_interval * 1000, true);
}

void loop()
{

  server.handleClient();

  if (sendStats)
  {
    sendStatsInterval();
    sendStats = false;
  }
}

// Relative to absolute humidity
// Based on https://carnotcycle.wordpress.com/2012/08/04/how-to-convert-relative-humidity-to-absolute-humidity/
float absoluteHumidity(float temperature, float humidity)
{
  return (13.2471 * pow(EULER, 17.67 * temperature / (temperature + 243.5)) * humidity / (cToKOffset + temperature));
}

// Calculate saturation vapor pressure
// Based on dew.js, Copyright 2011 Wolfgang Kuehn, Apache License 2.0
float saturationVaporPressure(float temperature)
{
  if (temperature < 173 || temperature > 678)
    return -112; //Temperature out of range

  float svp = 0;
  if (temperature <= cToKOffset)
  {
    /**
      * -100-0°C -> Saturation vapor pressure over ice
      * ITS-90 Formulations by Bob Hardy published in 
      * "The Proceedings of the Third International 
      * Symposium on Humidity & Moisture",
      * Teddington, London, England, April 1998
      */

    svp = exp(-5.8666426e3 / temperature + 2.232870244e1 + (1.39387003e-2 + (-3.4262402e-5 + (2.7040955e-8 * temperature)) * temperature) * temperature + 6.7063522e-1 * log(temperature));
  }
  else
  {
    /**
      * 0°C-400°C -> Saturation vapor pressure over water
      * IAPWS Industrial Formulation 1997
      * for the Thermodynamic Properties of Water and Steam
      * by IAPWS (International Association for the Properties
      * of Water and Steam), Erlangen, Germany, September 1997.
      * Equation 30 in Section 8.1 "The Saturation-Pressure 
      * Equation (Basic Equation)"
      */

    const float th = temperature + -0.23855557567849 / (temperature - 0.65017534844798e3);
    const float a = (th + 0.11670521452767e4) * th + -0.72421316703206e6;
    const float b = (-0.17073846940092e2 * th + 0.12020824702470e5) * th + -0.32325550322333e7;
    const float c = (0.14915108613530e2 * th + -0.48232657361591e4) * th + 0.40511340542057e6;

    svp = 2 * c / (-b + sqrt(b * b - 4 * a * c));
    svp *= svp;
    svp *= svp;
    svp *= 1e6;
  }

  yield();

  return svp;
}

// Calculate dew point in °C
// Based on dew.js, Copyright 2011 Wolfgang Kuehn, Apache License 2.0
float dewPoint(float temperature, float humidity)
{
  temperature += cToKOffset; //Celsius to Kelvin

  if (humidity < 0 || humidity > 100)
    return -111; //Invalid humidity
  if (temperature < 173 || temperature > 678)
    return -112; //Temperature out of range

  humidity = humidity / 100 * saturationVaporPressure(temperature);

  byte mc = 10;

  float xNew;
  float dx;
  float z;

  do
  {
    dx = temperature / 1000;
    z = saturationVaporPressure(temperature);
    xNew = temperature + dx * (humidity - z) / (saturationVaporPressure(temperature + dx) - z);
    if (abs((xNew - temperature) / xNew) < 0.0001)
    {
      return xNew - cToKOffset;
    }
    temperature = xNew;
    mc--;
  } while (mc > 0);

  return -113; //Solver did not get a close result
}



