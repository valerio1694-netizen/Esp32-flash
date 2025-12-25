#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <TFT_eSPI.h>

#define TFT_BL 32   // Backlight

TFT_eSPI tft = TFT_eSPI();
WebServer server(80);

// -------- OTA WEB UI ----------
const char* AP_SSID = "ESP32-OTA";
const char* AP_PASS = "12345678";

String page() {
  return R"rawliteral(
<!DOCTYPE html><html>
<body>
<h2>ESP32 OTA</h2>
<form method="POST" action="/update" enctype="multipart/form-data">
<input type="file" name="update">
<input type="submit" value="Update">
</form>
</body></html>
)rawliteral";
}

void setup() {
  Serial.begin(115200);

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  // --- WiFi AP ---
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  delay(200);

  // --- TFT ---
  tft.init();
  tft.setRotation(3);     // Querformat 480x320
  tft.fillScreen(TFT_BLACK);

  // Farbtest (GANZE FLÄCHE!)
  tft.fillRect(0,   0, 96, 320, TFT_RED);
  tft.fillRect(96,  0, 96, 320, TFT_GREEN);
  tft.fillRect(192, 0, 96, 320, TFT_BLUE);
  tft.fillRect(288, 0, 96, 320, TFT_YELLOW);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 10);
  tft.println("ST7796 OK");

  // --- OTA ---
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", page());
  });

  server.on("/update", HTTP_POST,
    []() {
      server.send(200, "text/plain", Update.hasError() ? "FAIL" : "OK");
      ESP.restart();
    },
    []() {
      HTTPUpload& upload = server.upload();
      if (upload.status == UPLOAD_FILE_START)
        Update.begin();
      else if (upload.status == UPLOAD_FILE_WRITE)
        Update.write(upload.buf, upload.currentSize);
      else if (upload.status == UPLOAD_FILE_END)
        Update.end(true);
    }
  );

  server.begin();
}

void loop() {
  server.handleClient();
}
