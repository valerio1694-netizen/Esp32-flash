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

/* ================= TOUCH CAL (Platzhalter, damit nix crasht) ================= */
struct Cal {
  uint16_t minX=200, maxX=3800, minY=200, maxY=3800;
  bool swapXY=true, invX=false, invY=false, valid=false;
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

  // solange keine Kalibrierung gespeichert ist, geben wir trotzdem RAW in Screen-Nähe aus
  if (!cal.valid) {
    // grob mappen (damit man wenigstens sieht, dass Touch reagiert)
    sx = mapc(x, 200, 3800, 0, 319);
    sy = mapc(y, 200, 3800, 0, 479);
    return true;
  }

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

  // Topbar
  tft.fillRect(0,0,320,40,TFT_DARKGREY);
  tft.setTextColor(TFT_WHITE,TFT_DARKGREY);
  tft.setTextSize(2);
  tft.setCursor(10,10);
  tft.print("ESP32 TFT + OTA");

  // Tabs
  tft.drawRect(10,45,90,30, mode==DEMO?TFT_GREEN:TFT_WHITE);
  tft.drawRect(115,45,90,30, mode==PAINT?TFT_GREEN:TFT_WHITE);
  tft.drawRect(220,45,90,30, mode==INFO?TFT_GREEN:TFT_WHITE);

  tft.setCursor(30,52);  tft.print("DEMO");
  tft.setCursor(135,52); tft.print("PAINT");
  tft.setCursor(245,52); tft.print("INFO");

  // Content
  if (mode == DEMO) {
    for (int y=80; y<420; y+=10) {
      tft.fillRect(0,y,320,10, tft.color565((y*2)&255, (y*3)&255, (y*5)&255));
    }
    tft.setTextColor(TFT_BLACK);
    tft.setCursor(10, 90);
    tft.print("Demo Screen");

  } else if (mode == PAINT) {
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(10, 90);
    tft.print("Paint: wischen");
    tft.drawRect(5,110,310,310, TFT_DARKGREY);

  } else {
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(10, 90);
    tft.print("AP: "); tft.println(AP_SSID);
    tft.setCursor(10, 120);
    tft.print("IP: "); tft.println(WiFi.softAPIP());
    tft.setCursor(10, 150);
    tft.print("OTA: /update");
  }
}

/* ================= OTA ================= */
void setupOTA() {
  server.on("/", HTTP_GET, [](){
    server.send(200, "text/plain", "OK. Open /update for OTA.");
  });

  server.on("/update", HTTP_GET, [](){
    server.send(200,"text/html",
      "<!doctype html><html><body><h2>OTA Update</h2>"
      "<form method='POST' action='/update' enctype='multipart/form-data'>"
      "<input type='file' name='update'>"
      "<input type='submit' value='Upload'></form></body></html>");
  });

  server.on("/update", HTTP_POST,
    [](){ server.send(200, "text/plain", Update.hasError() ? "FAIL" : "OK"); delay(200); ESP.restart(); },
    [](){
      HTTPUpload& u = server.upload();
      if(u.status == UPLOAD_FILE_START) Update.begin(UPDATE_SIZE_UNKNOWN);
      else if(u.status == UPLOAD_FILE_WRITE) Update.write(u.buf, u.currentSize);
      else if(u.status == UPLOAD_FILE_END) Update.end(true);
      else if(u.status == UPLOAD_FILE_ABORTED) Update.end();
    }
  );

  server.begin();
}

/* ================= SETUP ================= */
void setup() {
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  pinMode(TOUCH_IRQ, INPUT_PULLUP);

  tft.init();
  tft.setRotation(1);

  // Wichtig: KEIN eigenes SPI-Objekt. TFT_eSPI kümmert sich.
  ts.begin();

  // Cal laden (optional)
  prefs.begin("touch", true);
  cal.valid = prefs.getBool("valid", false);
  if (cal.valid) {
    cal.minX   = prefs.getUShort("minX", 200);
    cal.maxX   = prefs.getUShort("maxX", 3800);
    cal.minY   = prefs.getUShort("minY", 200);
    cal.maxY   = prefs.getUShort("maxY", 3800);
    cal.swapXY = prefs.getBool("swap", true);
    cal.invX   = prefs.getBool("invX", false);
    cal.invY   = prefs.getBool("invY", false);
  }
  prefs.end();

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);

  setupOTA();
  drawUI();
}

/* ================= LOOP ================= */
void loop() {
  server.handleClient();

  uint16_t x,y;
  if (readTouch(x,y)) {
    // Tabs
    if (y < 80) {
      if (x < 110) mode = DEMO;
      else if (x < 210) mode = PAINT;
      else mode = INFO;
      drawUI();
      delay(200);
      return;
    }

    // Paint
    if (mode == PAINT) {
      if (x >= 5 && x <= 315 && y >= 110 && y <= 420) {
        tft.fillCircle(x, y, 5, TFT_RED);
      }
    }
  }
}
