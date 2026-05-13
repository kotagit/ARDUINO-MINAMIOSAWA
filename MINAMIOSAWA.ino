//全畝個別潅水時間

#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <time.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>

// Wi-Fi情報
const char* ssid = "NASU";
const char* password = "19141001";

WebServer server(80);

// ピン割り当て
const int RELAY_0 = 13; // ポンプ
const int RELAY_1 = 14; // バルブ1 (通常オープン)：オクラ
const int RELAY_2 = 15; // バルブ2 (通常クローズ)：大玉トマト
const int RELAY_3 = 16; // バルブ3 (通常クローズ)：茄子
const int RELAY_4 = 17; // バルブ4 (通常オープン)：ピーマン

// ----------------------------------------------------
// 時間設定（ここを変更するだけで動作時間を調整できる）
// ----------------------------------------------------

// スケジュール自動起動時刻（SCHEDULE_MINUTEは0〜58で設定すること）
const int SCHEDULE_HOUR   = 5;
const int SCHEDULE_MINUTE = 0;

// 各バルブを開けてからポンプを開くまでの待機時間（ミリ秒）
const unsigned long PUMP_DELAY = 8000;

// 各バルブの通水時間 ― 通常スケジュール（ミリ秒）
const unsigned long WATER_TIME_V1 = 2 * 60 * 1000UL;  // バルブ1: 10分 (※元コードの注釈ママ)
const unsigned long WATER_TIME_V2 = 2 * 60 * 1000UL;  // バルブ2: 10分
const unsigned long WATER_TIME_V3 = 5 * 60 * 1000UL;  // バルブ3: 10分
const unsigned long WATER_TIME_V4 = 2 * 60 * 1000UL;  // バルブ4: 10分

// 手動通水の自動停止時間（ミリ秒）
const unsigned long MANUAL_TIMEOUT = 10 * 60 * 1000UL;

// 各バルブの通水時間 ― 通水テスト（速）（ミリ秒）
const unsigned long TEST_FAST_PUMP_DELAY    = 10000;
const unsigned long TEST_FAST_WATER_TIME_V1 = 5000;  // バルブ1: 15秒
const unsigned long TEST_FAST_WATER_TIME_V2 = 5000;  // バルブ2: 15秒
const unsigned long TEST_FAST_WATER_TIME_V3 = 20000;  // バルブ3: 15秒
const unsigned long TEST_FAST_WATER_TIME_V4 = 5000;  // バルブ4: 15秒

// ----------------------------------------------------
// スケジュール管理
// ----------------------------------------------------
bool scheduleRunning = false;
bool scheduleDone    = false;
int  scheduleStep    = 0;
unsigned long scheduleStepStart = 0;
unsigned long currentPumpDelay = PUMP_DELAY;
unsigned long currentWaterTimes[4] = {0, 0, 0, 0}; // バルブ1〜4の通水時間

// 手動通水タイマー管理
bool   manualRunning  = false;
int    manualValve    = 0;
unsigned long manualStart = 0;
bool   manualPumpOn   = false;
unsigned long manualPumpStart = 0;

// WiFi再接続用のタイマー
unsigned long lastWiFiCheckTime = 0;

// ----------------------------------------------------
// OTAをブロックしない安全なdelay
// ----------------------------------------------------
void safeDelay(unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) {
    ArduinoOTA.handle();
    server.handleClient();
    delay(10);
  }
}

// ----------------------------------------------------
// NTP経由で時刻取得
// ----------------------------------------------------
void syncTimeViaNTP() {
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  setenv("TZ", "JST-9", 1);
  tzset();

  Serial.print("Waiting for NTP");
  time_t now = time(nullptr);
  int retry = 0;
  while (now < 1700000000 && retry < 40) {
    delay(500);
    Serial.print(".");
    now = time(nullptr);
    retry++;
  }
  Serial.printf("\ntime_t value: %ld\n", (long)now);
  if (now > 1700000000) {
    Serial.println("NTP OK");
  } else {
    Serial.println("NTP FAILED");
  }
}

