#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

// ----------- WLAN AP -----------
#define AP_SSID "ESP32-OTA"
#define AP_PASS "12345678"

// ----------- TFT / TOUCH PINS -----------
#define TFT_BL 4
#define TOUCH_CS 15
#define TOUCH_IRQ 25

TFT_eSPI tft = TFT_eSPI();
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);
WebServer server(80);

// ----------- HTML OTA PAGE -----------
String page() {
  return R"rawliteral(
<!DOCTYPE html><html>
<body>
<h2>ESP32 OTA</h2>
<form method='POST' action='/update' enctype='multipart/form-data'>
<input type='file' name='update'>
<input type='submit' value='Update'>
</form>
</body>
</html>
)rawliteral";
}

void setup() {
  Serial.begin(115200);

  // Backlight
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  // WiFi AP
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  delay(200);

  // TFT init
  tft.init();
  tft.setRotation(1); // 480x320
  tft.fillScreen(TFT_BLACK);

  // Farbtest volle Fläche
  int w = tft.width() / 5;
  int h = tft.height();
  tft.fillRect(0*w, 0, w, h, TFT_RED);
  tft.fillRect(1*w, 0, w, h, TFT_GREEN);
  tft.fillRect(2*w, 0, w, h, TFT_BLUE);
  tft.fillRect(3*w, 0, w, h, TFT_YELLOW);
  tft.fillRect(4*w, 0, w, h, TFT_MAGENTA);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 10);
  tft.println("ST7796 OK");

  // Touch init
  ts.begin();
  ts.setRotation(1);

  // OTA
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", page());
  });

  server.on("/update", HTTP_POST,
    []() {
      server.sendHeader("Connection", "close");
      server.send(200, "text/plain", "OK");
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

void loop() {
  server.handleClient();

  if (ts.touched()) {
    TS_Point p = ts.getPoint();
    Serial.printf("Touch: x=%d y=%d\n", p.x, p.y);
  }
}
