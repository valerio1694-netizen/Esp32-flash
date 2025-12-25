#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <Preferences.h>

#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

/* ===================== AP / OTA ===================== */
static const char* AP_SSID = "ESP32-OTA";
static const char* AP_PASS = "12345678";
WebServer server(80);

/* ===================== Pins ===================== */
// TFT_eSPI Pins sind im User_Setup.h (Workflow patcht das)
static constexpr int TFT_BL = 32;

// Touch (XPT2046)
static constexpr int TOUCH_CS  = 27;
static constexpr int TOUCH_IRQ = 33;

// Shared SPI pins (VSPI default)
static constexpr int SPI_SCK  = 18;
static constexpr int SPI_MOSI = 23;
static constexpr int SPI_MISO = 19;

/* ===================== Objects ===================== */
TFT_eSPI tft;
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);
Preferences prefs;

/* ===================== Touch calibration data ===================== */
struct TouchCal {
  int16_t minX = 200;
  int16_t maxX = 3800;
  int16_t minY = 200;
  int16_t maxY = 3800;
  bool swapXY = false;
  bool invX   = false;
  bool invY   = false;
};
TouchCal cal;

enum class Mode : uint8_t { TFT_TEST = 0, TOUCH_CAL = 1, HMI = 2 };
Mode mode = Mode::HMI;

static uint32_t lastFrameMs = 0;
static uint32_t fpsCounterMs = 0;
static uint32_t fpsFrames = 0;
static uint16_t lastFps = 0;

/* ===================== Helpers ===================== */
static inline int clampi(int v, int lo, int hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

void blOn(bool on) {
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, on ? HIGH : LOW);
}

void saveCal() {
  prefs.begin("touch", false);
  prefs.putInt("minX", cal.minX);
  prefs.putInt("maxX", cal.maxX);
  prefs.putInt("minY", cal.minY);
  prefs.putInt("maxY", cal.maxY);
  prefs.putBool("swap", cal.swapXY);
  prefs.putBool("invX", cal.invX);
  prefs.putBool("invY", cal.invY);
  prefs.end();
}

void loadCal() {
  prefs.begin("touch", true);
  cal.minX  = prefs.getInt("minX", cal.minX);
  cal.maxX  = prefs.getInt("maxX", cal.maxX);
  cal.minY  = prefs.getInt("minY", cal.minY);
  cal.maxY  = prefs.getInt("maxY", cal.maxY);
  cal.swapXY= prefs.getBool("swap", cal.swapXY);
  cal.invX  = prefs.getBool("invX", cal.invX);
  cal.invY  = prefs.getBool("invY", cal.invY);
  prefs.end();
}

// Map raw touch (0..4095-ish) into display coords (0..W-1 / 0..H-1)
bool getTouchPixel(int &x, int &y, int &zRaw) {
  if (!ts.touched()) return false;

  TS_Point p = ts.getPoint();
  int rx = p.x;
  int ry = p.y;
  zRaw = p.z;

  // basic sanity
  if (rx <= 0 || ry <= 0) return false;

  int tx = rx;
  int ty = ry;

  if (cal.swapXY) {
    int tmp = tx; tx = ty; ty = tmp;
  }

  // map to 0..W-1 / 0..H-1
  int W = tft.width();
  int H = tft.height();

  int mx = map(tx, cal.minX, cal.maxX, 0, W - 1);
  int my = map(ty, cal.minY, cal.maxY, 0, H - 1);

  mx = clampi(mx, 0, W - 1);
  my = clampi(my, 0, H - 1);

  if (cal.invX) mx = (W - 1) - mx;
  if (cal.invY) my = (H - 1) - my;

  x = mx; y = my;
  return true;
}