// ----------------------------------------------------
// 全停止
// ----------------------------------------------------
void allOff() {
  digitalWrite(RELAY_0, LOW);
  digitalWrite(RELAY_1, LOW);
  digitalWrite(RELAY_2, LOW);
  digitalWrite(RELAY_3, LOW);
  digitalWrite(RELAY_4, LOW);
  Serial.println("Action: ALL OFF");
}

// ----------------------------------------------------
// 各バルブの通水準備
// ----------------------------------------------------
void prepareValve1() {
  digitalWrite(RELAY_0, LOW);
  digitalWrite(RELAY_1, LOW);
  digitalWrite(RELAY_2, LOW);
  digitalWrite(RELAY_3, LOW);
  digitalWrite(RELAY_4, HIGH);
}

void prepareValve2() {
  digitalWrite(RELAY_0, LOW);
  digitalWrite(RELAY_1, HIGH);
  digitalWrite(RELAY_2, HIGH);
  digitalWrite(RELAY_3, LOW);
  digitalWrite(RELAY_4, HIGH);
}

void prepareValve3() {
  digitalWrite(RELAY_0, LOW);
  digitalWrite(RELAY_1, HIGH);
  digitalWrite(RELAY_2, LOW);
  digitalWrite(RELAY_3, HIGH);
  digitalWrite(RELAY_4, HIGH);
}

void prepareValve4() {
  digitalWrite(RELAY_0, LOW);
  digitalWrite(RELAY_1, HIGH);
  digitalWrite(RELAY_2, LOW);
  digitalWrite(RELAY_3, LOW);
  digitalWrite(RELAY_4, LOW);
}

// ----------------------------------------------------
// スケジュール開始
// ----------------------------------------------------
void startSchedule(unsigned long pumpDelay, unsigned long waterTimes[4]) {
  allOff();
  scheduleRunning   = true;
  scheduleDone      = false;
  scheduleStep      = 0;
  scheduleStepStart = millis();
  currentPumpDelay  = pumpDelay;
  for (int i = 0; i < 4; i++) currentWaterTimes[i] = waterTimes[i];
  Serial.println("Schedule: Start");
}

