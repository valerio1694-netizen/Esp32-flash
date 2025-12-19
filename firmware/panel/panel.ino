#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <SPI.h>
#include <Preferences.h>

#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

// ===================== AP / OTA =====================
const char* AP_SSID = "ESP32-OTA";
const char* AP_PASS = "12345678";   // >=8 Zeichen oder "" offen
WebServer server(80);

// ===================== Pins =====================
static constexpr int TFT_BL   = 32;   // Backlight (manuell)
static constexpr int TOUCH_CS  = 27;
static constexpr int TOUCH_IRQ = 33;  // du hast IRQ an GPIO33

// SPI (shared VSPI)
static constexpr int SPI_SCK  = 18;
static constexpr int SPI_MISO = 19;
static constexpr int SPI_MOSI = 23;

// ===================== Display / Touch =====================
TFT_eSPI tft;
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);
SPIClass& spi = SPI;

// ===================== Persistent Calibration =====================
Preferences prefs;

struct CalData {
  uint16_t minX=200, maxX=3800, minY=200, maxY=3800;
  bool swapXY=true;
  bool invX=false;
  bool invY=false;
  bool valid=false;
};
CalData cal;

// ===================== UI State =====================
enum Mode : uint8_t { MODE_DEMO=0, MODE_PAINT=1, MODE_INFO=2 };
Mode mode = MODE_DEMO;

bool calRequestFromWeb = false;
bool calibrating = false;

// ===================== Helpers =====================
static inline int clampi(int v, int lo, int hi) { return (v<lo)?lo:(v>hi)?hi:v; }

static inline uint16_t mapU16(int v, int inMin, int inMax, int outMin, int outMax) {
  v = clampi(v, inMin, inMax);
  long num = (long)(v - inMin) * (outMax - outMin);
  long den = (inMax - inMin);
  return (uint16_t)(outMin + num / den);
}

void loadCal() {
  prefs.begin("touchcal", true);
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
}

void saveCal() {
  prefs.begin("touchcal", false);
  prefs.putBool("valid", true);
  prefs.putUShort("minX", cal.minX);
  prefs.putUShort("maxX", cal.maxX);
  prefs.putUShort("minY", cal.minY);
  prefs.putUShort("maxY", cal.maxY);
  prefs.putBool("swap", cal.swapXY);
  prefs.putBool("invX", cal.invX);
  prefs.putBool("invY", cal.invY);
  prefs.end();
  cal.valid = true;
}

void resetCal() {
  prefs.begin("touchcal", false);
  prefs.clear();
  prefs.end();
  cal = CalData{};
  cal.valid = false;
}

// ===================== Web OTA + Cal Menu =====================
String calJson() {
  String s="{";
  s += "\"valid\":" + String(cal.valid?"true":"false");
  s += ",\"swapXY\":" + String(cal.swapXY?"true":"false");
  s += ",\"invX\":" + String(cal.invX?"true":"false");
  s += ",\"invY\":" + String(cal.invY?"true":"false");
  s += ",\"minX\":" + String(cal.minX);
  s += ",\"maxX\":" + String(cal.maxX);
  s += ",\"minY\":" + String(cal.minY);
  s += ",\"maxY\":" + String(cal.maxY);
  s += ",\"calibrating\":" + String(calibrating?"true":"false");
  s += "}";
  return s;
}

static const char CAL_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Touch Calibration</title>
<style>
body{font-family:sans-serif;max-width:720px;margin:16px;}
button{padding:12px 16px;margin:6px 0;font-size:16px;}
pre{background:#111;color:#0f0;padding:10px;overflow:auto;}
</style>
</head><body>
<h2>Touch Calibration</h2>
<p>Kalibrierung passiert am TFT. Hier starten und Werte ansehen.</p>
<p>
<button onclick="fetch('/api/start_cal').then(r=>r.text()).then(alert)">Start Calibration (TFT)</button><br>
<button onclick="fetch('/api/reset_cal').then(r=>r.text()).then(alert)">Reset Calibration</button>
</p>
<h3>Status</h3>
<pre id="st">loading...</pre>
<p><a href="/update">OTA Update</a></p>
<script>
async function refresh(){
  const t = await fetch('/api/cal').then(r=>r.text());
  document.getElementById('st').textContent = t;
}
setInterval(refresh, 1000); refresh();
</script>
</body></html>
)HTML";

