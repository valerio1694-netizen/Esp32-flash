#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <SPI.h>
#include <Preferences.h>
#include <LovyanGFX.hpp>

// ===================== AP / OTA =====================
const char* AP_SSID = "ESP32-OTA";
const char* AP_PASS = "12345678";   // >=8 Zeichen oder "" offen
WebServer server(80);

// ===================== Pins =====================
static constexpr int TFT_SCK  = 18;
static constexpr int TFT_MOSI = 23;
static constexpr int TFT_MISO = 19;
static constexpr int TFT_CS   = 5;
static constexpr int TFT_DC   = 21;
static constexpr int TFT_RST  = 22;
static constexpr int TFT_BL   = 32;

static constexpr int TOUCH_CS = 27;

// ===================== Forward declarations =====================
void setupWeb();
void handleTouchPaint();
void startCalibration();
bool mapTouchToScreen(uint16_t rx, uint16_t ry, uint16_t &sx, uint16_t &sy);

// ===================== TFT via LovyanGFX (Display only) =====================
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9488 _panel;
  lgfx::Bus_SPI _bus;
public:
  LGFX() {
    { // SPI
      auto cfg = _bus.config();
      cfg.spi_host   = VSPI_HOST;
      cfg.spi_mode   = 0;
      cfg.freq_write = 27000000;   // stabil
      cfg.freq_read  = 16000000;
      cfg.pin_sclk   = TFT_SCK;
      cfg.pin_mosi   = TFT_MOSI;
      cfg.pin_miso   = TFT_MISO;
      cfg.pin_dc     = TFT_DC;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    { // Panel
      auto cfg = _panel.config();
      cfg.pin_cs   = TFT_CS;
      cfg.pin_rst  = TFT_RST;
      cfg.pin_busy = -1;
      cfg.panel_width  = 320;
      cfg.panel_height = 480;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.invert = false;          // falls Farben komisch: true/false
      _panel.config(cfg);
    }
    setPanel(&_panel);
  }
};
LGFX lcd;

// ===================== XPT2046 RAW via SPI =====================
SPIClass vspi(VSPI);
SPISettings touchSPI(2000000, MSBFIRST, SPI_MODE0);

static constexpr uint8_t CMD_X = 0xD0; // X position
static constexpr uint8_t CMD_Y = 0x90; // Y position

uint16_t xpt2046_read12(uint8_t cmd) {
  vspi.beginTransaction(touchSPI);
  digitalWrite(TOUCH_CS, LOW);
  vspi.transfer(cmd);
  uint16_t v = vspi.transfer16(0x0000);
  digitalWrite(TOUCH_CS, HIGH);
  vspi.endTransaction();
  return (v >> 3) & 0x0FFF;
}

bool xpt2046_readXY(uint16_t &rx, uint16_t &ry) {
  // Dummy reads (manche Boards brauchen das)
  (void)xpt2046_read12(CMD_X);
  (void)xpt2046_read12(CMD_Y);

  uint16_t x1 = xpt2046_read12(CMD_X);
  uint16_t y1 = xpt2046_read12(CMD_Y);
  uint16_t x2 = xpt2046_read12(CMD_X);
  uint16_t y2 = xpt2046_read12(CMD_Y);

  // typische "tot" Werte:
  if ((x1 == 0 && y1 == 0) || (x1 == 4095 && y1 == 4095)) return false;
  if ((x2 == 0 && y2 == 0) || (x2 == 4095 && y2 == 4095)) return false;

  auto diff = [](uint16_t a, uint16_t b){ return (a > b) ? (a - b) : (b - a); };
  if (diff(x1, x2) > 250 || diff(y1, y2) > 250) return false;

  rx = (x1 + x2) / 2;
  ry = (y1 + y2) / 2;
  return true;
}

// ===================== Calibration data (persist) =====================
struct CalData {
  uint16_t rawMinX = 200;
  uint16_t rawMaxX = 3800;
  uint16_t rawMinY = 200;
  uint16_t rawMaxY = 3800;
  bool swapXY = true;
  bool invX = true;
  bool invY = false;
  bool valid = false;
};

Preferences prefs;
CalData cal;

// helpers
static inline uint16_t umin4(uint16_t a,uint16_t b,uint16_t c,uint16_t d){ return min(min(a,b),min(c,d)); }
static inline uint16_t umax4(uint16_t a,uint16_t b,uint16_t c,uint16_t d){ return max(max(a,b),max(c,d)); }

