#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <TFT_eSPI.h>

// =====================
// WIFI AP
// =====================
const char* ap_ssid = "ESP32-OTA";
const char* ap_pass = "12345678";

// =====================
// SERVER
// =====================
WebServer server(80);

// =====================
// TFT
// =====================
TFT_eSPI tft = TFT_eSPI();

// =====================
// OTA PAGE
// =====================
const char* uploadPage =
"<html><body>"
"<h2>ESP32 Web OTA</h2>"
"<form method='POST' action='/update' enctype='multipart/form-data'>"
"<input type='file' name='update'>"
"<input type='submit' value='Update'>"
"</form>"
"</body></html>";

void handleRoot() {
  server.send(200, "text/html", uploadPage);
}

void handleUpdate() {
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    Update.begin(UPDATE_SIZE_UNKNOWN);
  } 
  else if (upload.status == UPLOAD_FILE_WRITE) {
    Update.write(upload.buf, upload.currentSize);
  } 
  else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      server.send(200, "text/plain", "Update OK. Rebooting...");
      delay(1000);
      ESP.restart();
    } else {
      server.send(500, "text/plain", "Update FAILED");
    }
  }
}

void setup() {
  Serial.begin(115200);

  // =====================
  // TFT INIT
  // =====================
  tft.init();
  tft.setRotation(1); // Landscape
  tft.fillScreen(TFT_BLACK);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(20, 20);
  tft.println("ESP32 TFT OK");
  tft.println("ST7796S");
  tft.println("Web OTA aktiv");

  // =====================
  // WIFI AP
  // =====================
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ap_ssid, ap_pass);

  IPAddress ip = WiFi.softAPIP();
  Serial.print("AP IP: ");
  Serial.println(ip);

  // =====================
  // WEB
  // =====================
  server.on("/", HTTP_GET, handleRoot);
  server.on("/update", HTTP_POST, []() {
    server.send(200);
  }, handleUpdate);

  server.begin();
}

void loop() {
  server.handleClient();
}
