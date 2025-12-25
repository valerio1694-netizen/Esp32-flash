#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

/* ========= WLAN AP ========= */
#define AP_SSID "ESP32-PANEL"
#define AP_PASS "12345678"

/* ========= TFT PINS ========= */
#define TFT_CS   5
#define TFT_DC   21
#define TFT_RST  22
#define TFT_BL   32

/* ========= TOUCH ========= */
#define TOUCH_CS   27
#define TOUCH_IRQ  33   // optional, kann -1 sein

/* ========= SPI ========= */
#define SPI_SCK   18
#define SPI_MOSI  23
#define SPI_MISO  19

WebServer server(80);
TFT_eSPI tft = TFT_eSPI();
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);

/* ========= HTML ========= */
String page() {
  return R"rawliteral(
<!DOCTYPE html>
<html>
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

/* ========= SETUP ========= */
void setup() {
  Serial.begin(115200);

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);   // Backlight AN

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  delay(200);

  // TFT
  tft.init();
  tft.setRotation(1); // 480x320 quer
  tft.fillScreen(TFT_BLACK);

  // Farbtest GANZE Fläche
  int w = tft.width() / 5;
  int h = tft.height();
  tft.fillRect(0*w, 0, w, h, TFT_MAGENTA);
  tft.fillRect(1*w, 0, w, h, TFT_YELLOW);
  tft.fillRect(2*w, 0, w, h, TFT_BLUE);
  tft.fillRect(3*w, 0, w, h, TFT_GREEN);
  tft.fillRect(4*w, 0, w, h, TFT_RED);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 10);
  tft.println("ST7796S OK");

  // Touch
  SPI.begin(SPI_SCK, SPI_MOSI, SPI_MISO);
  ts.begin();
  ts.setRotation(1);

  // OTA
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

/* ========= LOOP ========= */
void loop() {
  server.handleClient();

  if (ts.touched()) {
    TS_Point p = ts.getPoint();
    tft.fillCircle(p.x, p.y, 4, TFT_WHITE);
  }
}
