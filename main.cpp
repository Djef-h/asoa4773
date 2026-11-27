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
#include <Preferences.h>
#include <esp_task_wdt.h>
#include <esp_system.h>

// ── Версия и ID на устройството ────────────────────────
#define FW_VERSION  "3.3.0"
#define DEVICE_ID   "esp32_main"

// ── Списък с Wi-Fi Мрежи (по приоритет) ────────────────
struct WiFiNetwork {
  const char* ssid;
  const char* password;
};

const WiFiNetwork WIFI_NETWORKS[] = {
  { "DESKTOP-C1PKIL4 2200", "H3[h6610" },
  { "LAB",                  "Ropotamo24#" }
  // Добави още мрежи тук при нужда:
  // { "ImeNaMreja", "Parola" }
};
const int WIFI_NETWORK_COUNT = sizeof(WIFI_NETWORKS) / sizeof(WIFI_NETWORKS[0]);

// ── Supabase Настройки ─────────────────────────────────
const char* SUPABASE_URL = "https://loyevdyvjmndqgtjxjmh.supabase.co";
const char* SUPABASE_KEY = "sb_publishable_vgSZZL_mrtKFXxfg-Qhd6Q_V97bjPgL";

// ── Интервали и Сигурност ──────────────────────────────
#define WATCHDOG_TIMEOUT_SEC  25
const unsigned long SENSOR_INTERVAL = 2000;   // Четене и LCD: на всеки 2 сек
const unsigned long LOG_INTERVAL    = 3000;   // Пращане на терминал логове: на 3 сек
unsigned long sendInterval          = 60000;  // Динамичен интервал за БД/OTA (по подразбиране 60 сек)
const unsigned long REBOOT_INTERVAL = 6UL * 60UL * 60UL * 1000UL; // Профилактичен рестарт на 6 часа

// ── Хардуерни Пинове ───────────────────────────────────
#define ONE_WIRE_BUS 25    // DS18B20 температурен сензор (вода) на GPIO 25
#define DHTPIN       14    // DHT11 сензор (въздух) на GPIO 14
#define DHTTYPE      DHT11
// I2C пинове за ADS1115 и LCD 16x2: SDA = GPIO 21, SCL = GPIO 22

// ── Динамична Калибрация на EC / TDS (зарежда се от паметта / Supabase) ──
float calSlope     = 6.3373f;
float calIntercept = 0.0711f;
float tdsCoef      = 0.6400f;

// ── Офлайн буфер в RAM при липса на Wi-Fi (до 120 записа) ──
#define MAX_OFFLINE_RECORDS 120
struct SensorRecord {
  float temp;
  float air_temp;
  float air_hum;
  float ec;
  float tds;
  int   wl;
  int   rssi;
  unsigned long uptime_sec;
  uint32_t free_heap;
};
SensorRecord offlineBuffer[MAX_OFFLINE_RECORDS];
int offlineCount = 0;

// ── Обекти ─────────────────────────────────────────────
Adafruit_ADS1115 ads;
bool adsAvailable = false;

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature tempSensor(&oneWire);
DHT dht(DHTPIN, DHTTYPE);
LiquidCrystal_I2C lcd(0x27, 16, 2);
Preferences prefs;

// ── Глобални променливи за данните ─────────────────────
float lastWaterTemp = 25.0f;
float lastAirTemp   = 0.0f;
float lastAirHum    = 0.0f;
float lastEC        = 0.0f;
float lastTDS       = 0.0f;
int   lastWL        = 85;
String bootResetReason = "Unknown";

// Буфер за отдалечения уеб терминал
String logBuffer = "";

// Функция, която пише едновременно в USB Serial и в уеб терминала
void webLog(String msg) {
  Serial.println(msg);
  if (logBuffer.length() < 1500) {
    logBuffer += msg + "\n";
  }
}

// Помощна функция за изписване на точно 16 символа на ред на LCD (без премигване)
void printLcdLine(uint8_t row, const char* text) {
  char line[17];
  snprintf(line, sizeof(line), "%-16.16s", text);
  lcd.setCursor(0, row);
  lcd.print(line);
}

