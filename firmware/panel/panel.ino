#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <LovyanGFX.hpp>

// ===================== AP / OTA =====================
const char* AP_SSID = "ESP32-OTA";
const char* AP_PASS = "12345678";   // >=8 Zeichen oder "" für offen
WebServer server(80);

// ===================== Pins =====================
// TFT (VSPI)
static constexpr int TFT_SCK  = 18;
static constexpr int TFT_MOSI = 23;
static constexpr int TFT_MISO = 19;
static constexpr int TFT_CS   = 5;
static constexpr int TFT_DC   = 21;
static constexpr int TFT_RST  = 22;
static constexpr int TFT_BL   = 32;   // Backlight GPIO

// Touch (XPT2046)
static constexpr int TOUCH_CS  = 27;
static constexpr int TOUCH_IRQ = -1;  // IRQ AUS (Polling). Wenn du IRQ sauber verdrahtet hast: z.B. 33

// ===================== LovyanGFX: ILI9488 + XPT2046 =====================
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9488 _panel;
  lgfx::Bus_SPI       _bus;
  lgfx::Touch_XPT2046 _touch;

public:
  LGFX() {
    { // SPI Bus
      auto cfg = _bus.config();
      cfg.spi_host   = VSPI_HOST;
      cfg.spi_mode   = 0;
      cfg.freq_write = 27000000;   // stabil; wenn sicher: 40000000 testen
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

      cfg.invert   = false; // wenn Farben komisch: true/false wechseln
      _panel.config(cfg);
    }

    { // Touch XPT2046 (shared SPI)
      auto cfg = _touch.config();
      cfg.x_min = 0;  cfg.x_max = 319;
      cfg.y_min = 0;  cfg.y_max = 479;

      cfg.pin_int = TOUCH_IRQ;     // -1 = polling
      cfg.bus_shared = true;

      // WICHTIG: wenn Touch nicht stimmt: 0/1/2/3 testen
      cfg.offset_rotation = 1;

      cfg.spi_host = VSPI_HOST;
      cfg.freq     = 2000000;
      cfg.pin_sclk = TFT_SCK;
      cfg.pin_mosi = TFT_MOSI;
      cfg.pin_miso = TFT_MISO;
      cfg.pin_cs   = TOUCH_CS;

      _touch.config(cfg);
      _panel.setTouch(&_touch);
    }

    setPanel(&_panel);
  }
};

LGFX lcd;

// ===================== OTA Webseiten =====================
static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 OTA</title></head>
<body style="font-family:sans-serif">
<h2>ESP32 TFT OTA (AP)</h2>
<p><a href="/update">Firmware hochladen</a></p>
</body></html>
)HTML";

static const char UPDATE_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>OTA Update</title></head>
<body style="font-family:sans-serif">
<h2>Firmware Upload</h2>
<form method="POST" action="/update" enctype="multipart/form-data">
  <input type="file" name="update">
  <input type="submit" value="Upload">
</form>
<p>Nach Upload rebootet der ESP32 automatisch.</p>
</body></html>
)HTML";

void setupWebOTA() {
  server.on("/", HTTP_GET, []() {
    server.send_P(200, "text/html", INDEX_HTML);
  });

  server.on("/update", HTTP_GET, []() {
    server.send_P(200, "text/html", UPDATE_HTML);
  });

  server.on("/update", HTTP_POST,
    []() {
      server.send(200, "text/plain", Update.hasError() ? "FAIL" : "OK. Reboot...");
      delay(300);
      ESP.restart();
    },
    []() {
      HTTPUpload& up = server.upload();
      if (up.status == UPLOAD_FILE_START) {
        Update.begin(UPDATE_SIZE_UNKNOWN);
      } else if (up.status == UPLOAD_FILE_WRITE) {
        Update.write(up.buf, up.currentSize);
      } else if (up.status == UPLOAD_FILE_END) {
        Update.end(true);
      } else if (up.status == UPLOAD_FILE_ABORTED) {
        Update.end();
      }
    }
  );

  server.begin();
}

// ===================== UI / Demo =====================
enum Mode : uint8_t { MODE_DEMO = 0, MODE_PAINT = 1, MODE_INFO = 2 };
Mode mode = MODE_DEMO;

struct Button {
  int x, y, w, h;
  const char* label;
  Mode target;
};

static const Button btns[] = {
  {  10,  10,  95, 40, "DEMO",  MODE_DEMO  },
  { 112,  10,  95, 40, "PAINT", MODE_PAINT },
  { 214,  10,  95, 40, "INFO",  MODE_INFO  },
};

static inline bool inRect(int px, int py, int x, int y, int w, int h) {
  return (px >= x && px < x + w && py >= y && py < y + h);
}

void drawButtons() {
  for (auto &b : btns) {
    bool active = (mode == b.target);
    lcd.fillRoundRect(b.x, b.y, b.w, b.h, 8, active ? TFT_DARKGREEN : TFT_DARKGREY);
    lcd.drawRoundRect(b.x, b.y, b.w, b.h, 8, TFT_WHITE);
    lcd.setTextSize(2);
    lcd.setTextColor(TFT_WHITE);
    int tx = b.x + 10;
    int ty = b.y + 12;
    lcd.setCursor(tx, ty);
    lcd.print(b.label);
  }
}

void drawStatusBar(const char* msg) {
  lcd.fillRect(0, 430, 320, 50, TFT_BLACK);
  lcd.drawFastHLine(0, 430, 320, TFT_DARKGREY);
  lcd.setTextSize(2);
  lcd.setTextColor(TFT_YELLOW);
  lcd.setCursor(10, 445);
  lcd.print(msg);
}