void loadCal() {
  prefs.begin("touchcal", true);
  cal.valid = prefs.getBool("valid", false);
  if (cal.valid) {
    cal.rawMinX = prefs.getUShort("minx", 200);
    cal.rawMaxX = prefs.getUShort("maxx", 3800);
    cal.rawMinY = prefs.getUShort("miny", 200);
    cal.rawMaxY = prefs.getUShort("maxy", 3800);
    cal.swapXY  = prefs.getBool("swap", true);
    cal.invX    = prefs.getBool("invx", true);
    cal.invY    = prefs.getBool("invy", false);
  }
  prefs.end();
}

void saveCal() {
  prefs.begin("touchcal", false);
  prefs.putBool("valid", true);
  prefs.putUShort("minx", cal.rawMinX);
  prefs.putUShort("maxx", cal.rawMaxX);
  prefs.putUShort("miny", cal.rawMinY);
  prefs.putUShort("maxy", cal.rawMaxY);
  prefs.putBool("swap",  cal.swapXY);
  prefs.putBool("invx",  cal.invX);
  prefs.putBool("invy",  cal.invY);
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

// Map raw -> screen (uses cal)
static inline int clampi(int v,int lo,int hi){ if(v<lo) return lo; if(v>hi) return hi; return v; }

static inline uint16_t mapU16(int v, int inMin, int inMax, int outMin, int outMax) {
  v = clampi(v, inMin, inMax);
  long num = (long)(v - inMin) * (outMax - outMin);
  long den = (inMax - inMin);
  return (uint16_t)(outMin + num / den);
}

bool mapTouchToScreen(uint16_t rx, uint16_t ry, uint16_t &sx, uint16_t &sy) {
  if (!cal.valid) return false;

  uint16_t ax = cal.swapXY ? ry : rx; // screen X raw source
  uint16_t ay = cal.swapXY ? rx : ry; // screen Y raw source

  // map
  uint16_t mx = mapU16(ax, cal.rawMinX, cal.rawMaxX, 0, 319);
  uint16_t my = mapU16(ay, cal.rawMinY, cal.rawMaxY, 0, 479);

  if (cal.invX) mx = 319 - mx;
  if (cal.invY) my = 479 - my;

  sx = mx;
  sy = my;
  return true;
}

// ===================== UI =====================
bool calibrating = false;

void drawHome() {
  lcd.fillScreen(TFT_BLACK);
  lcd.setTextSize(2);
  lcd.setTextColor(TFT_WHITE);
  lcd.setCursor(10, 10);
  lcd.println("ESP32 TFT + OTA + CAL");

  lcd.setTextColor(TFT_CYAN);
  lcd.setCursor(10, 40);
  lcd.print("OTA: http://192.168.4.1/update");

  lcd.setCursor(10, 70);
  lcd.print("CAL: http://192.168.4.1/cal");

  lcd.setTextColor(TFT_YELLOW);
  lcd.setCursor(10, 110);
  lcd.print("Touch: ");
  lcd.println(cal.valid ? "CAL OK" : "NOT CAL");

  lcd.setTextColor(TFT_GREEN);
  lcd.setCursor(10, 150);
  lcd.println("Tippe zum Malen (wenn CAL OK)");

  // kleine Legende
  lcd.setTextColor(TFT_DARKGREY);
  lcd.setCursor(10, 450);
  lcd.println("Web: /cal -> Start Calibration");
}

void drawCalParams() {
  lcd.fillRect(0, 180, 320, 240, TFT_BLACK);
  lcd.setTextColor(TFT_WHITE);
  lcd.setTextSize(2);
  lcd.setCursor(10, 180);
  lcd.printf("swapXY: %s\n", cal.swapXY ? "true" : "false");
  lcd.setCursor(10, 205);
  lcd.printf("invX:   %s\n", cal.invX ? "true" : "false");
  lcd.setCursor(10, 230);
  lcd.printf("invY:   %s\n", cal.invY ? "true" : "false");
  lcd.setCursor(10, 255);
  lcd.printf("minX:%u maxX:%u\n", cal.rawMinX, cal.rawMaxX);
  lcd.setCursor(10, 280);
  lcd.printf("minY:%u maxY:%u\n", cal.rawMinY, cal.rawMaxY);
}

bool waitForStableRaw(uint16_t &rx, uint16_t &ry) {
  uint32_t t0 = millis();
  while (millis() - t0 < 15000) { // 15s timeout
    uint16_t a,b;
    if (xpt2046_readXY(a,b)) {
      // einfache "Stabilität": gleiche Stelle 3x
      uint16_t a2,b2,a3,b3;
      delay(30);
      if (!xpt2046_readXY(a2,b2)) continue;
      delay(30);
      if (!xpt2046_readXY(a3,b3)) continue;

      auto d = [](uint16_t p, uint16_t q){ return (p>q)?(p-q):(q-p); };
      if (d(a,a2)<80 && d(a2,a3)<80 && d(b,b2)<80 && d(b2,b3)<80) {
        rx = (a+a2+a3)/3;
        ry = (b+b2+b3)/3;
        // warten bis losgelassen (damit nächster Punkt nicht sofort feuert)
        uint32_t rel = millis();
        while (millis() - rel < 800) {
          uint16_t tmpx,tmpy;
          if (!xpt2046_readXY(tmpx,tmpy)) break;
          delay(20);
        }
        return true;
      }
    }
    delay(10);
  }
  return false;
}

void startCalibration() {
  calibrating = true;

  lcd.fillScreen(TFT_BLACK);
  lcd.setTextSize(2);
  lcd.setTextColor(TFT_WHITE);
  lcd.setCursor(10, 10);
  lcd.println("CALIBRATION");
  lcd.setCursor(10, 40);
  lcd.println("Tippe 4 Punkte:");

  struct P { uint16_t rx, ry; } tl{}, tr{}, br{}, bl{};

  // Zielpunkte (Screen)
  auto ask = [&](const char* label, int sx, int sy, P &out){
    lcd.fillRect(0, 80, 320, 60, TFT_BLACK);
    lcd.setCursor(10, 80);
    lcd.setTextColor(TFT_YELLOW);
    lcd.printf("Tippe: %s", label);

    // Marker zeichnen
    lcd.fillRect(0, 140, 320, 340, TFT_BLACK);
    lcd.drawCircle(sx, sy, 12, TFT_GREEN);
    lcd.drawCircle(sx, sy, 2, TFT_GREEN);

    uint16_t rx, ry;
    if (!waitForStableRaw(rx, ry)) {
      lcd.fillRect(0, 420, 320, 60, TFT_BLACK);
      lcd.setCursor(10, 430);
      lcd.setTextColor(TFT_RED);
      lcd.println("Timeout / kein Touch");
      delay(1200);
      calibrating = false;
      drawHome();
      return;
    }
    out.rx = rx; out.ry = ry;

    lcd.setCursor(10, 110);
    lcd.setTextColor(TFT_CYAN);
    lcd.printf("RAW: x=%u y=%u   ", rx, ry);
  };

  ask("TOP-LEFT",     20,  70, tl);
  ask("TOP-RIGHT",   300,  70, tr);
  ask("BOTTOM-RIGHT",300, 460, br);
  ask("BOTTOM-LEFT",  20, 460, bl);

  // === Auto-Analyse: Swap + Invert + Min/Max ===
  auto ad = [](uint16_t a, uint16_t b){ return (a>b)?(a-b):(b-a); };

  // Welche RAW-Achse korreliert mit Screen-X? (TL->TR)
  uint16_t dx_rx = ad(tr.rx, tl.rx);
  uint16_t dx_ry = ad(tr.ry, tl.ry);
  cal.swapXY = (dx_ry > dx_rx); // wenn ry stärker auf X reagiert => swap

  // Wähle "rawX/rawY" gemäß swap
  auto rawX = [&](const P& p)->uint16_t { return cal.swapXY ? p.ry : p.rx; };
  auto rawY = [&](const P& p)->uint16_t { return cal.swapXY ? p.rx : p.ry; };

  // Invert X? (TL->TR muss rawX steigen, sonst invert)
  cal.invX = !(rawX(tr) > rawX(tl));

  // Invert Y? (TL->BL muss rawY steigen, sonst invert)
  cal.invY = !(rawY(bl) > rawY(tl));

  // Min/Max aus 4 Ecken
  uint16_t rxtl = rawX(tl), rxtr = rawX(tr), rxbr = rawX(br), rxbl = rawX(bl);
  uint16_t rytl = rawY(tl), rytr = rawY(tr), rybr = rawY(br), rybl = rawY(bl);

  cal.rawMinX = umin4(rxtl, rxtr, rxbr, rxbl);
  cal.rawMaxX = umax4(rxtl, rxtr, rxbr, rxbl);
  cal.rawMinY = umin4(rytl, rytr, rybr, rybl);
  cal.rawMaxY = umax4(rytl, rytr, rybr, rybl);

  // kleiner Sicherheitsrand (damit Rand erreichbar bleibt)
  auto expand = [](uint16_t &mn, uint16_t &mx){
    uint16_t span = (mx > mn) ? (mx - mn) : 0;
    uint16_t pad  = max<uint16_t>(30, span / 25); // ~4%
    mn = (mn > pad) ? (mn - pad) : 0;
    mx = (mx + pad < 4095) ? (mx + pad) : 4095;
  };
  expand(cal.rawMinX, cal.rawMaxX);
  expand(cal.rawMinY, cal.rawMaxY);

  saveCal();

  // Ergebnis anzeigen
  lcd.fillScreen(TFT_BLACK);
  lcd.setTextColor(TFT_GREEN);
  lcd.setTextSize(2);
  lcd.setCursor(10, 10);
  lcd.println("CAL SAVED");
  drawCalParams();

  lcd.setTextColor(TFT_YELLOW);
  lcd.setCursor(10, 420);
  lcd.println("Teste: Tippe/Malen");
  delay(1500);

  calibrating = false;
  drawHome();
  drawCalParams();
}

void handleTouchPaint() {
  if (calibrating) return;

  uint16_t rx, ry;
  if (!xpt2046_readXY(rx, ry)) return;

  uint16_t sx, sy;
  if (!mapTouchToScreen(rx, ry, sx, sy)) return;

  // Malen
  lcd.fillCircle(sx, sy, 6, TFT_RED);

  // Statuszeile
  lcd.fillRect(0, 430, 320, 50, TFT_BLACK);
  lcd.setTextColor(TFT_WHITE);
  lcd.setTextSize(2);
  lcd.setCursor(10, 440);
  lcd.printf("x=%u y=%u", sx, sy);
}

// ===================== Web UI =====================
String calJson() {
  String s = "{";
  s += "\"valid\":" + String(cal.valid ? "true" : "false");
  s += ",\"swapXY\":" + String(cal.swapXY ? "true" : "false");
  s += ",\"invX\":" + String(cal.invX ? "true" : "false");
  s += ",\"invY\":" + String(cal.invY ? "true" : "false");
  s += ",\"minX\":" + String(cal.rawMinX);
  s += ",\"maxX\":" + String(cal.rawMaxX);
  s += ",\"minY\":" + String(cal.rawMinY);
  s += ",\"maxY\":" + String(cal.rawMaxY);
  s += ",\"calibrating\":" + String(calibrating ? "true" : "false");
  s += "}";
  return s;
}

const char CAL_HTML[] PROGMEM = R"HTML(
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
<p>Kalibrierung passiert am TFT (nicht im Browser). Hier kannst du starten und Werte ansehen.</p>
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
  // root
  server.on("/", HTTP_GET, [](){
    server.send(200, "text/plain", "OK. Open /update for OTA or /cal for calibration menu.");
  });

  // OTA pages
  server.on("/update", HTTP_GET, [](){
    server.send(200, "text/html",
      "<!doctype html><html><body><h2>OTA Update</h2>"
      "<form method='POST' action='/update' enctype='multipart/form-data'>"
      "<input type='file' name='update'><input type='submit' value='Upload'>"
      "</form></body></html>"
    );
  });

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
  server.on("/cal", HTTP_GET, [](){
    server.send_P(200, "text/html", CAL_HTML);
  });

  server.on("/api/cal", HTTP_GET, [](){
    server.send(200, "application/json", calJson());
  });

  server.on("/api/start_cal", HTTP_GET, [](){
    if (!calibrating) {
      // nicht im Handler blockieren -> Flag setzen, Start im loop()
      calibrating = true;
      server.send(200, "text/plain", "OK: Calibration will start on TFT now.");
    } else {
      server.send(200, "text/plain", "Already calibrating.");
    }
  });

  server.on("/api/reset_cal", HTTP_GET, [](){
    resetCal();
    server.send(200, "text/plain", "Calibration reset.");
    drawHome();
    drawCalParams();
  });

  server.begin();
}

// ===================== Setup / Loop =====================
void setup() {
  Serial.begin(115200);

  // Backlight
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH); // falls dunkel: LOW testen

  // TFT
  lcd.init();
  lcd.setRotation(1);

  // Touch SPI
  pinMode(TOUCH_CS, OUTPUT);
  digitalWrite(TOUCH_CS, HIGH);
  vspi.begin(TFT_SCK, TFT_MISO, TFT_MOSI, -1);

  loadCal();

  // AP
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  delay(100);

  setupWeb();
  drawHome();
  if (cal.valid) drawCalParams();
}

void loop() {
  server.handleClient();

  // Start calibration if requested from web
  if (calibrating) {
    // calibrating flag is used as trigger; startCalibration() will clear it on exit
    // But we need a separate internal "started" to avoid re-enter.
    static bool started = false;
    if (!started) {
      started = true;
      startCalibration();
      started = false;
      calibrating = false;
    }
    return;
  }

  // normal paint
  handleTouchPaint();
}
