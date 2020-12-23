#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <ESP8266HTTPClient.h>
#include <WiFiUdp.h>
#include <WiFiClient.h>
#include <ArduinoOTA.h>
#include <NTPClient.h>
#include <ArduinoJson.h>
#include <TaskScheduler.h>
#include <PubSubClient.h>

#define ENABLE_GxEPD2_GFX 0
#include <GxEPD2_BW.h> // including both doesn't hurt
#include <Fonts/FreeMonoBold9pt7b.h>

// copy the constructor for your e-paper from GxEPD2_Example.ino (and for AVR boards needed #defines for MAX_HEIGHT).
#define MAX_DISPLAY_BUFFER_SIZE 800 //
#define MAX_HEIGHT(EPD) (EPD::HEIGHT <= MAX_DISPLAY_BUFFER_SIZE / (EPD::WIDTH / 8) ? EPD::HEIGHT : MAX_DISPLAY_BUFFER_SIZE / (EPD::WIDTH / 8))

GxEPD2_BW<GxEPD2_290, GxEPD2_290::HEIGHT> mDisplay(GxEPD2_290(/*CS=15*/ SS, /*DC=4*/ 4, /*RST=5*/ 5, /*BUSY=16*/ 16));



const char *ssid = STASSID;
const char *password = STAPSK;
const char *myLocationSun = "http://api.sunrise-sunset.org/json?lat=59.0&lng=25.42";
const char *mqttBroker = "192.168.1.123";
const char *hostName = "FrontDooraAuto";

WiFiUDP udpClient;
WiFiClient wifiClient;
NTPClient timeClient(udpClient, "europe.pool.ntp.org", 7200, 36000);
HTTPClient httpClient;
PubSubClient mqttClient(wifiClient);
StaticJsonDocument<1024> sunSetJson;
StaticJsonDocument<1024> mqttStatusJson;
char mqttMessageBuffer[10240];
Scheduler schedulerRunner;

String mqttInTopic;
String mqttStatusTopic;
String mqttEventTopic;

int sunRaiseHour = 6;
int sunSetHour = 18; // Turn on light
int timeOffHour = 0; // Turn off light
int currentHour = 10;
int relayStatus = 0;

String sunsetString;
String turnOnLightMessage;
String turnOffLightMessage;

void 	setupOTA();
void 	setupTask();

void 	displayStatus(const String &message, const String &message2 = "", const String &message3 = "");
void 	checkSunsetToday();
int 	getHourFromString(const String &hourMessage);

void 	dailyTaskCb();
void 	reportTaskCb();
void 	relayTaskCb();

int 	mqttReconnect();
void 	mqttPublishStatus();
void 	setupMqttConnection();
void 	mqttCallback(char *topic, byte *payload, unsigned int length);

Task dailyTask(86400000, TASK_FOREVER, &dailyTaskCb);
Task reportTask(60000, TASK_FOREVER, &reportTaskCb);
Task relayTask(60000, TASK_FOREVER, &relayTaskCb);

const int relayPIN = BUILTIN_LED;
void turnOnRelay();
void turnOffRelay();

void setup()
{
	Serial.begin(115200);
	mDisplay.init();
	displayStatus("Booting device");
	WiFi.mode(WIFI_STA);
	WiFi.begin(ssid, password);
	while (WiFi.waitForConnectResult() != WL_CONNECTED)
	{
		displayStatus("WiFi Connection Failed! Rebooting...");
		delay(5000);
		ESP.restart();
	}

	// Port defaults to 8266
	// ArduinoOTA.setPort(8266);
	setupOTA();

	timeClient.begin();
	timeClient.setTimeOffset(7200);

	displayStatus("Connection established!");
	
	setupMqttConnection();
	setupTask();
}

void loop()
{
	ArduinoOTA.handle();
	if (!timeClient.update())
	{
		timeClient.forceUpdate();
	}
	schedulerRunner.execute();
	mqttClient.loop();
	delay(10);
}

