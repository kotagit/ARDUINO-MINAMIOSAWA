#include <WiFi.h>

const char* WIFI_SSID = "YOUR_SSID";
const char* WIFI_PASSWORD = "YOUR_PASSWORD";

void setup() {
  Serial.begin(115200);
  Serial.println("MINAMIOSAWA ESP32 Starting...");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Connected: ");
  Serial.println(WiFi.localIP());
}

void loop() {
  // TODO: メイン処理を追加
  delay(1000);
}
