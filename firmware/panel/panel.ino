#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <TFT_eSPI.h>

// ===================== WIFI / OTA =====================
const char* AP_SSID = "ESP32-OTA";
const char* AP_PASS = "12345678";
WebServer server(80);

// ===================== DISPLAY =====================
#define TFT_BL 32
TFT_eSPI tft;

// ===================== OTA SETUP =====================
void setupOTA() {
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html",
      "<h2>ESP32 Web OTA</h2>"
      "<p>Firmware Update:</p>"
      "<a href='/update'>/update</a>");
  });

  server.on("/update", HTTP_GET, []() {
    server.send(200, "text/html",
      "<form method='POST' action='/update' enctype='multipart/form-data'>"
      "<input type='file' name='update'>"
      "<input type='submit' value='Upload'>"
      "</form>");
  });

  server.on(
    "/update",
    HTTP_POST,
    []() {
      server.send(200, "text/plain",
                  Update.hasError() ? "OTA FAILED" : "OTA OK – Reboot");
      delay(500);
      ESP.restart();
    },
    []() {
      HTTPUpload& upload = server.upload();
      if (upload.status == UPLOAD_FILE_START) {
        Update.begin();
      } else if (upload.status == UPLOAD_FILE_WRITE) {
        Update.write(upload.buf, upload.currentSize);
      } else if (upload.status == UPLOAD_FILE_END) {
        Update.end(true);
      }
    }
  );

  server.begin();
}

// ===================== DISPLAY TEST =====================
void drawDisplayTest() {
  tft.fillScreen(TFT_BLACK);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);

  tft.setCursor(20, 20);
  tft.println("ST7796S DISPLAY TEST");

  tft.setCursor(20, 60);
  tft.println("Wenn das ruhig ist,");

  tft.setCursor(20, 90);
  tft.println("ist alles OK.");

  // Farbflächen (zeigen sofort Fehler)
  tft.fillRect(0, 140, 480, 40, TFT_RED);
  tft.fillRect(0, 180, 480, 40, TFT_GREEN);
  tft.fillRect(0, 220, 480, 40, TFT_BLUE);
  tft.fillRect(0, 260, 480, 40, TFT_YELLOW);

  tft.setCursor(20, 310);
  tft.setTextSize(1);
  tft.println("AP: ESP32-OTA  |  OTA: /update");
}

// ===================== SETUP =====================
void setup() {
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  tft.init();
  tft.setRotation(1); // 480x320
  drawDisplayTest();

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);

  setupOTA();
}

// ===================== LOOP =====================
void loop() {
  server.handleClient();
}
