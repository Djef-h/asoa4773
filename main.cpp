//eBjCmoJ48A27PR6K // Superbase ASOA4773 password 
//admin@asoa.bg =  V.4773
/*
 * ASOA 
 Date:26.11.2026
 
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_ADS1X15.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <esp_task_wdt.h> // Хардуерна библиотека за Watchdog пазача на ESP32

#define WIFI_SSID   "LAB"
#define WIFI_PASS   "Ropotamo24#"
#define MQTT_HOST   "broker.hivemq.com"
#define MQTT_PORT   1883
#define MQTT_TOPIC  "archeoiot/abc123xyz"

// ── Настройки за Сигурност и Рестарт ────────────────────
#define WATCHDOG_TIMEOUT_SEC  10                      // Авариен таймаут: 10 секунди
const unsigned long REBOOT_INTERVAL = 6 * 60 * 60 * 1000; // Профилактичен рестарт на всеки 6 часа (в ms)

// ── Хардуерни Пинове за ESP32 ──────────────────────────
#define ONE_WIRE_BUS 4     // DS18B20 температурен сензор на GPIO 4
// ADS1115 използва стандартните хардуерни I2C пинове за ESP32: SDA (GPIO 21), SCL (GPIO 22)

// ── Коефициенти и Константи ─────────────────────────────
const unsigned long SEND_INTERVAL = 2000; // Интервал за пращане към БД (ms)
const float CAL_SLOPE      = 6.3373;      // Измереният наклон (m)
const float CAL_INTERCEPT  = 0.0711;      // Измереният офсет (b)
const float TDS_COEF       = 0.6400;      // Стандарт 442 за природни води

// ── Обекти ────────────────────────────────────────────
Adafruit_ADS1115 ads;
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature tempSensor(&oneWire);
WiFiClient espClient;
PubSubClient mqtt(espClient);

// ── Глобални променливи за данните ─────────────────────
float lastTemp = 25.0f;
float lastEC   = 0.0f; 
float lastTDS  = 0.0f; 
int   lastWL   = 85;   // Заложена статична стойност за Dashboard-а

// Task Handle за Watchdog задачата на второто ядро
TaskHandle_t WatchdogTaskHandle = NULL;

// ── Прототипи на функциите (Задължително за PlatformIO) ──
void handleMQTT();
void readSensors();
void sendDataToDatabase();
void checkScheduledReset();
void watchdogTask(void * pvParameters);

// ── Настройка (Setup) ──────────────────────────────────
void setup() {
  Serial.begin(115200);

  // Стартиране на I2C шината
  Wire.begin();

  // Инициализация на АЦП ADS1115
  if (!ads.begin()) {
    Serial.println("Критична грешка: ADS1115 не е намерен на I2C шината!");
    while (1);
  }
  ads.setGain(GAIN_FOUR); // Обхват до +/- 1.024V (31.25 uV на единица)

  // Старт на температурния сензор
  tempSensor.begin();

  // Свързване към Wi-Fi мрежата
  Serial.print("\nСвързване към Wi-Fi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 15000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWi-Fi Свързан! IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nWi-Fi Грешка - уредът преминава в автономен режим");
  }

  // Настройка на MQTT параметрите
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setKeepAlive(60);

  // ── СТАРТИРАНЕ НА ХАРДУЕРНИЯ WATCHDOG НА ЯДРО 0 (CORE 0) ──
  xTaskCreatePinnedToCore(
    watchdogTask,            // Функция на задачата
    "WatchdogTask",          // Текстово име
    4000,                    // Размер на стека
    NULL,                    // Входящи параметри
    1,                       // Приоритет
    &WatchdogTaskHandle,     // Указател към задачата
    0                        // ИНСТРУКЦИЯ: Изпълнявай само на Ядро 0
  );

  Serial.println("Watchdog системата е активирана успешно на Core 0!");
}

// ── Главен Цикъл (Върти се на Ядро 1 по подразбиране) ──
void loop() {
  // 1. Проверка за профилактичен 6-часов рестарт на паметта
  checkScheduledReset();

  // 2. Поддържане на MQTT мрежовия трафик жив
  handleMQTT();

  // 3. Измерване и изпращане в зададения времеви прозорец
  static unsigned long lastSend = 0;
  if (millis() - lastSend >= SEND_INTERVAL) {
    lastSend = millis();

    // Извикване на измерванията
    readSensors();

    // Локален дебъг в терминала на PlatformIO
    Serial.printf("Лог -> Т: %.1f °C | EC: %.3f mS/cm | TDS: %.2f PPT\n", lastTemp, lastEC, lastTDS);

    // Изпращане на данните в JSON формат
    sendDataToDatabase();
  }
}

// ── ЗАДАЧА ЗА ЯДРО 0: СЛЕДЕНЕ ЗА ЗАБИВАНЕ (WATCHDOG) ────
void watchdogTask(void * pvParameters) {
  // Активиране на Task Watchdog Timer за ядрото
  esp_task_wdt_init(WATCHDOG_TIMEOUT_SEC, true); // true = хардуерен рестарт при забиване
  esp_task_wdt_add(NULL);                         // Закачане на текущата нишка

  for(;;) {
    // Редовно "храним" кучето на всеки 1 секунда
    esp_task_wdt_reset();
    
    // Предаваме контрола обратно на FreeRTOS за 1 секунда (неблокираща пауза)
    vTaskDelay(1000 / portTICK_PERIOD_MS); 
  }
}

// ── ФУНКЦИЯ ЗА АВТОМАТИЧНО ИЗЧИСТВАНЕ НА RAM ПАМЕТТА ─────
void checkScheduledReset() {
  // Проверка дали времето на непрекъсната работа е надхвърлило 6 часа
  if (millis() >= REBOOT_INTERVAL) {
    Serial.println("🚨 Изпълнява се планиран 6-часов рестарт за превантивно изчистване на паметта!");
    delay(500);    // Изчакване за изпращане на серийния лог
    ESP.restart(); // Софтуерно-хардуерен рестарт на целия микроконтролер
  }
}

// ── МРЕЖОВИ И ИЗМЕРВАТЕЛНИ ФУНКЦИИ ───────────────────────

void handleMQTT() {
  if (mqtt.connected()) {
    mqtt.loop();
    return;
  }
  
  static unsigned long lastMqttAttempt = 0;
  if (millis() - lastMqttAttempt < 5000) return;
  lastMqttAttempt = millis();

  Serial.print("Опит за MQTT връзка...");
  
  // Създаване на уникално MQTT ID на база хардуерния MAC адрес
  uint64_t chipid = ESP.getEfuseMac();
  String clientId = "ESP32_Archeo_" + String((uint32_t)(chipid >> 32), HEX);
  
  if (mqtt.connect(clientId.c_str())) {
    Serial.println(" Успешно свързване!");
  } else {
    Serial.printf(" Грешка: %d\n", mqtt.state());
  }
}

void readSensors() {
  // 1. Четене на температурата от цифровия сензор DS18B20
  tempSensor.requestTemperatures();
  float currentTemp = tempSensor.getTempCByIndex(0);
  if (currentTemp != DEVICE_DISCONNECTED_C && currentTemp > -40.0f) {
    lastTemp = currentTemp;
  }

  // 2. Четене на суровото напрежение от високопрецизния ADS1115 (Канал A0)
  int16_t adcResults = ads.readADC_SingleEnded(0);
  float voltageRaw = adcResults * 0.00003125; 

  // 3. Температурна компенсация, приведена към 25°C
  float temperatureCoefficient = 1.0f + 0.0185f * (lastTemp - 25.0f);
  float compensatedVoltage = voltageRaw / temperatureCoefficient;

  // 4. Математическо изчисляване на EC на базата на двете тествани точки
  if (voltageRaw > 0.005f) { // Софтуерен филтър за работа във въздушна среда
    lastEC = (CAL_SLOPE * compensatedVoltage) + CAL_INTERCEPT;
    if (lastEC < 0.0f) lastEC = 0.0f;
  } else {
    lastEC = 0.0f; 
  }

  // 5. Точна формула за превръщане в PPT за нуждите на ASOA терминала
  // Вече изпращаме директно реалната стойност (напр. 1.36 mS/cm * 0.64 = 0.87 PPT)
  lastTDS = lastEC * TDS_COEF;
}

void sendDataToDatabase() {
  if (!mqtt.connected()) return;

  // Инициализиране на JSON пакета
  StaticJsonDocument<128> doc;
  doc["temp"] = round(lastTemp * 10)  / 10.0;
  doc["ec"]   = round(lastEC   * 100) / 100.0; 
  doc["tds"]  = round(lastTDS  * 100) / 100.0; // Изпраща се като чисто PPT (напр. 0.87)
  doc["wl"]   = lastWL;                       

  String json;
  serializeJson(doc, json);
  mqtt.publish(MQTT_TOPIC, json.c_str());
}
*/
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_ADS1X15.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <DHT.h>
#include <LiquidCrystal_I2C.h>
#include <esp_task_wdt.h>

