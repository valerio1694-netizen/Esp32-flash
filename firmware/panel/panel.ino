#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <TFT_eSPI.h>

// ===== AP Daten =====
static const char* AP_SSID = "ESP32-OTA";
static const char* AP_PASS = "12345678";

// ===== Webserver =====
WebServer server(80);

// ===== TFT =====
TFT_eSPI tft = TFT_eSPI();

// ===== OTA Seite =====
static const char* OTA_PAGE =
"<html><body>"
"<h2>ESP32 Web OTA</h2>"
"<p>WLAN: <b>ESP32-OTA</b> (Pass: 12345678)</p>"
"<p>OTA: <a href='/update'>/update</a></p>"
"<form method='POST' action='/update' enctype='multipart/form-data'>"
"<input type='file' name='update' accept='.bin'>"
"<input type='submit' value='Upload'>"
"</form>"
"</body></html>";

void handleRoot() {
  server.send(200, "text/html", OTA_PAGE);
}

void handleUpdateUpload() {
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    Update.begin(UPDATE_SIZE_UNKNOWN);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    Update.write(upload.buf, upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      server.send(200, "text/plain", "OK. Rebooting...");
      delay(300);
      ESP.restart();
    } else {
      server.send(500, "text/plain", "FAIL");
    }
  }
}

void drawDisplayTest() {
  // Wenn SPI/CS/DC/RST stimmt, MUSS man die Flächen sehen
  tft.fillScreen(TFT_RED);
  delay(250);
  tft.fillScreen(TFT_GREEN);
  delay(250);
  tft.fillScreen(TFT_BLUE);
  delay(250);
  tft.fillScreen(TFT_BLACK);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 10);
  tft.println("DISPLAY TEST");
  tft.println("ST7796 (TFT_eSPI)");

  tft.setCursor(10, 70);
  tft.print("AP: ");
  tft.println(AP_SSID);

  tft.setCursor(10, 95);
  tft.print("IP: ");
  tft.println(WiFi.softAPIP());

  tft.setCursor(10, 120);
  tft.println("OTA: /update");

  // Farb-Balken unten
  int w = tft.width() / 5;
  int y = tft.height() - 40;
  tft.fillRect(0*w, y, w, 40, TFT_RED);
  tft.fillRect(1*w, y, w, 40, TFT_GREEN);
  tft.fillRect(2*w, y, w, 40, TFT_BLUE);
  tft.fillRect(3*w, y, w, 40, TFT_YELLOW);
  tft.fillRect(4*w, y, w, 40, TFT_MAGENTA);
}

void setup() {
  Serial.begin(115200);

  // AP starten (damit IP sofort da ist für Displaytext)
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  delay(200);

  // TFT
  tft.init();
  tft.setRotation(1); // Querformat 480x320 bei ST7796-480x320 Panels
  drawDisplayTest();

  // Web / OTA
  server.on("/", HTTP_GET, handleRoot);
  server.on("/update", HTTP_GET, handleRoot);
  server.on("/update", HTTP_POST, []() { server.send(200); }, handleUpdateUpload);
  server.begin();
}

void loop() {
  server.handleClient();
}
