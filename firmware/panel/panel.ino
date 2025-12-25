#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>

#include <TFT_eSPI.h>   // benutzt User_Setup.h

/* =========================
   WLAN / OTA (Web-OTA)
   ========================= */
const char* ssid     = "DEIN_WLAN";
const char* password = "DEIN_PASSWORT";

WebServer server(80);
TFT_eSPI tft = TFT_eSPI();

/* =========================
   OTA Handler
   ========================= */
void handleRoot() {
  server.send(200, "text/html",
    "<form method='POST' action='/update' enctype='multipart/form-data'>"
    "<input type='file' name='update'>"
    "<input type='submit' value='Update'>"
    "</form>");
}

void handleUpdate() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    Update.begin();
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    Update.write(upload.buf, upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END) {
    Update.end(true);
  }
}

void setup() {
  Serial.begin(115200);

  /* ===== TFT ===== */
  tft.init();
  tft.setRotation(1);          // WICHTIG für dein Panel
  tft.fillScreen(TFT_BLACK);

  // Testanzeige
  tft.fillRect(0, 0, 320, 40, TFT_DARKCYAN);
  tft.setTextColor(TFT_WHITE, TFT_DARKCYAN);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("ST7796S OK", 160, 20, 4);

  // Farbbalken
  int w = 320 / 5;
  tft.fillRect(0*w, 60, w, 420, TFT_RED);
  tft.fillRect(1*w, 60, w, 420, TFT_GREEN);
  tft.fillRect(2*w, 60, w, 420, TFT_BLUE);
  tft.fillRect(3*w, 60, w, 420, TFT_YELLOW);
  tft.fillRect(4*w, 60, w, 420, TFT_MAGENTA);

  /* ===== WLAN ===== */
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
  }

  /* ===== OTA Web ===== */
  server.on("/", HTTP_GET, handleRoot);
  server.on("/update", HTTP_POST,
    []() { server.sendHeader("Connection", "close"); server.send(200, "text/plain", "OK"); ESP.restart(); },
    handleUpdate
  );
  server.begin();
}

void loop() {
  server.handleClient();
}