// Определяне на причината за последния рестарт
String getResetReasonText() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "Power On (Tok)";
    case ESP_RST_SW:        return "Software / OTA / Remote";
    case ESP_RST_PANIC:     return "Crash / Exception";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:       return "Watchdog Reset";
    case ESP_RST_BROWNOUT:  return "Brownout (Nisko naprezhenie)";
    default:                return "Other (" + String((int)esp_reset_reason()) + ")";
  }
}

// ── Прототипи ──────────────────────────────────────────
bool connectToKnownWiFi();
void checkWiFi();
void readSensors();
void updateLCD();
void flushLogsToSupabase();
void handleDataSync();
bool postSingleRecord(const SensorRecord& rec, int bufLeft);
void flushOfflineBuffer();
void checkRemoteControl();
void performOTAUpdate(String binUrl);
void checkScheduledReset();

void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);

  bootResetReason = getResetReasonText();

  // Зареждане на запазените калибрационни коефициенти от енергонезависимата памет
  prefs.begin("asoa_cal", false);
  calSlope     = prefs.getFloat("slope", 6.3373f);
  calIntercept = prefs.getFloat("intercept", 0.0711f);
  tdsCoef      = prefs.getFloat("tds_coef", 0.6400f);

  // Старт на LCD дисплея
  lcd.init();
  lcd.backlight();
  printLcdLine(0, "ASOA IoT v" FW_VERSION);
  printLcdLine(1, "Startirane...");

  // Инициализация на ADS1115
  if (ads.begin()) {
    ads.setGain(GAIN_FOUR);
    adsAvailable = true;
    webLog("[INIT] ADS1115 OK");
  } else {
    webLog("[ERROR] ADS1115 ne e nameren na I2C!");
    printLcdLine(1, "Greshka ADS1115!");
    delay(2000);
  }

  // Старт на DS18B20 (асинхронно) и DHT11
  tempSensor.begin();
  tempSensor.setWaitForConversion(false);
  tempSensor.requestTemperatures();
  dht.begin();

  // Свързване към някоя от зададените Wi-Fi мрежи
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  connectToKnownWiFi();

  webLog("[SYS] Start v" FW_VERSION " | Reset: " + bootResetReason + " | Slope: " + String(calSlope, 4));

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

  // 3. Запис на данните (или в офлайн буфера) и проверка за OTA / настройки
  static unsigned long lastCloudSync = 0;
  if (millis() - lastCloudSync >= sendInterval) {
    lastCloudSync = millis();
    handleDataSync();
    checkRemoteControl();
  }
}

bool connectToKnownWiFi() {
  for (int i = 0; i < WIFI_NETWORK_COUNT; i++) {
    // Превантивно нулиране на Watchdog при опит за връзка в loop()
    if (esp_task_wdt_status(NULL) == ESP_OK) {
      esp_task_wdt_reset();
    }

    String currentSsid = String(WIFI_NETWORKS[i].ssid);
    webLog("[WIFI] Opit za svarzvane kam: " + currentSsid);

    printLcdLine(0, "Wi-Fi Tyrsene...");
    printLcdLine(1, currentSsid.c_str());

    WiFi.disconnect(true);
    delay(100);
    WiFi.begin(WIFI_NETWORKS[i].ssid, WIFI_NETWORKS[i].password);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 8000) {
      delay(250);
    }

    if (WiFi.status() == WL_CONNECTED) {
      webLog("[WIFI] Svarzan kam: " + WiFi.SSID() +
             " | IP: " + WiFi.localIP().toString() +
             " | Signal: " + String(WiFi.RSSI()) + " dBm");

      printLcdLine(0, "Wi-Fi Svarzan!");
      printLcdLine(1, WiFi.SSID().c_str());
      delay(1200);
      return true;
    } else {
      webLog("[WIFI] Neuspeshno kam: " + currentSsid);
    }
  }

  webLog("[WIFI] Nyama vrazka s nito edna mreja - aktiviran offline bufer.");
  printLcdLine(0, "Wi-Fi: Nyama");
  printLcdLine(1, "Offline rejim");
  delay(1200);
  return false;
}