void setupOTA()
{
	ArduinoOTA.setHostname(hostName);

	// No authentication by default
	// ArduinoOTA.setPassword(OTA_PASSWORD);

	// Password can be set with it's md5 value as well
	// MD5(admin) = 21232f297a57a5a743894a0e4a801fc3
	ArduinoOTA.setPasswordHash(OTA_PASSWORD);

	ArduinoOTA.onStart([]() {
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
	ArduinoOTA.onEnd([]() {
		Serial.println("\nEnd");
	});
	ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
		Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
	});
	ArduinoOTA.onError([](ota_error_t error) {
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
}

void setupTask()
{
	schedulerRunner.addTask(dailyTask);
	dailyTask.enable();

	schedulerRunner.addTask(reportTask);
	reportTask.enable();

	schedulerRunner.addTask(relayTask);
	relayTask.enable();
}

void setupMqttConnection()
{
	mqttClient.setServer(mqttBroker, 1883);
	mqttClient.setCallback(mqttCallback);
}

void mqttCallback(char *topic, byte *payload, unsigned int length)
{
	Serial.print("Message arrived [");
	Serial.print(topic);
	Serial.print("] ");
	for (int i = 0; i < length; i++)
	{
		Serial.print((char)payload[i]);
	}
	Serial.println();

	// Switch on the LED if an 1 was received as first character
	if ((char)payload[0] == '1')
	{
		turnOnRelay(); // Turn the LED on (Note that LOW is the voltage level
		// but actually the LED is on; this is because
		// it is active low on the ESP-01)
	}
	else
	{
		turnOffRelay(); // Turn the LED off by making the voltage HIGH
	}
}

void dailyTaskCb()
{
	checkSunsetToday();
}

void relayTaskCb()
{
	auto ctime = timeClient.getFormattedTime();
	auto chour = getHourFromString(ctime);

	if (chour < 0) turnOffRelay();
	else 
	{
		if (chour - 12 > sunSetHour)
		{
			turnOnRelay();
		} else 
		{
			turnOffRelay();
		}
	}
}

void turnOnRelay()
{
	digitalWrite(BUILTIN_LED, LOW);
	relayStatus = 1;
}

void turnOffRelay()
{
	digitalWrite(BUILTIN_LED, HIGH);
	relayStatus = 0;
}

void reportTaskCb()
{
	if (!mqttClient.connected())
	{
		if (!mqttReconnect())
			return;
	}

	mqttPublishStatus();
}

int mqttReconnect()
{
	Serial.printf("[MQTT] Connecting to %s ", mqttBroker);

	String clientId = hostName;
	clientId += String(ESP.getChipId());

	mqttInTopic = "/home/device/" + String(ESP.getChipId());
	mqttInTopic += "/command";
	mqttStatusTopic = "/home/device/" + String(ESP.getChipId());
	mqttStatusTopic += "/status";

	// Attempt to connect
	if (mqttClient.connect(clientId.c_str()))
	{
		Serial.println("OK");
		mqttClient.subscribe(mqttInTopic.c_str());
	}
	else
	{
		Serial.print("FAILED, rc=");
		Serial.print(mqttClient.state());
		Serial.println(".");
		return false;
	}

	return true;
}

void mqttPublishStatus()
{
	turnOffLightMessage = "Clock: " + timeClient.getFormattedTime();
	displayStatus(sunsetString, turnOnLightMessage, turnOffLightMessage);

	mqttStatusJson["time"] = timeClient.getFormattedTime();
	mqttStatusJson["relays"][0] = relayStatus ? "ON" : "OFF";
	mqttStatusJson["message"] = "Sunset: " + sunsetString;
	serializeJson(mqttStatusJson, mqttMessageBuffer, 1024);

	Serial.printf("[MQTT] Publishing on '%s' %s .\n", mqttStatusTopic.c_str(), mqttMessageBuffer);
	mqttClient.publish(mqttStatusTopic.c_str(), mqttMessageBuffer);
}

void checkSunsetToday()
{
	// Invoking API to get Sunrise and Sun set
	if (httpClient.begin(wifiClient, myLocationSun))
	{
		Serial.printf("[HTTP] GET: %s\n", myLocationSun);
		// start connection and send HTTP header
		int httpCode = httpClient.GET();

		// httpCode will be negative on error
		if (httpCode > 0)
		{
			Serial.printf("[HTTP] GET... code: %d\n", httpCode);

			if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY)
			{
				String payload = httpClient.getString();
				deserializeJson(sunSetJson, payload);
				sunsetString = String(sunSetJson["results"]["astronomical_twilight_end"].as<const char *>());
				sunSetHour = getHourFromString(sunsetString);
				sunsetString = "Sunset: " + sunsetString;
				turnOnLightMessage = "ON: " + String(sunSetHour) + ":00 PM";
				turnOffLightMessage = "Clock: " + timeClient.getFormattedTime();

				displayStatus(sunsetString, turnOnLightMessage, turnOffLightMessage);
				Serial.println(payload);
			}
		}
		else
		{
			Serial.printf("[HTTP] GET... failed, error: %s\n", httpClient.errorToString(httpCode).c_str());
		}

		httpClient.end();
	}
	else
	{
		Serial.printf("[HTTP] Failed to load sunset\n");
	}
}

void displayStatus(const String &message, const String &message2, const String &message3)
{
	Serial.println(message);

	auto printIp = [](IPAddress address) {
		static char szRet[20];
		String str = String(address[0]);
		str += ".";
		str += String(address[1]);
		str += ".";
		str += String(address[2]);
		str += ".";
		str += String(address[3]);
		str.toCharArray(szRet, 20);
		return szRet;
	};
	String ipmessage = "Address: ";
	ipmessage += printIp(WiFi.localIP());

	mDisplay.setRotation(1);
	mDisplay.setFont(&FreeMonoBold9pt7b);
	mDisplay.setTextColor(GxEPD_BLACK);
	int16_t tbx, tby;
	uint16_t tbw, tbh;
	mDisplay.getTextBounds("HOME AUTOMATION", 0, 0, &tbx, &tby, &tbw, &tbh);
	// center the bounding box by transposition of the origin:
	uint16_t x = 10;
	uint16_t y = ((mDisplay.height() - tbh) / 2) - tby;
	mDisplay.setFullWindow();
	mDisplay.firstPage();
	do
	{
		mDisplay.fillScreen(GxEPD_WHITE);

		mDisplay.setCursor(70, 20);
		mDisplay.print("HOME AUTOMATION");
		mDisplay.setCursor(x, 50);
		mDisplay.print(ipmessage);
		mDisplay.setCursor(x, 70);
		mDisplay.print(message);
		if (message2)
		{
			mDisplay.setCursor(x, 90);
			mDisplay.print(message2);
		}
		if (message3)
		{
			mDisplay.setCursor(x, 110);
			mDisplay.print(message3);
		}
	} while (mDisplay.nextPage());

	mDisplay.hibernate();
}

int getHourFromString(const String &hourMessage)
{
	int ind1 = hourMessage.indexOf(':'); //finds location of first :
	if (ind1 < 0)
		return sunSetHour;

	return hourMessage.substring(0, ind1).toInt();
}