// ----------------------------------------------------
// HTML生成
// ----------------------------------------------------
String getHTML() {
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  char timebuf[32];
  strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", t);

  String status = "";
  if (scheduleRunning) {
    int valveNum = (scheduleStep / 2) + 1;
    if (valveNum > 4) valveNum = 4;
    status = "スケジュール実行中（バルブ" + String(valveNum) + "）";
  } else if (manualRunning) {
    status = manualValve == 0 ? "全バルブ手動通水中" : "バルブ" + String(manualValve) + " 手動通水中";
  } else {
    status = "待機中";
  }

  String html = "<!DOCTYPE html><html lang=\"ja\"><head><meta charset=\"UTF-8\">";
  html += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
  html += "<meta http-equiv=\"refresh\" content=\"10\">";
  html += "<title>水やりリモコン</title>";
  html += "<style>";
  html += "body { font-family: sans-serif; text-align: center; background-color: #f0f2f5; padding: 20px; }";
  html += ".container { max-width: 450px; margin: auto; background: white; padding: 20px; border-radius: 15px; box-shadow: 0 4px 6px rgba(0,0,0,0.1); }";
  html += ".btn { display: block; width: 100%; margin: 10px 0; padding: 15px; font-size: 18px; text-decoration: none; border-radius: 8px; color: white; font-weight: bold; border: none; box-sizing: border-box; }";
  html += ".btn-blue { background-color: #1a73e8; }";
  html += ".btn-gray { background-color: #888; }";
  html += ".btn-green { background-color: #34a853; }";
  html += ".btn-orange { background-color: #fb8c00; }";
  html += ".btn-yellow { background-color: #f9a825; }";
  html += ".btn-red { background-color: #ea4335; }";
  html += ".label { font-size: 1em; font-weight: bold; margin-top: 15px; text-align: left; color: #333; }";
  html += ".status { font-size: 0.9em; background: #e8f5e9; padding: 10px; border-radius: 8px; margin-bottom: 15px; }";
  html += "hr { margin: 10px 0; border: 0; border-top: 1px solid #ddd; }";
  html += "</style></head><body>";
  html += "<div class=\"container\"><h1>水やり制御パネル</h1>";
  html += "<div class=\"status\">現在時刻: " + String(timebuf) + "<br>状態: " + status + "</div>";

  html += "<div class=\"label\">バルブ 1</div>";
  html += "<a href=\"/valve1_on\" class=\"btn btn-blue\">バルブ1 通水</a>";
  html += "<a href=\"/valve1_off\" class=\"btn btn-gray\">バルブ1 止水</a>";
  html += "<hr>";

  html += "<div class=\"label\">バルブ 2</div>";
  html += "<a href=\"/valve2_on\" class=\"btn btn-blue\">バルブ2 通水</a>";
  html += "<a href=\"/valve2_off\" class=\"btn btn-gray\">バルブ2 止水</a>";
  html += "<hr>";

  html += "<div class=\"label\">バルブ 3</div>";
  html += "<a href=\"/valve3_on\" class=\"btn btn-blue\">バルブ3 通水</a>";
  html += "<a href=\"/valve3_off\" class=\"btn btn-gray\">バルブ3 止水</a>";
  html += "<hr>";

  html += "<div class=\"label\">バルブ 4</div>";
  html += "<a href=\"/valve4_on\" class=\"btn btn-blue\">バルブ4 通水</a>";
  html += "<a href=\"/valve4_off\" class=\"btn btn-gray\">バルブ4 止水</a>";
  html += "<hr>";

  html += "<div class=\"label\">全バルブ</div>";
  html += "<a href=\"/all_on\" class=\"btn btn-green\">全バルブ 通水</a>";
  html += "<a href=\"/all_off\" class=\"btn btn-red\">全バルブ 止水</a>";
  html += "<hr>";

  html += "<div class=\"label\">テスト</div>";
  html += "<a href=\"/test\" class=\"btn btn-orange\">通水テスト（バルブ1→4順番）</a>";
  html += "<a href=\"/test_fast\" class=\"btn btn-yellow\">通水テスト（速）</a>";

  html += "</div></body></html>";
  return html;
}

// ----------------------------------------------------
// 手動通水開始
// ----------------------------------------------------
void startManual(int valveNum) {
  allOff();
  switch (valveNum) {
    case 1: prepareValve1(); break;
    case 2: prepareValve2(); break;
    case 3: prepareValve3(); break;
    case 4: prepareValve4(); break;
  }
  manualValve     = valveNum;
  manualRunning   = true;
  manualPumpOn    = false;
  manualStart     = millis();
  manualPumpStart = millis();
  Serial.printf("Manual valve%d start\n", valveNum);
}

// ----------------------------------------------------
// ハンドラ
// ----------------------------------------------------
void handleRoot() { server.send(200, "text/html", getHTML()); }

void handleValve1On()  { startManual(1); server.sendHeader("Location", "/"); server.send(303); }
void handleValve2On()  { startManual(2); server.sendHeader("Location", "/"); server.send(303); }
void handleValve3On()  { startManual(3); server.sendHeader("Location", "/"); server.send(303); }
void handleValve4On()  { startManual(4); server.sendHeader("Location", "/"); server.send(303); }

void handleValve1Off() { allOff(); manualRunning = false; server.sendHeader("Location", "/"); server.send(303); }
void handleValve2Off() { allOff(); manualRunning = false; server.sendHeader("Location", "/"); server.send(303); }
void handleValve3Off() { allOff(); manualRunning = false; server.sendHeader("Location", "/"); server.send(303); }
void handleValve4Off() { allOff(); manualRunning = false; server.sendHeader("Location", "/"); server.send(303); }

