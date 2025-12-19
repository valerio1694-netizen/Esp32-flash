#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <SPI.h>
#include <Preferences.h>

#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

/* ================= AP / OTA ================= */
const char* AP_SSID = "ESP32-OTA";
const char* AP_PASS = "12345678";
WebServer server(80);

/* ================= PINS ================= */
static constexpr int TFT_BL    = 32;
static constexpr int TOUCH_CS  = 27;
static constexpr int TOUCH_IRQ = 33;

/* ================= HW ================= */
TFT_eSPI tft;
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);
Preferences prefs;

/* ================= TOUCH CAL ================= */
struct Cal {
  uint16_t minX, maxX, minY, maxY;
  bool swapXY, invX, invY, valid;
} cal;

/* ================= UI ================= */
enum Mode { DEMO, PAINT, INFO };
Mode mode = DEMO;

/* ================= HELPERS ================= */
int mapc(int v, int inMin, int inMax, int outMin, int outMax) {
  v = constrain(v, inMin, inMax);
  return map(v, inMin, inMax, outMin, outMax);
}

/* ================= TOUCH ================= */
bool readTouch(uint16_t &sx, uint16_t &sy) {
  if (digitalRead(TOUCH_IRQ)) return false;
  if (!ts.touched()) return false;

  TS_Point p = ts.getPoint();
  uint16_t x = p.x;
  uint16_t y = p.y;

  if (!cal.valid) return false;

  if (cal.swapXY) std::swap(x, y);
  sx = mapc(x, cal.minX, cal.maxX, 0, 319);
  sy = mapc(y, cal.minY, cal.maxY, 0, 479);
  if (cal.invX) sx = 319 - sx;
  if (cal.invY) sy = 479 - sy;
  return true;
}

/* ================= DRAW ================= */
void drawUI() {
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0,0,320,40,TFT_DARKGREY);
  tft.setTextColor(TFT_WHITE,TFT_DARKGREY);
  tft.setTextSize(2);
  tft.setCursor(10,10);
  tft.print("ESP32 TFT + TOUCH");

  tft.drawRect(10,45,90,30, mode==DEMO?TFT_GREEN:TFT_WHITE);
  tft.drawRect(115,45,90,30, mode==PAINT?TFT_GREEN:TFT_WHITE);
  tft.drawRect(220,45,90,30, mode==INFO?TFT_GREEN:TFT_WHITE);

  tft.setCursor(30,52);  tft.print("DEMO");
  tft.setCursor(135,52); tft.print("PAINT");
  tft.setCursor(245,52); tft.print("INFO");
}

/* ================= OTA ================= */
void setupOTA() {
  server.on("/update", HTTP_GET, [](){
    server.send(200,"text/html",
      "<form method