// ── Версия и ID на устройството ────────────────────────
#define FW_VERSION  "3.1.0"
#define DEVICE_ID   "esp32_main"

// ── Wi-Fi Настройки ────────────────────────────────────
#define WIFI_SSID   "DESKTOP-C1PKIL4 2200"
#define WIFI_PASS   "H3[h6610"

// ── Supabase Настройки ─────────────────────────────────
const char* SUPABASE_URL = "https://loyevdyvjmndqgtjxjmh.supabase.co";
const char* SUPABASE_KEY = "sb_publishable_vgSZZL_mrtKFXxfg-Qhd6Q_V97bjPgL"; // <--- СМЕНИ ТОВА С ТВОЯ sb_publishable_... КЛЮЧ!

// ── Интервали и Сигурност ──────────────────────────────
#define WATCHDOG_TIMEOUT_SEC  25
const unsigned long SENSOR_INTERVAL = 2000;   // Четене и LCD: на всеки 2 сек
const unsigned long LOG_INTERVAL    = 3000;   // Пращане на терминал логове: на 3 сек
const unsigned long SEND_INTERVAL   = 60000;  // Пращане на данни и проверка за OTA: на 1 минута
const unsigned long REBOOT_INTERVAL = 6UL * 60UL * 60UL * 1000UL; // Профилактичен рестарт на 6 часа