void handleAllOn() {
  allOff();
  digitalWrite(RELAY_2, HIGH);
  digitalWrite(RELAY_3, HIGH);
  manualValve     = 0;
  manualRunning   = true;
  manualPumpOn    = false;
  manualStart     = millis();
  manualPumpStart = millis();
  server.sendHeader("Location", "/"); server.send(303);
}

void handleAllOff() {
  allOff();
  manualRunning   = false;
  scheduleRunning = false;
  server.sendHeader("Location", "/"); server.send(303);
}

void handleTest() {
  unsigned long normalTimes[4] = {WATER_TIME_V1, WATER_TIME_V2, WATER_TIME_V3, WATER_TIME_V4};
  startSchedule(PUMP_DELAY, normalTimes);
  server.sendHeader("Location", "/"); server.send(303);
}

void handleTestFast() {
  unsigned long testTimes[4] = {TEST_FAST_WATER_TIME_V1, TEST_FAST_WATER_TIME_V2, TEST_FAST_WATER_TIME_V3, TEST_FAST_WATER_TIME_V4};
  startSchedule(TEST_FAST_PUMP_DELAY, testTimes);
  server.sendHeader("Location", "/"); server.send(303);
}

// ----------------------------------------------------
// スケジュール処理（loop内で呼ぶ）
// ----------------------------------------------------
void handleSchedule() {
  unsigned long now = millis();

  if (scheduleStep == 0) {
    prepareValve1();
    scheduleStep = 1;
    scheduleStepStart = now;
    Serial.println("Schedule: Valve1 prepare");
    return;
  }
  if (scheduleStep == 1 && now - scheduleStepStart >= currentPumpDelay) {
    digitalWrite(RELAY_0, HIGH);
    scheduleStep = 2;
    scheduleStepStart = now;
    Serial.println("Schedule: Valve1 pump ON");
    return;
  }
  if (scheduleStep == 2 && now - scheduleStepStart >= currentWaterTimes[0]) {
    prepareValve2();
    scheduleStep = 3;
    scheduleStepStart = now;
    Serial.println("Schedule: Valve2 prepare");
    return;
  }
  if (scheduleStep == 3 && now - scheduleStepStart >= currentPumpDelay) {
    digitalWrite(RELAY_0, HIGH);
    scheduleStep = 4;
    scheduleStepStart = now;
    Serial.println("Schedule: Valve2 pump ON");
    return;
  }
  if (scheduleStep == 4 && now - scheduleStepStart >= currentWaterTimes[1]) {
    prepareValve3();
    scheduleStep = 5;
    scheduleStepStart = now;
    Serial.println("Schedule: Valve3 prepare");
    return;
  }
  if (scheduleStep == 5 && now - scheduleStepStart >= currentPumpDelay) {
    digitalWrite(RELAY_0, HIGH);
    scheduleStep = 6;
    scheduleStepStart = now;
    Serial.println("Schedule: Valve3 pump ON");
    return;
  }
  if (scheduleStep == 6 && now - scheduleStepStart >= currentWaterTimes[2]) {
    prepareValve4();
    scheduleStep = 7;
    scheduleStepStart = now;
    Serial.println("Schedule: Valve4 prepare");
    return;
  }
  if (scheduleStep == 7 && now - scheduleStepStart >= currentPumpDelay) {
    digitalWrite(RELAY_0, HIGH);
    scheduleStep = 8;
    scheduleStepStart = now;
    Serial.println("Schedule: Valve4 pump ON");
    return;
  }
  if (scheduleStep == 8 && now - scheduleStepStart >= currentWaterTimes[3]) {
    allOff();
    scheduleRunning = false;
    scheduleDone    = true;
    Serial.println("Schedule: Complete");
    return;
  }
}

