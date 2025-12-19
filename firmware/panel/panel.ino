#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <SPI.h>

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

/* ================= Touch mapping (erstmal simpel, später kalibrieren) =================
   Für “es reagiert und passt ungefähr” – echte Kalibrierung kommt als nächster Schritt.
*/
struct Cal {
  int minX=200, maxX=3800, minY=200, maxY=3800;
  bool swapXY=false;
  bool invX=false;
  bool invY=false;
  bool valid=true;
} cal;

/* ================= UI ================= */
enum Mode : uint8_t { DEMO=0, PAINT=1, INFO=2 };
Mode mode = DEMO;

/* ================= Helpers ================= */
static inline int clampi(int v, int lo, int hi){ return v<lo?lo:(v>hi?hi:v); }
static inline int mapi(int v, int inMin, int inMax, int outMin, int outMax){
  v = clampi(v, inMin, inMax);
  long num = (long)(v - inMin) * (outMax - outMin);
  long den = (inMax - inMin);
  return (int)(outMin + num / den);
}

bool getTouch(int &sx, int &sy) {
  if (digitalRead(TOUCH_IRQ) == HIGH) return false;
  if (!ts.touched()) return false;

  TS_Point p = ts.getPoint();
  int rx = p.x;
  int ry = p.y;

  if (cal.swapXY) { int tmp=rx; rx=ry; ry=tmp; }

  int W = tft.width();
  int H = tft.height();

  sx = mapi(rx, cal.minX, cal.maxX, 0, W-1);
  sy = mapi(ry, cal.minY, cal.maxY, 0, H-1);

  if (cal.invX) sx = (W-1) - sx;
  if (cal.invY) sy = (H-1) - sy;

  return true;
}

/* ================= OTA ================= */
void setupOTA() {
  server.on("/", HTTP_GET, [](){
    server.send(200, "text/plain", "OK. /update for OTA");
  });

  server.on("/update", HTTP_GET, [](){
    server.send(200,"text/html",
      "<!doctype html><html><body><h2>OTA Update</h2>"
      "<form method='POST' action='/update' enctype='multipart/form-data'>"
      "<input type='file' name='update'>"
      "<input type='submit' value='Upload'></form></body></html>");
  });

  server.on("/update", HTTP_POST,
    [](){
      server.send(200, "text/plain", Update.hasError() ? "FAIL" : "OK - reboot");
      delay(200);
      ESP.restart();
    },
    [](){
      HTTPUpload& u = server.upload();
      if (u.status == UPLOAD_FILE_START) Update.begin(UPDATE_SIZE_UNKNOWN);
      else if (u.status == UPLOAD_FILE_WRITE) Update.write(u.buf, u.currentSize);
      else if (u.status == UPLOAD_FILE_END) Update.end(true);
      else if (u.status == UPLOAD_FILE_ABORTED) Update.end();
    }
  );

  server.begin();
}

/* ================= UI drawing ================= */
void drawTabs() {
  int W = tft.width();
  // Top bar
  tft.fillRect(0, 0, W, 50, TFT_DARKGREY);
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
  tft.setCursor(10, 15);
  tft.print("ESP32 TFT + OTA");

  // Tabs area
  int tabY = 55, tabH = 35;
  int tabW = W / 3;

  for (int i=0;i<3;i++){
    int x = i*tabW;
    uint16_t col = (mode==i) ? TFT_GREEN : TFT_WHITE;
    tft.drawRect(x+5, tabY, tabW-10, tabH, col);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(x + 20, tabY + 10);
    if (i==0) tft.print("DEMO");
    if (i==1) tft.print("PAINT");
    if (i==2) tft.print("INFO");
  }
}

void drawDemo() {
  int W = tft.width();
  int H = tft.height();
  int top = 95;

  // Farbverlauf/Stripes über die komplette Breite (zeigt, dass 480 px genutzt werden)
  for (int y=top; y<H; y+=8) {
    uint16_t c = tft.color565((y*3)&255, (y*5)&255, (y*7)&255);
    tft.fillRect(0, y, W, 8, c);
  }

  tft.setTextSize(2);
  tft.setTextColor(TFT_BLACK);
  tft.setCursor(10, top+10);
  tft.print("Demo Fullscreen");
}

void drawPaint() {
  int W = tft.width();
  int H = tft.height();
  int top = 95;

  tft.fillRect(0, top, W, H-top, TFT_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE);
  tft.setCursor(10, top+10);
  tft.print("Paint: wischen");

  tft.drawRect(10, top+50, W-20, H-(top+60), TFT_DARKGREY);
}

void drawInfo() {
  int W = tft.width();
  int H = tft.height();
  int top = 95;

  tft.fillRect(0, top, W, H-top, TFT_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE);

  tft.setCursor(10, top+10);
  tft.print("AP: "); tft.println(AP_SSID);
  tft.setCursor(10, top+40);
  tft.print("IP: "); tft.println(WiFi.softAPIP());
  tft.setCursor(10, top+70);
  tft.print("OTA: /update");
}

void render() {
  tft.fillScreen(TFT_BLACK);
  drawTabs();
  if (mode == DEMO)  drawDemo();
  if (mode == PAINT) drawPaint();
  if (mode == INFO)  drawInfo();
}

void setup() {
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  pinMode(TOUCH_IRQ, INPUT_PULLUP);

  tft.init();
  // Landscape für 480x320
  tft.setRotation(1);

  ts.begin();

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  setupOTA();

  render();
}

void loop() {
  server.handleClient();

  int x,y;
  if (!getTouch(x,y)) return;

  int W = tft.width();
  int tabY = 55, tabH = 35;
  int tabW = W/3;

  // Tabs
  if (y >= tabY && y <= tabY+tabH) {
    int idx = x / tabW;
    if (idx < 0) idx = 0;
    if (idx > 2) idx = 2;
    mode = (Mode)idx;
    render();
    delay(150);
    return;
  }

  // Paint
  if (mode == PAINT) {
    int top = 95;
    int boxX = 10, boxY = top+50, boxW = W-20, boxH = tft.height()-(top+60);
    if (x >= boxX && x <= boxX+boxW && y >= boxY && y <= boxY+boxH) {
      tft.fillCircle(x, y, 6, TFT_RED);
    }
  }
}