/* ===================== Web UI ===================== */
String htmlHeader(const String& title) {
  String s;
  s += "<!doctype html><html><head><meta charset='utf-8'>";
  s += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  s += "<title>" + title + "</title>";
  s += "<style>";
  s += "body{font-family:system-ui,Segoe UI,Arial;margin:16px;max-width:820px}";
  s += "a,button,input{font-size:16px}";
  s += ".card{border:1px solid #ddd;border-radius:12px;padding:14px;margin:12px 0}";
  s += ".row{display:flex;gap:12px;flex-wrap:wrap}";
  s += ".row>div{flex:1;min-width:240px}";
  s += "code{background:#f4f4f4;padding:2px 6px;border-radius:6px}";
  s += "</style></head><body>";
  s += "<h2>" + title + "</h2>";
  return s;
}

String htmlFooter() {
  return "</body></html>";
}

void handleRoot() {
  String s = htmlHeader("ESP32 Panel (ST7796S) - Control");
  s += "<div class='card'><b>AP:</b> <code>" + String(AP_SSID) + "</code><br>";
  s += "<b>IP:</b> <code>" + WiFi.softAPIP().toString() + "</code><br>";
  s += "<b>OTA:</b> <a href='/update'>/update</a></div>";

  s += "<div class='card'><b>Mode</b><div class='row'>";
  s += "<div><a href='/mode?m=0'>TFT Test</a></div>";
  s += "<div><a href='/mode?m=1'>Touch Cal</a></div>";
  s += "<div><a href='/mode?m=2'>HMI Demo</a></div>";
  s += "</div></div>";

  s += "<div class='card'><b>Touch calibration</b><br>";
  s += "Current: ";
  s += "minX=" + String(cal.minX) + " maxX=" + String(cal.maxX) + " ";
  s += "minY=" + String(cal.minY) + " maxY=" + String(cal.maxY) + " ";
  s += "swap=" + String(cal.swapXY) + " invX=" + String(cal.invX) + " invY=" + String(cal.invY) + "<br><br>";
  s += "<a href='/cal/start'>Start on-screen calibration</a><br>";
  s += "<a href='/cal'>Manual calibration page</a>";
  s += "</div>";

  s += htmlFooter();
  server.send(200, "text/html", s);
}

void handleMode() {
  if (!server.hasArg("m")) { server.send(400, "text/plain", "missing m"); return; }
  int m = server.arg("m").toInt();
  if (m < 0 || m > 2) { server.send(400, "text/plain", "m must be 0..2"); return; }
  mode = (Mode)m;
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "OK");
}

void handleCalPage() {
  String s = htmlHeader("Touch calibration (manual)");
  s += "<div class='card'>";
  s += "<p>Set raw min/max and flags. Then <b>Save</b>.</p>";
  s += "<form action='/cal/set' method='get'>";
  s += "minX <input name='minX' value='"+String(cal.minX)+"' size='6'> ";
  s += "maxX <input name='maxX' value='"+String(cal.maxX)+"' size='6'><br><br>";
  s += "minY <input name='minY' value='"+String(cal.minY)+"' size='6'> ";
  s += "maxY <input name='maxY' value='"+String(cal.maxY)+"' size='6'><br><br>";
  s += "<label><input type='checkbox' name='swap' "+String(cal.swapXY?"checked":"")+"> swapXY</label><br>";
  s += "<label><input type='checkbox' name='invX' "+String(cal.invX?"checked":"")+"> invert X</label><br>";
  s += "<label><input type='checkbox' name='invY' "+String(cal.invY?"checked":"")+"> invert Y</label><br><br>";
  s += "<button type='submit'>Save</button>";
  s += "</form></div>";
  s += "<div class='card'><a href='/'>Back</a></div>";
  s += htmlFooter();
  server.send(200, "text/html", s);
}

