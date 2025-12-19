#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>

#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

/* ================= WIFI / OTA ================= */
const char* AP_SSID = "ESP32-OTA";
const char* AP_PASS = "12345678";
WebServer server(80);

/* ================= PINS ================= */
#define TFT_BL     32
#define TOUCH_CS  27
#define TOUCH_IRQ 33

/* ================= HW ================= */
TFT_eSPI tft;
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);

/* ================= TOUCH RAW MAP ================= */
int RAW_MIN_X = 200;
int RAW_MAX_X = 3800;
int RAW_MIN_Y = 200;
int RAW_MAX_Y = 3800;

/* ================= UI ================= */
enum Mode { DEMO, PAINT, INFO };
Mode mode = DEMO;

/* ================= OTA ================= */
void setupOTA() {
  server.on("/update", HTTP_GET, []() {
    server.send(200, "text/html",
      "<form method='POST' action='/update' enctype='multipart/form-data'>"
      "<input type='file' name='update'>"
      "<input type='submit'></form>");
  });

  server.on("/update", HTTP_POST,
    []() { server.send(200); ESP.restart(); },
    []() {
      HTTPUpload& u = server.upload();
      if (u.status == UPLOAD_FILE_START) Update.begin();
      else if (u.status == UPLOAD_FILE_WRITE) Update.write(u.buf, u.currentSize);
      else if (u.status == UPLOAD_FILE_END) Update.end(true);
    }
  );

  server.begin();
}

/* ================= TOUCH ================= */
bool readTouch(int &x, int &y) {
  if (digitalRead(TOUCH_IRQ)) return false;
  if (!ts.touched()) return false;

  TS_Point p = ts.getPoint();

  x = map(p.y, RAW_MIN_Y, RAW_MAX_Y, 0, tft.width());
  y = map(p.x, RAW_MIN_X, RAW_MAX_X, 0, tft.height());

  return true;
}

/* ================= DRAW ================= */
void drawUI() {
  tft.fillScreen(TFT_BLACK);

  tft.fillRect(0, 0, tft.width(), 40, TFT_DARKGREY);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
  tft.setTextSize(2);
  tft.setCursor(10, 10);
  tft.print("ESP32 TFT + OTA");

  int w = tft.width() / 3;
  tft.drawRect(0, 45, w, 30, mode == DEMO ? TFT_GREEN : TFT_WHITE);
  tft.drawRect(w, 45, w, 30, mode == PAINT ? TFT_GREEN : TFT_WHITE);
  tft.drawRect(2 * w, 45, w, 30, mode == INFO ? TFT_GREEN : TFT_WHITE);

  tft.setCursor(20, 52); tft.print("DEMO");
  tft.setCursor(w + 20, 52); tft.print("PAINT");
  tft.setCursor(2 * w + 20, 52); tft.print("INFO");
}

void drawDemo() {
  for (int y = 90; y < tft.height(); y += 6) {
    uint16_t c = tft.color565(y * 2, y * 3, y * 4);
    tft.fillRect(0, y, tft.width(), 6, c);
  }
}

void drawInfo() {
  tft.fillRect(0, 90, tft.width(), tft.height() - 90, TFT_BLACK);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(2);
  tft.setCursor(10, 100);
  tft.print("IP: ");
  tft.println(WiFi.softAPIP());
  tft.setCursor(10, 130);
  tft.print("/update");
}

/* ================= SETUP ================= */
void setup() {
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  pinMode(TOUCH_IRQ, INPUT_PULLUP);

  tft.init();
  tft.setRotation(1);

  ts.begin();

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  setupOTA();

  drawUI();
  drawDemo();
}

/* ================= LOOP ================= */
void loop() {
  server.handleClient();

  int x, y;
  if (!readTouch(x, y)) return;

  if (y < 80) {
    int w = tft.width() / 3;
    if (x < w) mode = DEMO;
    else if (x < 2 * w) mode = PAINT;
    else mode = INFO;

    drawUI();
    if (mode == DEMO) drawDemo();
    if (mode == INFO) drawInfo();
    delay(250);
    return;
  }

  if (mode == PAINT) {
    tft.fillCircle(x, y, 5, TFT_RED);
  }
}