// ── Хардуерни Пинове ───────────────────────────────────
#define ONE_WIRE_BUS 25    // DS18B20 температурен сензор (вода) на GPIO 25
#define DHTPIN       14    // DHT11 сензор (въздух) на GPIO 14
#define DHTTYPE      DHT11
// I2C пинове за ADS1115 и LCD 16x2: SDA = GPIO 21, SCL = GPIO 22

// ── Калибрация на EC / TDS ─────────────────────────────
const float CAL_SLOPE     = 6.3373;
const float CAL_INTERCEPT = 0.0711;
const float TDS_COEF      = 0.6400;

// ── Обекти ─────────────────────────────────────────────
Adafruit_ADS1115 ads;
bool adsAvailable = false;

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature tempSensor(&oneWire);
DHT dht(DHTPIN, DHTTYPE);
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ── Глобални променливи за данните ─────────────────────
float lastWaterTemp = 25.0f;
float lastAirTemp   = 0.0f;
float lastAirHum    = 0.0f;
float lastEC        = 0.0f;
float lastTDS       = 0.0f;
int   lastWL        = 85;

// Буфер за отдалечения уеб терминал
String logBuffer = "";

// Функция, която пише едновременно в USB Serial и в уеб терминала
void webLog(String msg) {
  Serial.println(msg);
  if (logBuffer.length() < 1500) {
    logBuffer += msg + "\n";
  }
}

// ── Прототипи ──────────────────────────────────────────
void setupWiFi();
void checkWiFi();
void readSensors();
void updateLCD();
void flushLogsToSupabase();
void sendDataToSupabase();
void checkRemoteOTA();
void performOTAUpdate(String binUrl);
void checkScheduledReset();