void handleCalSet() {
  if (server.hasArg("minX")) cal.minX = server.arg("minX").toInt();
  if (server.hasArg("maxX")) cal.maxX = server.arg("maxX").toInt();
  if (server.hasArg("minY")) cal.minY = server.arg("minY").toInt();
  if (server.hasArg("maxY")) cal.maxY = server.arg("maxY").toInt();
  cal.swapXY = server.hasArg("swap");
  cal.invX   = server.hasArg("invX");
  cal.invY   = server.hasArg("invY");
  saveCal();
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "saved");
}

/* ---- OTA page ---- */
static const char* OTA_FORM =
  "<!doctype html><html><head><meta charset='utf-8'>"
  "<meta name='viewport' content='width=device-width,initial-scale=1'>"
  "<title>ESP32 OTA</title>"
  "<style>body{font-family:system-ui;margin:16px;max-width:720px}"
  ".card{border:1px solid #ddd;border-radius:12px;padding:14px;margin:12px 0}</style>"
  "</head><body><h2>ESP32 Web OTA</h2>"
  "<div class='card'>"
  "<form method='POST' action='/update' enctype='multipart/form-data'>"
  "<input type='file' name='update' accept='.bin' required><br><br>"
  "<button type='submit'>Upload</button>"
  "</form></div>"
  "<div class='card'><a href='/'>Back</a></div>"
  "</body></html>";

void handleUpdateGet() {
  server.send(200, "text/html", OTA_FORM);
}

void handleUpdatePost() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    Update.begin(UPDATE_SIZE_UNKNOWN);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    Update.write(upload.buf, upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END) {
    bool ok = Update.end(true);
    server.send(200, "text/plain", ok ? "OK. Rebooting..." : "FAIL");
    delay(300);
    ESP.restart();
  }
}

/* ===================== UI drawing ===================== */
void drawTopBar(const char* title) {
  tft.fillRect(0, 0, tft.width(), 34, TFT_DARKGREY);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
  tft.setTextFont(2);
  tft.setCursor(8, 10);
  tft.print(title);

  // mode buttons
  int y = 6, h = 22, w = 92, gap = 6;
  int x0 = tft.width() - (3*w + 2*gap) - 8;

  auto btn = [&](int x, const char* txt, bool active) {
    uint16_t bg = active ? TFT_GREEN : TFT_BLACK;
    uint16_t fg = active ? TFT_BLACK : TFT_WHITE;
    tft.fillRoundRect(x, y, w, h, 4, bg);
    tft.drawRoundRect(x, y, w, h, 4, TFT_WHITE);
    tft.setTextColor(fg, bg);
    tft.setTextFont(1);
    tft.setCursor(x + 10, y + 7);
    tft.print(txt);
  };

  btn(x0 + 0*(w+gap), "TFT",  mode==Mode::TFT_TEST);
  btn(x0 + 1*(w+gap), "CAL",  mode==Mode::TOUCH_CAL);
  btn(x0 + 2*(w+gap), "HMI",  mode==Mode::HMI);
}

bool topBarHit(int x, int y) {
  if (y < 6 || y > 28) return false;
  int w = 92, gap = 6;
  int x0 = tft.width() - (3*w + 2*gap) - 8;
  if (x < x0 || x > x0 + 3*w + 2*gap) return false;

  int idx = (x - x0) / (w + gap);
  if (idx < 0 || idx > 2) return false;

  mode = (Mode)idx;
  return true;
}