// ----------------------------------------------------
// 手動タイマー処理（loop内で呼ぶ）
// ----------------------------------------------------
void handleManualTimer() {
  unsigned long now = millis();

  if (!manualPumpOn && now - manualPumpStart >= PUMP_DELAY) {
    digitalWrite(RELAY_0, HIGH);
    manualPumpOn = true;
    Serial.println("Manual: Pump ON");
  }

  if (now - manualStart >= MANUAL_TIMEOUT + PUMP_DELAY) {
    allOff();
    manualRunning = false;
    Serial.println("Manual: Timer expired, ALL OFF");
  }
}

// ----------------------------------------------------
// Setup
// ----------------------------------------------------
void setup() {
  Serial.begin(115200);

  pinMode(RELAY_0, OUTPUT);
  pinMode(RELAY_1, OUTPUT);
  pinMode(RELAY_2, OUTPUT);
  pinMode(RELAY_3, OUTPUT);
  pinMode(RELAY_4, OUTPUT);

  digitalWrite(RELAY_0, LOW);
  digitalWrite(RELAY_1, LOW);
  digitalWrite(RELAY_2, LOW);
  digitalWrite(RELAY_3, LOW);
  digitalWrite(RELAY_4, LOW);

  Serial.println("\nWiFi Resetting...");
  WiFi.disconnect(true, true);
  delay(1000);
  WiFi.mode(WIFI_STA);

  IPAddress local_IP(192, 168, 68, 22);
  IPAddress gateway(192, 168, 68, 1);
  IPAddress subnet(255, 255, 255, 0);
  IPAddress dns(8, 8, 8, 8);
  WiFi.config(local_IP, gateway, subnet, gateway, dns);

  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");

  int count = 0;
  while (WiFi.status() != WL_CONNECTED && count < 20) {
    delay(500);
    Serial.print(".");
    count++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi Connected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());

    syncTimeViaNTP();

    ArduinoOTA.setHostname("esp32-water-system");
//  ArduinoOTA.setPassword("1914");
    ArduinoOTA.begin();
    Serial.println("OTA Ready!");
  } else {
    Serial.println("\nWiFi Connection Failed. Check SSID/Password.");
  }

  server.on("/",           handleRoot);
  server.on("/valve1_on",  handleValve1On);
  server.on("/valve1_off", handleValve1Off);
  server.on("/valve2_on",  handleValve2On);
  server.on("/valve2_off", handleValve2Off);
  server.on("/valve3_on",  handleValve3On);
  server.on("/valve3_off", handleValve3Off);
  server.on("/valve4_on",  handleValve4On);
  server.on("/valve4_off", handleValve4Off);
  server.on("/all_on",     handleAllOn);
  server.on("/all_off",    handleAllOff);
  server.on("/test",       handleTest);
  server.on("/test_fast",  handleTestFast);
  server.begin();
}

// ----------------------------------------------------
// Loop
// ----------------------------------------------------
void loop() {
  unsigned long currentMillis = millis();

  // 10秒おきにWiFi接続状態をチェックし、切断されていれば再接続（ノンブロッキング）
  if (currentMillis - lastWiFiCheckTime >= 10000) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi Disconnected. Attempting to reconnect...");
      WiFi.reconnect();
    }
    lastWiFiCheckTime = currentMillis;
  }

  ArduinoOTA.handle();
  server.handleClient();

  time_t now = time(nullptr);
  struct tm* t = localtime(&now);

  // 毎日SCHEDULE_HOUR:SCHEDULE_MINUTEに自動スケジュール起動
  if (t->tm_hour == SCHEDULE_HOUR && t->tm_min == SCHEDULE_MINUTE && t->tm_sec == 0) {
    if (!scheduleRunning && !scheduleDone) {
      unsigned long normalTimes[4] = {WATER_TIME_V1, WATER_TIME_V2, WATER_TIME_V3, WATER_TIME_V4};
      startSchedule(PUMP_DELAY, normalTimes);
    }
  }

  // スケジュール設定時間「以外」の時に、翌日に備えてフラグを確実に戻す
  if (t->tm_hour != SCHEDULE_HOUR) {
    scheduleDone = false;
  }

  if (scheduleRunning) handleSchedule();
  if (manualRunning)   handleManualTimer();

  delay(10);
}