void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);

  // Старт на LCD дисплея
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("ASOA IoT v" FW_VERSION);
  lcd.setCursor(0, 1);
  lcd.print("Startirane...");

  // Инициализация на ADS1115
  if (ads.begin()) {
    ads.setGain(GAIN_FOUR);
    adsAvailable = true;
    webLog("[INIT] ADS1115 OK");
  } else {
    webLog("[ERROR] ADS1115 ne e nameren na I2C!");
    lcd.setCursor(0, 1);
    lcd.print("Greshka ADS1115!");
    delay(2000);
  }

  // Старт на DS18B20 (асинхронно) и DHT11
  tempSensor.begin();
  tempSensor.setWaitForConversion(false);
  tempSensor.requestTemperatures();
  dht.begin();

  // Свързване към Wi-Fi
  setupWiFi();
  webLog("[SYS] Ustroystvoto startira. Ver: " FW_VERSION);

  // Активиране на Watchdog за главния loop()
  esp_task_wdt_init(WATCHDOG_TIMEOUT_SEC, true);
  esp_task_wdt_add(NULL);
}

void loop() {
  esp_task_wdt_reset();
  checkScheduledReset();
  checkWiFi();

  // 1. Четене на сензори и смяна на екрана на LCD (на 2 сек)
  static unsigned long lastSensorRead = 0;
  if (millis() - lastSensorRead >= SENSOR_INTERVAL) {
    lastSensorRead = millis();
    readSensors();
    updateLCD();

    char buf[96];
    snprintf(buf, sizeof(buf), "[SENS] W:%.1fC | Air:%.1fC %.0f%% | EC:%.2f | TDS:%.2f",
             lastWaterTemp, lastAirTemp, lastAirHum, lastEC, lastTDS);
    webLog(String(buf));
  }

  // 2. Изпращане на логовете към уеб терминала (на 3 сек)
  static unsigned long lastLogFlush = 0;
  if (millis() - lastLogFlush >= LOG_INTERVAL) {
    lastLogFlush = millis();
    flushLogsToSupabase();
  }

  // 3. Запис на данните в базата и проверка за OTA ъпдейт (на 1 минута)
  static unsigned long lastCloudSync = 0;
  if (millis() - lastCloudSync >= SEND_INTERVAL) {
    lastCloudSync = millis();
    sendDataToSupabase();
    checkRemoteOTA();
  }
}

void setupWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    webLog("[WIFI] Svarzan! IP: " + WiFi.localIP().toString());
  } else {
    webLog("[WIFI] Nyama vrazka - avtonomen rejim.");
  }
}

void checkWiFi() {
  if (WiFi.status() != WL_CONNECTED) {
    static unsigned long lastRetry = 0;
    if (millis() - lastRetry > 15000) {
      lastRetry = millis();
      WiFi.disconnect();
      WiFi.reconnect();
    }
  }
}

void flushLogsToSupabase() {
  if (WiFi.status() != WL_CONNECTED || logBuffer.length() == 0) return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  http.begin(client, String(SUPABASE_URL) + "/rest/v1/device_logs");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Prefer", "return=minimal");

  StaticJsonDocument<1024> doc;
  logBuffer.trim();
  doc["message"] = logBuffer;
  logBuffer = "";

  String payload;
  serializeJson(doc, payload);
  http.POST(payload);
  http.end();
}

void sendDataToSupabase() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  http.begin(client, String(SUPABASE_URL) + "/rest/v1/sensor_data");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Prefer", "return=minimal");

  StaticJsonDocument<256> doc;
  doc["temp"]     = round(lastWaterTemp * 10) / 10.0;
  doc["air_temp"] = round(lastAirTemp   * 10) / 10.0;
  doc["air_hum"]  = round(lastAirHum    * 10) / 10.0;
  doc["ec"]       = round(lastEC        * 100) / 100.0;
  doc["tds"]      = round(lastTDS       * 100) / 100.0;
  doc["wl"]       = lastWL;
  doc["rssi"]     = WiFi.RSSI();
  doc["ver"]      = FW_VERSION;

  String payload;
  serializeJson(doc, payload);
  int code = http.POST(payload);
  if (code == 201 || code == 200) {
    webLog("[DB] Dannite sa zapisani v Supabase (1 min).");
  } else {
    webLog("[DB ERROR] Kod: " + String(code));
  }
  http.end();
}