/* ===================== Mode 1: TFT Test ===================== */
void tftTestDrawOnce() {
  tft.fillScreen(TFT_BLACK);
  drawTopBar("ST7796S TFT TEST");

  // Gradient
  int y0 = 40;
  for (int y = y0; y < tft.height(); y++) {
    uint8_t r = map(y, y0, tft.height()-1, 10, 250);
    uint8_t g = 120;
    uint8_t b = map(y, y0, tft.height()-1, 250, 10);
    uint16_t c = tft.color565(r, g, b);
    tft.drawFastHLine(0, y, tft.width(), c);
  }

  // Color bars
  int bx = 10, by = 50, bw = tft.width()-20, bh = 80;
  int seg = bw/6;
  uint16_t cols[6] = {TFT_RED, TFT_GREEN, TFT_BLUE, TFT_CYAN, TFT_MAGENTA, TFT_YELLOW};
  for (int i=0;i<6;i++) tft.fillRect(bx+i*seg, by, seg, bh, cols[i]);

  // Text
  tft.setTextFont(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.fillRect(10, by+bh+10, tft.width()-20, 52, TFT_BLACK);
  tft.setCursor(14, by+bh+16);
  tft.printf("Res: %dx%d", tft.width(), tft.height());
  tft.setCursor(14, by+bh+34);
  tft.printf("SPI: 27MHz, BGR, ST7796S");
}

/* ===================== Mode 2: Touch Calibration ===================== */
enum class CalState : uint8_t { IDLE, TL, TR, BR, BL, DONE };
CalState calState = CalState::IDLE;
TS_Point pTL, pTR, pBR, pBL;

void calReset() {
  calState = CalState::TL;
  tft.fillScreen(TFT_BLACK);
  drawTopBar("TOUCH CALIBRATION");

  tft.setTextFont(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(12, 44);
  tft.println("Tap the targets in order:");
  tft.setCursor(12, 64);
  tft.println("TL -> TR -> BR -> BL");
}

void drawTarget(int x, int y) {
  tft.drawCircle(x, y, 16, TFT_WHITE);
  tft.drawCircle(x, y, 15, TFT_WHITE);
  tft.drawFastHLine(x-20, y, 40, TFT_WHITE);
  tft.drawFastVLine(x, y-20, 40, TFT_WHITE);
}

void calDrawTargets() {
  // clear area
  tft.fillRect(0, 90, tft.width(), tft.height()-90, TFT_BLACK);

  int margin = 30;
  int W = tft.width(), H = tft.height();
  if (calState == CalState::TL) drawTarget(margin, 110);
  if (calState == CalState::TR) drawTarget(W-margin, 110);
  if (calState == CalState::BR) drawTarget(W-margin, H-margin);
  if (calState == CalState::BL) drawTarget(margin, H-margin);

  tft.setTextFont(2);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(12, 92);
  const char* st = (calState==CalState::TL) ? "Tap TOP-LEFT"
                 : (calState==CalState::TR) ? "Tap TOP-RIGHT"
                 : (calState==CalState::BR) ? "Tap BOTTOM-RIGHT"
                 : (calState==CalState::BL) ? "Tap BOTTOM-LEFT"
                 : "Done";
  tft.println(st);
}

void calComputeAndSave() {
  // Raw points
  int tlx=pTL.x, tly=pTL.y;
  int trx=pTR.x, try_=pTR.y;
  int brx=pBR.x, bry=pBR.y;
  int blx=pBL.x, bly=pBL.y;

  // Decide swap: compare how x changes when moving left->right in raw
  // If (TL->TR) changes mainly in Y, then swapped.
  int dX_lr = abs(trx - tlx);
  int dY_lr = abs(try_ - tly);
  bool swap = (dY_lr > dX_lr);

  cal.swapXY = swap;

  auto getX = [&](int x, int y){ return swap ? y : x; };
  auto getY = [&](int x, int y){ return swap ? x : y; };

  int TLx = getX(tlx, tly), TLy = getY(tlx, tly);
  int TRx = getX(trx, try_), TRy = getY(trx, try_);
  int BRx = getX(brx, bry), BRy = getY(brx, bry);
  int BLx = getX(blx, bly), BLy = getY(blx, bly);

  int minX = min(TLx, BLx);
  int maxX = max(TRx, BRx);
  int minY = min(TLy, TRy);
  int maxY = max(BLy, BRy);

  cal.minX = minX;
  cal.maxX = maxX;
  cal.minY = minY;
  cal.maxY = maxY;

  // Determine inversion by checking expected direction:
  // On screen, X increases from TL->TR
  cal.invX = (TRx < TLx);
  // On screen, Y increases from TL->BL
  cal.invY = (BLy < TLy);

  saveCal();

  tft.fillRect(0, 90, tft.width(), tft.height()-90, TFT_BLACK);
  tft.setTextFont(2);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setCursor(12, 110);
  tft.println("Saved calibration:");
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(12, 132);
  tft.printf("minX=%d maxX=%d\n", cal.minX, cal.maxX);
  tft.setCursor(12, 152);
  tft.printf("minY=%d maxY=%d\n", cal.minY, cal.maxY);
  tft.setCursor(12, 172);
  tft.printf("swap=%d invX=%d invY=%d\n", cal.swapXY, cal.invX, cal.invY);
  tft.setCursor(12, 200);
  tft.println("Test by touching: dots must follow finger.");
}

/* ===================== Mode 3: HMI Demo ===================== */
struct Button {
  int x,y,w,h;
  const char* label;
  bool state=false;
};

Button b1{20, 60, 140, 70, "RUN", false};
Button b2{180,60, 140, 70, "STOP", false};
Button b3{20, 150, 300, 70, "FAULT ACK", false};

int sliderVal = 50; // 0..100

bool hitBtn(const Button& b, int px, int py) {
  return (px>=b.x && px<b.x+b.w && py>=b.y && py<b.y+b.h);
}

void drawBtn(const Button& b) {
  uint16_t bg = b.state ? TFT_GREEN : TFT_BLACK;
  uint16_t fg = b.state ? TFT_BLACK : TFT_WHITE;
  tft.fillRoundRect(b.x, b.y, b.w, b.h, 10, bg);
  tft.drawRoundRect(b.x, b.y, b.w, b.h, 10, TFT_WHITE);
  tft.setTextFont(2);
  tft.setTextColor(fg, bg);
  int tx = b.x + 16;
  int ty = b.y + (b.h/2) - 8;
  tft.setCursor(tx, ty);
  tft.print(b.label);
}

void drawSlider() {
  int x=20, y=245, w=300, h=20;
  tft.drawRoundRect(x, y, w, h, 8, TFT_WHITE);
  int fillW = map(sliderVal, 0, 100, 0, w-4);
  tft.fillRoundRect(x+2, y+2, fillW, h-4, 6, TFT_CYAN);
  tft.fillRect(x+2+fillW, y+2, (w-4)-fillW, h-4, TFT_BLACK);

  tft.setTextFont(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(20, 272);
  tft.printf("SETPOINT: %d%%", sliderVal);
}

void hmiDrawOnce() {
  tft.fillScreen(TFT_BLACK);
  drawTopBar("HMI DEMO");
  drawBtn(b1); drawBtn(b2); drawBtn(b3);
  drawSlider();

  // status footer
  tft.fillRect(0, tft.height()-40, tft.width(), 40, TFT_DARKGREY);
  tft.setTextFont(2);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
  tft.setCursor(10, tft.height()-30);
  tft.printf("OTA: http://%s/update", WiFi.softAPIP().toString().c_str());
}

void hmiUpdateStatus() {
  tft.fillRect(0, tft.height()-40, tft.width(), 40, TFT_DARKGREY);
  tft.setTextFont(2);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
  tft.setCursor(10, tft.height()-30);

  const char* st = b2.state ? "STOPPED" : (b1.state ? "RUNNING" : "IDLE");
  if (b3.state) st = "FAULT ACKED";
  tft.printf("STATUS: %s | OTA: /update", st);
}

/* ===================== Setup ===================== */
void setupWeb() {
  server.on("/", handleRoot);
  server.on("/mode", handleMode);

  server.on("/cal", handleCalPage);
  server.on("/cal/set", handleCalSet);

  server.on("/cal/start", [](){
    mode = Mode::TOUCH_CAL;
    calReset();
    calDrawTargets();
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "ok");
  });

  server.on("/update", HTTP_GET, handleUpdateGet);
  server.on("/update", HTTP_POST, [](){ /* handled in upload */ }, handleUpdatePost);

  server.begin();
}

void setup() {
  Serial.begin(115200);

  // Backlight on
  blOn(true);

  // SPI init
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);

  // TFT init
  tft.init();
  tft.setRotation(1); // Landscape: 480x320
  tft.fillScreen(TFT_BLACK);

  // Touch init
  ts.begin();
  // Touch controller often needs lower clock; lib uses SPISettings internally

  // Load saved calibration
  loadCal();

  // Start AP
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  delay(100);

  setupWeb();

  // Default screen
  mode = Mode::HMI;
  hmiDrawOnce();

  fpsCounterMs = millis();
  lastFrameMs = millis();
}

/* ===================== Loop ===================== */
void loop() {
  server.handleClient();

  // Touch handling
  int x,y,z;
  if (getTouchPixel(x,y,z)) {
    // handle topbar buttons first
    if (topBarHit(x,y)) {
      if (mode == Mode::TFT_TEST) tftTestDrawOnce();
      if (mode == Mode::TOUCH_CAL) { calReset(); calDrawTargets(); }
      if (mode == Mode::HMI) hmiDrawOnce();
      delay(180);
      return;
    }

    if (mode == Mode::TFT_TEST) {
      // draw dots following finger
      tft.fillCircle(x, y, 6, TFT_WHITE);
      delay(10);
    }

    if (mode == Mode::TOUCH_CAL) {
      // Collect raw points in sequence using raw readings (not mapped)
      TS_Point p = ts.getPoint();

      auto acceptPoint = [&](TS_Point &dst){
        dst = p;
        tft.fillCircle(x, y, 6, TFT_GREEN);
        delay(250);
      };

      if (calState == CalState::IDLE) {
        calReset();
        calDrawTargets();
      } else if (calState == CalState::TL) {
        acceptPoint(pTL);
        calState = CalState::TR;
        calDrawTargets();
      } else if (calState == CalState::TR) {
        acceptPoint(pTR);
        calState = CalState::BR;
        calDrawTargets();
      } else if (calState == CalState::BR) {
        acceptPoint(pBR);
        calState = CalState::BL;
        calDrawTargets();
      } else if (calState == CalState::BL) {
        acceptPoint(pBL);
        calState = CalState::DONE;
        calComputeAndSave();
      } else {
        // DONE: show live dots for verification
        tft.fillCircle(x, y, 5, TFT_CYAN);
        delay(10);
      }
    }

    if (mode == Mode::HMI) {
      bool changed=false;

      if (hitBtn(b1, x, y)) { b1.state = true; b2.state = false; b3.state=false; changed=true; }
      else if (hitBtn(b2, x, y)) { b2.state = true; b1.state = false; b3.state=false; changed=true; }
      else if (hitBtn(b3, x, y)) { b3.state = !b3.state; changed=true; }

      // slider area
      int sx=20, sy=245, sw=300, sh=20;
      if (x>=sx && x<sx+sw && y>=sy && y<sy+sh) {
        sliderVal = map(x, sx, sx+sw-1, 0, 100);
        sliderVal = clampi(sliderVal, 0, 100);
        changed=true;
      }

      if (changed) {
        drawBtn(b1); drawBtn(b2); drawBtn(b3);
        drawSlider();
        hmiUpdateStatus();
        delay(120);
      } else {
        // draw touch dots faint
        tft.fillCircle(x, y, 3, TFT_YELLOW);
      }
    }
  }

  // Lightweight FPS counter (just for sanity)
  fpsFrames++;
  uint32_t now = millis();
  if (now - fpsCounterMs >= 1000) {
    lastFps = fpsFrames;
    fpsFrames = 0;
    fpsCounterMs = now;
  }
}