void setupWeb() {
  server.on("/", HTTP_GET, [](){
    server.send(200, "text/plain",
      "OK. Open /update for OTA or /cal for calibration menu.");
  });

  // OTA page
  server.on("/update", HTTP_GET, [](){
    server.send(200, "text/html",
      "<!doctype html><html><body><h2>OTA Update</h2>"
      "<form method='POST' action='/update' enctype='multipart/form-data'>"
      "<input type='file' name='update'><input type='submit' value='Upload'>"
      "</form></body></html>"
    );
  });

  // OTA upload
  server.on("/update", HTTP_POST,
    [](){
      server.send(200, "text/plain", Update.hasError() ? "FAIL" : "OK. Reboot...");
      delay(250);
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

  // Calibration menu
  server.on("/cal", HTTP_GET, [](){ server.send_P(200, "text/html", CAL_HTML); });
  server.on("/api/cal", HTTP_GET, [](){ server.send(200, "application/json", calJson()); });
  server.on("/api/start_cal", HTTP_GET, [](){
    calRequestFromWeb = true;
    server.send(200, "text/plain", "OK: Calibration will start on TFT.");
  });
  server.on("/api/reset_cal", HTTP_GET, [](){
    resetCal();
    server.send(200, "text/plain", "Calibration reset.");
  });

  server.begin();
}

// ===================== Touch read (IRQ) =====================
bool touchRaw(uint16_t &rx, uint16_t &ry) {
  // IRQ active-low; HIGH = nix gedrückt
  if (digitalRead(TOUCH_IRQ) == HIGH) return false;
  if (!ts.touched()) return false;
  TS_Point p = ts.getPoint(); // raw
  rx = (uint16_t)p.x;
  ry = (uint16_t)p.y;
  return true;
}

bool mapToScreen(uint16_t rx, uint16_t ry, uint16_t &sx, uint16_t &sy) {
  if (!cal.valid) return false;

  uint16_t ax = cal.swapXY ? ry : rx;
  uint16_t ay = cal.swapXY ? rx : ry;

  uint16_t x = mapU16(ax, cal.minX, cal.maxX, 0, 319);
  uint16_t y = mapU16(ay, cal.minY, cal.maxY, 0, 479);

  if (cal.invX) x = 319 - x;
  if (cal.invY) y = 479 - y;

  sx = x; sy = y;
  return true;
}

// ===================== UI Drawing =====================
void drawStatus(const String& s) {
  tft.fillRect(0, 430, 320, 50, TFT_BLACK);
  tft.drawFastHLine(0, 430, 320, TFT_DARKGREY);
  tft.setTextSize(2);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setCursor(10, 445);
  tft.print(s);
}

void drawTopBar() {
  tft.fillRect(0, 0, 320, 60, TFT_DARKGREY);
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
  tft.setCursor(10, 10);
  tft.print("ESP32 TFT + OTA");

  // Tabs
  tft.setCursor(10, 35);  tft.print("DEMO");
  tft.setCursor(120,35);  tft.print("PAINT");
  tft.setCursor(240,35);  tft.print("INFO");

  int x = (mode==MODE_DEMO)?10 : (mode==MODE_PAINT)?120 : 240;
  tft.drawRect(x-4, 32, 80, 24, TFT_GREEN);
}

void renderScreen() {
  tft.fillScreen(TFT_BLACK);
  drawTopBar();

  tft.fillRect(0, 60, 320, 370, TFT_BLACK);

  if (mode == MODE_DEMO) {
    // „sieht nach was aus“
    for (int y = 60; y < 360; y += 8) {
      uint16_t c = tft.color565((y*2)&255, (y*3)&255, (y*5)&255);
      tft.fillRect(0, y, 320, 8, c);
    }
    tft.drawRoundRect(10, 80, 140, 70, 12, TFT_BLACK);
    tft.fillCircle(250, 140, 30, TFT_WHITE);
    tft.drawTriangle(200, 290, 300, 290, 250, 220, TFT_BLACK);
    drawStatus(String("DEMO  |  Touch: ") + (cal.valid ? "OK" : "NOT CAL"));

  } else if (mode == MODE_PAINT) {
    tft.setTextSize(2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(10, 70);
    tft.print("Paint (wipe)");
    tft.drawRect(5, 110, 310, 310, TFT_DARKGREY);
    drawStatus("PAINT");

  } else {
    tft.setTextSize(2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(10, 70);
    tft.print("INFO");
    tft.setCursor(10, 100);
    tft.print("AP: "); tft.println(AP_SSID);
    tft.setCursor(10, 130);
    tft.print("IP: "); tft.println(WiFi.softAPIP());
    tft.setCursor(10, 160);
    tft.print("OTA: /update");
    tft.setCursor(10, 190);
    tft.print("CAL: /cal");
    drawStatus(String("Touch: ") + (cal.valid ? "CAL OK" : "NOT CAL"));
  }
}

// ===================== Calibration on TFT =====================
void drawCross(int x, int y, uint16_t col) {
  tft.drawLine(x-12,y, x+12,y, col);
  tft.drawLine(x,y-12, x,y+12, col);
  tft.drawCircle(x,y, 14, col);
}

bool waitStableTouch(uint16_t &rx, uint16_t &ry) {
  uint32_t t0 = millis();
  while (millis() - t0 < 15000) {
    uint16_t a,b;
    if (touchRaw(a,b)) {
      delay(30);
      uint16_t a2,b2; if (!touchRaw(a2,b2)) continue;
      delay(30);
      uint16_t a3,b3; if (!touchRaw(a3,b3)) continue;

      auto d=[](uint16_t p,uint16_t q){ return (p>q)?(p-q):(q-p); };
      if (d(a,a2)<80 && d(a2,a3)<80 && d(b,b2)<80 && d(b2,b3)<80) {
        rx=(a+a2+a3)/3; ry=(b+b2+b3)/3;

        // warten bis losgelassen
        uint32_t rel=millis();
        while (millis() - rel < 800) {
          uint16_t tx,ty;
          if (!touchRaw(tx,ty)) break;
          delay(20);
        }
        return true;
      }
    }
    delay(10);
  }
  return false;
}

void doCalibration() {
  calibrating = true;

  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(10, 10);
  tft.println("TOUCH CALIBRATION");
  tft.setCursor(10, 40);
  tft.println("Tippe 4 Ecken");

  struct P { uint16_t rx, ry; } TL{}, TR{}, BR{}, BL{};

  auto ask = [&](const char* label, int sx, int sy, P &out)->bool{
    tft.fillRect(0, 80, 320, 60, TFT_BLACK);
    tft.setCursor(10, 80);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.print("Tippe: "); tft.print(label);

    tft.fillRect(0, 150, 320, 330, TFT_BLACK);
    drawCross(sx, sy, TFT_GREEN);

    uint16_t rx, ry;
    if (!waitStableTouch(rx, ry)) return false;
    out.rx=rx; out.ry=ry;

    tft.setCursor(10, 110);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.printf("RAW x=%u y=%u   ", rx, ry);
    return true;
  };

  // Zielpunkte (unter Topbar)
  const int TLx=20,  TLy=80;
  const int TRx=300, TRy=80;
  const int BRx=300, BRy=460;
  const int BLx=20,  BLy=460;

  if (!ask("TOP-LEFT", TLx, TLy, TL)) { drawStatus("CAL FAIL"); calibrating=false; return; }
  if (!ask("TOP-RIGHT",TRx, TRy, TR)) { drawStatus("CAL FAIL"); calibrating=false; return; }
  if (!ask("BOTTOM-RIGHT",BRx, BRy, BR)) { drawStatus("CAL FAIL"); calibrating=false; return; }
  if (!ask("BOTTOM-LEFT",BLx, BLy, BL)) { drawStatus("CAL FAIL"); calibrating=false; return; }

  auto ad=[](uint16_t a,uint16_t b){ return (a>b)?(a-b):(b-a); };

  // Swap bestimmen (welche RAW-Achse reagiert auf X?)
  uint16_t dx_rx = ad(TR.rx, TL.rx);
  uint16_t dx_ry = ad(TR.ry, TL.ry);
  cal.swapXY = (dx_ry > dx_rx);

  auto rawX = [&](const P& p)->uint16_t { return cal.swapXY ? p.ry : p.rx; };
  auto rawY = [&](const P& p)->uint16_t { return cal.swapXY ? p.rx : p.ry; };

  // invert bestimmen
  cal.invX = !(rawX(TR) > rawX(TL));
  cal.invY = !(rawY(BL) > rawY(TL));

  // min/max
  uint16_t x1=rawX(TL), x2=rawX(TR), x3=rawX(BR), x4=rawX(BL);
  uint16_t y1=rawY(TL), y2=rawY(TR), y3=rawY(BR), y4=rawY(BL);

  cal.minX = min(min(x1,x2), min(x3,x4));
  cal.maxX = max(max(x1,x2), max(x3,x4));
  cal.minY = min(min(y1,y2), min(y3,y4));
  cal.maxY = max(max(y1,y2), max(y3,y4));

  // padding
  auto expand=[](uint16_t &mn, uint16_t &mx){
    uint16_t span = (mx>mn)?(mx-mn):0;
    uint16_t pad  = max<uint16_t>(30, span/25);
    mn = (mn>pad)?(mn-pad):0;
    mx = (mx+pad<4095)?(mx+pad):4095;
  };
  expand(cal.minX, cal.maxX);
  expand(cal.minY, cal.maxY);

  saveCal();

  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setCursor(10, 10);
  tft.println("CAL SAVED");
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(10, 50);
  tft.printf("swapXY=%d invX=%d invY=%d\n", cal.swapXY, cal.invX, cal.invY);
  tft.setCursor(10, 80);
  tft.printf("minX=%u maxX=%u\n", cal.minX, cal.maxX);
  tft.setCursor(10, 110);
  tft.printf("minY=%u maxY=%u\n", cal.minY, cal.maxY);

  delay(1200);
  calibrating = false;
  renderScreen();
}

// ===================== Interaction + Demo Anim =====================
uint32_t lastTouchMs = 0;
int bx=40, by=220, vx=3, vy=2;
uint32_t lastAnim=0;

void handleTouch() {
  if (calibrating) return;

  uint16_t rx, ry;
  if (!touchRaw(rx, ry)) return;

  uint32_t now = millis();
  if (now - lastTouchMs < 25) return;
  lastTouchMs = now;

  uint16_t sx, sy;
  bool ok = mapToScreen(rx, ry, sx, sy);

  // Tabs in topbar (y 30..60 ungefähr)
  if (ok && sy >= 30 && sy < 60) {
    if (sx < 106) mode = MODE_DEMO;
    else if (sx < 213) mode = MODE_PAINT;
    else mode = MODE_INFO;
    renderScreen();
    return;
  }

  if (!ok) {
    drawStatus("Touch NOT CAL -> /cal");
    return;
  }

  if (mode == MODE_PAINT) {
    // paint box
    if (sx >= 5 && sx <= 315 && sy >= 110 && sy <= 420) {
      tft.fillCircle(sx, sy, 6, TFT_RED);
      drawStatus("PAINT");
    }
    return;
  }

  if (mode == MODE_INFO) {
    drawStatus("RAW->XY OK");
    tft.fillCircle(sx, sy, 6, TFT_CYAN);
    return;
  }

  if (mode == MODE_DEMO) {
    tft.fillCircle(sx, sy, 6, TFT_BLACK); // “radierer” in demo
  }
}

void demoTick() {
  if (mode != MODE_DEMO) return;
  uint32_t now = millis();
  if (now - lastAnim < 16) return;
  lastAnim = now;

  // bounce ball in demo area (60..430)
  tft.fillCircle(bx, by, 10, TFT_WHITE); // old ball “hinterlässt” kleine spur -> sieht lebendig aus
  tft.fillCircle(bx, by, 9, TFT_BLACK);

  bx += vx; by += vy;
  if (bx < 15 || bx > 305) vx = -vx;
  if (by < 75 || by > 415) vy = -vy;

  tft.fillCircle(bx, by, 10, TFT_WHITE);
}

// ===================== Setup / Loop =====================
void setup() {
  Serial.begin(115200);

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  // Shared SPI
  spi.begin(SPI_SCK, SPI_MISO, SPI_MOSI);

  // Display
  tft.init();
  tft.setRotation(1);

  // Touch
  pinMode(TOUCH_IRQ, INPUT_PULLUP);
  ts.begin(spi);

  loadCal();

  // AP + Web
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  delay(100);
  setupWeb();

  renderScreen();
}

void loop() {
  server.handleClient();

  if (calRequestFromWeb) {
    calRequestFromWeb = false;
    doCalibration();
  }

  handleTouch();
  demoTick();
}