void checkRemoteOTA() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  String url = String(SUPABASE_URL) + "/rest/v1/device_control?device_id=eq." + DEVICE_ID + "&select=trigger_ota,firmware_url";
  http.begin(client, url);
  http.addHeader("apikey", SUPABASE_KEY);

  if (http.GET() == 200) {
    StaticJsonDocument<512> doc;
    if (!deserializeJson(doc, http.getString()) && doc.size() > 0) {
      bool triggerOta = doc[0]["trigger_ota"];
      String binUrl = doc[0]["firmware_url"].as<String>();

      if (triggerOta && binUrl.length() > 10) {
        http.end();

        // Нулираме флага trigger_ota на false преди да започнем ъпдейта
        HTTPClient patchHttp;
        patchHttp.begin(client, String(SUPABASE_URL) + "/rest/v1/device_control?device_id=eq." + DEVICE_ID);
        patchHttp.addHeader("Content-Type", "application/json");
        patchHttp.addHeader("apikey", SUPABASE_KEY);
        patchHttp.PATCH("{\"trigger_ota\": false}");
        patchHttp.end();

        performOTAUpdate(binUrl);
        return;
      }
    }
  }
  http.end();
}

void performOTAUpdate(String binUrl) {
  webLog("[OTA] Zapochna izteglyane na nov firmware...");
  flushLogsToSupabase();

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("OTA Update...");
  lcd.setCursor(0, 1);
  lcd.print("Izteglyane...");

  esp_task_wdt_delete(NULL);

  WiFiClientSecure client;
  client.setInsecure();
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  httpUpdate.rebootOnUpdate(true);

  t_httpUpdate_return ret = httpUpdate.update(client, binUrl);
  if (ret == HTTP_UPDATE_FAILED) {
    webLog("[OTA ERROR] " + httpUpdate.getLastErrorString());
    lcd.setCursor(0, 1);
    lcd.print("OTA Greshka!    ");
    delay(3000);
    esp_task_wdt_add(NULL);
  }
}

void readSensors() {
  // DS18B20 (вода)
  float tW = tempSensor.getTempCByIndex(0);
  if (tW != DEVICE_DISCONNECTED_C && tW > -40.0f && tW < 85.0f) {
    lastWaterTemp = tW;
  }
  tempSensor.requestTemperatures();

  // DHT11 (въздух)
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  if (!isnan(h) && !isnan(t)) {
    lastAirHum = h;
    lastAirTemp = t;
  }

  // ADS1115 (EC / TDS с усредняване)
  if (!adsAvailable) {
    if (ads.begin()) {
      ads.setGain(GAIN_FOUR);
      adsAvailable = true;
    } else {
      return;
    }
  }

  long adcSum = 0;
  for (int i = 0; i < 10; i++) {
    adcSum += ads.readADC_SingleEnded(0);
    delay(5);
  }
  float voltageRaw = (adcSum / 10.0f) * 0.00003125f;
  float compV = voltageRaw / (1.0f + 0.0185f * (lastWaterTemp - 25.0f));

  lastEC = (voltageRaw > 0.005f) ? max(0.0f, (CAL_SLOPE * compV) + CAL_INTERCEPT) : 0.0f;
  lastTDS = lastEC * TDS_COEF;
}

void updateLCD() {
  static bool scr = true;
  lcd.clear();
  if (scr) {
    lcd.setCursor(0, 0);
    lcd.printf("W:%.1fC EC:%.2f", lastWaterTemp, lastEC);
    lcd.setCursor(0, 1);
    lcd.printf("TDS:%.2f PPT", lastTDS);
  } else {
    lcd.setCursor(0, 0);
    lcd.printf("Air:%.1fC H:%d%%", lastAirTemp, (int)lastAirHum);
    lcd.setCursor(0, 1);
    lcd.print(WiFi.status() == WL_CONNECTED ? "Cloud OK" : "Offline");
  }
  scr = !scr;
}

void checkScheduledReset() {
  if (millis() >= REBOOT_INTERVAL) {
    webLog("[SYS] Planiran 6-chasov restart...");
    flushLogsToSupabase();
    ESP.restart();
  }
}