void renderInfoScreen() {
  lcd.fillScreen(TFT_BLACK);
  drawButtons();

  lcd.setTextSize(2);
  lcd.setTextColor(TFT_WHITE);
  lcd.setCursor(10, 70);
  lcd.println("ESP32 TFT + Touch + OTA");

  lcd.setCursor(10, 100);
  lcd.print("AP: ");
  lcd.println(AP_SSID);

  lcd.setCursor(10, 130);
  lcd.print("IP: ");
  lcd.println(WiFi.softAPIP());

  lcd.setCursor(10, 160);
  lcd.println("OTA:");
  lcd.setCursor(10, 190);
  lcd.println("http://192.168.4.1/update");

  lcd.setCursor(10, 230);
  lcd.println("Touch Test:");
  lcd.setCursor(10, 260);
  lcd.println("Tippe irgendwo ->");
  lcd.setCursor(10, 290);
  lcd.println("Koordinaten unten");

  drawStatusBar("INFO");
}

void renderPaintScreen() {
  lcd.fillScreen(TFT_BLACK);
  drawButtons();
  lcd.drawRect(5, 60, 310, 365, TFT_DARKGREY);
  drawStatusBar("PAINT: Malen mit Finger");
}

void renderDemoScreen() {
  lcd.fillScreen(TFT_BLACK);
  drawButtons();
  drawStatusBar("DEMO: Farben/Shapes/Anim");
}

// Simple Demo Animation State
int ballX = 30, ballY = 120;
int vx = 3, vy = 2;
uint32_t lastDemoMs = 0;

void demoTick() {
  uint32_t now = millis();
  if (now - lastDemoMs < 16) return; // ~60fps
  lastDemoMs = now;

  // Hintergrundbereich (unter Buttons, über Status)
  const int top = 60;
  const int bottom = 430;

  // leichter Gradient + Balken (sparsam, sonst langsam)
  static uint16_t frame = 0;
  frame++;
  if ((frame % 20) == 0) {
    for (int y = top; y < bottom; y += 6) {
      uint16_t c = lcd.color565((y * 2) & 255, (y * 3) & 255, (y * 5) & 255);
      lcd.fillRect(0, y, 320, 6, c);
    }
    // Farbbalken
    lcd.fillRect(0, bottom - 25, 320, 25, TFT_BLACK);
    lcd.fillRect(0, bottom - 25, 80, 25, TFT_RED);
    lcd.fillRect(80, bottom - 25, 80, 25, TFT_GREEN);
    lcd.fillRect(160, bottom - 25, 80, 25, TFT_BLUE);
    lcd.fillRect(240, bottom - 25, 80, 25, TFT_YELLOW);
  }

  // Ball löschen (kleine Area)
  lcd.fillCircle(ballX, ballY, 10, TFT_BLACK);

  ballX += vx; ballY += vy;
  if (ballX < 12 || ballX > 308) vx = -vx;
  if (ballY < top + 12 || ballY > bottom - 40) vy = -vy;

  // Ball zeichnen
  lcd.fillCircle(ballX, ballY, 10, TFT_WHITE);
  lcd.drawCircle(ballX, ballY, 10, TFT_BLACK);

  // ein paar Shapes
  lcd.drawRoundRect(10, top + 10, 140, 60, 10, TFT_BLACK);
  lcd.setTextSize(2);
  lcd.setTextColor(TFT_BLACK);
  lcd.setCursor(20, top + 30);
  lcd.print("ILI9488?");

  lcd.drawTriangle(200, top + 70, 280, top + 20, 300, top + 110, TFT_BLACK);
}

void setMode(Mode m) {
  mode = m;
  if (mode == MODE_DEMO)  renderDemoScreen();
  if (mode == MODE_PAINT) renderPaintScreen();
  if (mode == MODE_INFO)  renderInfoScreen();
}

// Touch handling
uint32_t lastTouchMs = 0;

void handleTouch() {
  uint16_t x, y;
  if (!lcd.getTouch(&x, &y)) return;

  // simples Debounce
  uint32_t now = millis();
  if (now - lastTouchMs < 40) return;
  lastTouchMs = now;

  // Buttons
  for (auto &b : btns) {
    if (inRect(x, y, b.x, b.y, b.w, b.h)) {
      setMode(b.target);
      return;
    }
  }

  // In INFO: Koordinaten anzeigen
  if (mode == MODE_INFO) {
    char buf[64];
    snprintf(buf, sizeof(buf), "Touch x=%u y=%u", (unsigned)x, (unsigned)y);
    drawStatusBar(buf);
    // sichtbarer Marker
    lcd.fillCircle(x, y, 6, TFT_RED);
    return;
  }

  // In PAINT: Malen nur im Zeichenfeld
  if (mode == MODE_PAINT) {
    if (inRect(x, y, 5, 60, 310, 365)) {
      lcd.fillCircle(x, y, 6, TFT_CYAN);
      char buf[64];
      snprintf(buf, sizeof(buf), "x=%u y=%u", (unsigned)x, (unsigned)y);
      drawStatusBar(buf);
    }
    return;
  }

  // In DEMO: Tippen = invert toggle
  if (mode == MODE_DEMO) {
    drawStatusBar("Touch gesehen (DEMO)");
    lcd.fillCircle(x, y, 6, TFT_MAGENTA);
  }
}

// ===================== Setup / Loop =====================
void setup() {
  Serial.begin(115200);

  // Backlight an (ohne LovyanGFX-Backlight API)
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);   // falls dunkel: LOW testen (active-low Backlight)

  lcd.init();
  lcd.setRotation(1);
  lcd.fillScreen(TFT_BLACK);

  // AP starten
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  delay(100);

  setupWebOTA();

  // Startscreen
  setMode(MODE_DEMO);
}

void loop() {
  server.handleClient();

  handleTouch();

  if (mode == MODE_DEMO) {
    demoTick();
  }
}