void checkWiFi() {
  if (WiFi.status() != WL_CONNECTED) {
    static unsigned long lastRetry = 0;
    if (millis() - lastRetry > 20000) {
      lastRetry = millis();
      webLog("[WIFI] Zagubena vrazka! Tyrsene na nalichna mreja...");
      connectToKnownWiFi();
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

void handleDataSync() {
  SensorRecord currentRec = {
    roundf(lastWaterTemp * 10) / 10.0f,
    roundf(lastAirTemp   * 10) / 10.0f,
    roundf(lastAirHum    * 10) / 10.0f,
    roundf(lastEC        * 100) / 100.0f,
    roundf(lastTDS       * 100) / 100.0f,
    lastWL,
    WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : -100,
    millis() / 1000UL,
    ESP.getFreeHeap()
  };

  if (WiFi.status() == WL_CONNECTED) {
    // Ако има натрупани офлайн записи, първо изпращаме тях
    if (offlineCount > 0) {
      flushOfflineBuffer();
    }
    // Изпращаме текущия запис
    if (postSingleRecord(currentRec, offlineCount)) {
      webLog("[DB] Dannite sa zapisani v Supabase.");
    } else {
      webLog("[DB ERROR] Neuspeshen zapis - premestvane v bufera.");
      if (offlineCount < MAX_OFFLINE_RECORDS) {
        offlineBuffer[offlineCount++] = currentRec;
      }
    }
  } else {
    // Няма интернет -> пазим в RAM буфера
    if (offlineCount < MAX_OFFLINE_RECORDS) {
      offlineBuffer[offlineCount++] = currentRec;
      Serial.printf("[BUF] Offline zapis #%d v RAM.\n", offlineCount);
    } else {
      // Изместваме най-стария запис при препълване
      for (int i = 1; i < MAX_OFFLINE_RECORDS; i++) {
        offlineBuffer[i - 1] = offlineBuffer[i];
      }
      offlineBuffer[MAX_OFFLINE_RECORDS - 1] = currentRec;
    }
  }
}

void flushOfflineBuffer() {
  webLog("[BUF] Vazstanovena vrazka! Izprashtane na " + String(offlineCount) + " offline zapisa...");
  int sent = 0;
  for (int i = 0; i < offlineCount; i++) {
    esp_task_wdt_reset();
    if (postSingleRecord(offlineBuffer[i], offlineCount - i - 1)) {
      sent++;
      delay(80);
    } else {
      break;
    }
  }
  if (sent > 0) {
    for (int i = sent; i < offlineCount; i++) {
      offlineBuffer[i - sent] = offlineBuffer[i];
    }
    offlineCount -= sent;
    webLog("[BUF] Uspeshno kacheni " + String(sent) + " zapisa.");
  }
}

bool postSingleRecord(const SensorRecord& rec, int bufLeft) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  http.begin(client, String(SUPABASE_URL) + "/rest/v1/sensor_data");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Prefer", "return=minimal");

  StaticJsonDocument<420> doc;
  doc["temp"]           = rec.temp;
  doc["air_temp"]       = rec.air_temp;
  doc["air_hum"]        = rec.air_hum;
  doc["ec"]             = rec.ec;
  doc["tds"]            = rec.tds;
  doc["wl"]             = rec.wl;
  doc["rssi"]           = rec.rssi;
  doc["ver"]            = FW_VERSION;
  doc["uptime_sec"]     = rec.uptime_sec;
  doc["free_heap"]      = rec.free_heap;
  doc["reset_reason"]   = bootResetReason;
  doc["buffered_count"] = bufLeft;

  String payload;
  serializeJson(doc, payload);
  int code = http.POST(payload);
  http.end();
  return (code == 201 || code == 200);
}

void checkRemoteControl() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  String url = String(SUPABASE_URL) + "/rest/v1/device_control?device_id=eq." + DEVICE_ID + "&select=*";
  http.begin(client, url);
  http.addHeader("apikey", SUPABASE_KEY);

  if (http.GET() == 200) {
    StaticJsonDocument<1024> doc;
    if (!deserializeJson(doc, http.getString()) && doc.size() > 0) {
      bool triggerOta    = doc[0]["trigger_ota"] | false;
      bool triggerReboot = doc[0]["trigger_reboot"] | false;
      int newInterval    = doc[0]["send_interval_sec"] | 60;
      String binUrl      = doc[0]["firmware_url"].as<String>();

      // Проверка за нови калибрационни коефициенти от Admin панела
      float newSlope     = doc[0]["cal_slope"] | calSlope;
      float newIntercept = doc[0]["cal_intercept"] | calIntercept;
      float newTdsCoef   = doc[0]["tds_coef"] | tdsCoef;

      if (fabs(newSlope - calSlope) > 0.0001f ||
          fabs(newIntercept - calIntercept) > 0.0001f ||
          fabs(newTdsCoef - tdsCoef) > 0.0001f) {
        calSlope     = newSlope;
        calIntercept = newIntercept;
        tdsCoef      = newTdsCoef;

        prefs.putFloat("slope", calSlope);
        prefs.putFloat("intercept", calIntercept);
        prefs.putFloat("tds_coef", tdsCoef);

        webLog("[CAL] Obnovena kalibracia: Slope=" + String(calSlope, 4) +
               " Offset=" + String(calIntercept, 4) + " TDS=" + String(tdsCoef, 4));
      }

      // Актуализиране на интервала на изпращане
      if (newInterval >= 5 && newInterval <= 3600) {
        sendInterval = (unsigned long)newInterval * 1000UL;
      }

      // Отдалечен рестарт от Admin панела
      if (triggerReboot) {
        http.end();

        HTTPClient patchHttp;
        patchHttp.begin(client, String(SUPABASE_URL) + "/rest/v1/device_control?device_id=eq." + DEVICE_ID);
        patchHttp.addHeader("Content-Type", "application/json");
        patchHttp.addHeader("apikey", SUPABASE_KEY);
        patchHttp.PATCH("{\"trigger_reboot\": false}");
        patchHttp.end();

        webLog("[SYS] Poluchena komanda za otdalechen restart!");
        flushLogsToSupabase();
        delay(500);
        ESP.restart();
      }

      // Отдалечен OTA ъпдейт
      if (triggerOta && binUrl.length() > 10) {
        http.end();

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

  printLcdLine(0, "OTA Update...");
  printLcdLine(1, "Izteglyane...");

  esp_task_wdt_delete(NULL);

  WiFiClientSecure client;
  client.setInsecure();
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  httpUpdate.rebootOnUpdate(true);

  t_httpUpdate_return ret = httpUpdate.update(client, binUrl);
  if (ret == HTTP_UPDATE_FAILED) {
    webLog("[OTA ERROR] " + httpUpdate.getLastErrorString());
    printLcdLine(1, "OTA Greshka!");
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

  // ADS1115 (EC / TDS с усредняване и динамична калибрация)
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

  lastEC = (voltageRaw > 0.005f) ? max(0.0f, (calSlope * compV) + calIntercept) : 0.0f;
  lastTDS = lastEC * tdsCoef;
}

void updateLCD() {
  static bool scr = true;
  char line0[17];
  char line1[17];

  if (scr) {
    // Екран 1: Температура на водата, EC и TDS
    snprintf(line0, sizeof(line0), "Voda:%4.1fC EC:%4.2f", lastWaterTemp, lastEC);
    snprintf(line1, sizeof(line1), "TDS: %5.2f PPT", lastTDS);
  } else {
    // Екран 2: Въздух (темп. и влажност) + статус на мрежата
    snprintf(line0, sizeof(line0), "Vazd:%4.1fC Vl:%2d%%", lastAirTemp, (int)lastAirHum);
    if (WiFi.status() == WL_CONNECTED) {
      snprintf(line1, sizeof(line1), "WiFi:%-11.11s", WiFi.SSID().c_str());
    } else {
      snprintf(line1, sizeof(line1), "Offline Buf:%d", offlineCount);
    }
  }

  printLcdLine(0, line0);
  printLcdLine(1, line1);
  scr = !scr;
}

void checkScheduledReset() {
  if (millis() >= REBOOT_INTERVAL) {
    webLog("[SYS] Planiran 6-chasov restart...");
    flushLogsToSupabase();
    ESP.restart();
  }
}
