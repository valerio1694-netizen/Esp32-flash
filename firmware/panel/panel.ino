#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <SPI.h>
#include <LovyanGFX.hpp>

// ===================== AP / OTA =====================
const char* AP_SSID = "ESP32-OTA";
const char* AP_PASS = "12345678";
WebServer server(80);

// ===================== Pins =====================
// TFT (VSPI)
static constexpr int TFT_SCK  = 18;
static constexpr int TFT_MOSI = 23;
static constexpr int TFT_MISO = 19;
static constexpr int TFT_CS   = 5;
static constexpr int TFT_DC   = 21;
static constexpr int TFT_RST  = 22;
static constexpr int TFT_BL   = 32;

// Touch (XPT2046)
static constexpr int TOUCH_CS  = 27;
// T_IRQ wird hier NICHT benutzt (Polling)

// ===================== LovyanGFX (nur TFT) =====================
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9488 _panel;
  lgfx::Bus_SPI _bus;
public:
  LGFX() {
    { // SPI
      auto cfg = _bus.config();
      cfg.spi_host   = VSPI_HOST;
      cfg.spi_mode   = 0;
      cfg.freq_write = 27000000;
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
      cfg.invert = false;
      _panel.config(cfg);
    }

    setPanel(&_panel);
  }
};

LGFX lcd;

// ===================== OTA Web =====================
static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html><body>
<h2>ESP32 TFT OTA</h2>
<a href="/update">Firmware upload</a>
</body></html>
)HTML";

static const char UPDATE_HTML[] PROGMEM = R"HTML(
<!doctype html><html><body>
<h2>OTA Update</h2>
<form method="POST" action="/update" enctype="multipart/form-data">
<input type="file" name="update">
<input type="submit" value="Upload">
</form>
</body></html>
)HTML";

void setupWebOTA() {
  server.on("/", HTTP_GET, [](){ server.send_P(200,"text/html",INDEX_HTML); });
  server.on("/update", HTTP_GET, [](){ server.send_P(200,"text/html",UPDATE_HTML); });
  server.on("/update", HTTP_POST,
    [](){ server.send(200,"text/plain", Update.hasError()?"FAIL":"OK"); delay(200); ESP.restart(); },
    [](){
      HTTPUpload& u = server.upload();
      if (u.status == UPLOAD_FILE_START) Update.begin(UPDATE_SIZE_UNKNOWN);
      else if (u.status == UPLOAD_FILE_WRITE) Update.write(u.buf,u.currentSize);
      else if (u.status == UPLOAD_FILE_END) Update.end(true);
      else if (u.status == UPLOAD_FILE_ABORTED) Update.end();
    }
  );
  server.begin();
}

// ===================== XPT2046 RAW SPI =====================
SPIClass vspi(VSPI);
SPISettings touchSPI(2000000, MSBFIRST, SPI_MODE0);

// XPT2046 Commands (12-bit)
static constexpr uint8_t CMD_X = 0xD0; // X position
static constexpr uint8_t CMD_Y = 0x90; // Y position

uint16_t xpt2046_read12(uint8_t cmd) {
  vspi.beginTransaction(touchSPI);
  digitalWrite(TOUCH_CS, LOW);

  vspi.transfer(cmd);
  // XPT2046 liefert 12-bit linksbündig über 16 clocks -> >>3
  uint16_t v = vspi.transfer16(0x0000);
  digitalWrite(TOUCH_CS, HIGH);
  vspi.endTransaction();

  return (v >> 3) & 0x0FFF;
}

// Liest mehrfach und filtert Müll
bool xpt2046_readXY(uint16_t &rx, uint16_t &ry) {
  // Dummy reads (einige Boards brauchen das)
  (void)xpt2046_read12(CMD_X);
  (void)xpt2046_read12(CMD_Y);

  uint16_t x1 = xpt2046_read12(CMD_X);
  uint16_t y1 = xpt2046_read12(CMD_Y);
  uint16_t x2 = xpt2046_read12(CMD_X);
  uint16_t y2 = xpt2046_read12(CMD_Y);

  // Wenn komplett 0 oder komplett 4095 -> meist MISO tot / CS falsch
  if ((x1 == 0 && y1 == 0) || (x1 == 4095 && y1 == 4095)) return false;
  if ((x2 == 0 && y2 == 0) || (x2 == 4095 && y2 == 4095)) return false;

  // Plausibilitätscheck: zwei Messungen müssen halbwegs nah sein
  auto diff = [](uint16_t a, uint16_t b){ return (a > b) ? (a - b) : (b - a); };
  if (diff(x1, x2) > 200 || diff(y1, y2) > 200) return false;

  rx = (x1 + x2) / 2;
  ry = (y1 + y2) / 2;
  return true;
}

// Grobe Kalibrierung (später anpassen)
// Viele XPT2046 liegen grob bei 200..3800
static constexpr int RAW_MIN = 200;
static constexpr int RAW_MAX = 3800;

int mapClamp(int v, int inMin, int inMax, int outMin, int outMax) {
  if (v < inMin) v = inMin;
  if (v > inMax) v = inMax;
  long num = (long)(v - inMin) * (outMax - outMin);
  long den = (inMax - inMin);
  return (int)(outMin + num / den);
}

void drawHeader() {
  lcd.fillScreen(TFT_BLACK);
  lcd.setTextSize(2);
  lcd.setTextColor(TFT_WHITE);
  lcd.setCursor(10, 10);
  lcd.println("XPT2046 RAW TEST");
  lcd.setTextColor(TFT_YELLOW);
  lcd.setCursor(10, 40);
  lcd.println("Tippe -> RAW + Punkt");

  lcd.setTextColor(TFT_CYAN);
  lcd.setCursor(10, 80);
  lcd.print("OTA: http://192.168.4.1/update");
}

// ===================== Setup / Loop =====================
void setup() {
  Serial.begin(115200);

  // Backlight
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH); // falls dunkel: LOW testen

  // TFT init
  lcd.init();
  lcd.setRotation(1);
  drawHeader();

  // AP + OTA
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  setupWebOTA();

  // Touch SPI init (VSPI, gleiche Leitungen)
  pinMode(TOUCH_CS, OUTPUT);
  digitalWrite(TOUCH_CS, HIGH);

  vspi.begin(TFT_SCK, TFT_MISO, TFT_MOSI, -1);

  // Anzeige AP-IP
  lcd.setTextColor(TFT_GREEN);
  lcd.setCursor(10, 110);
  lcd.print("AP IP: ");
  lcd.println(WiFi.softAPIP());

  lcd.setTextColor(TFT_WHITE);
  lcd.setCursor(10, 140);
  lcd.println("Wenn RAW=0/4095 -> MISO/CS");
}

void loop() {
  server.handleClient();

  uint16_t rx, ry;
  if (xpt2046_readXY(rx, ry)) {
    Serial.printf("RAW x=%u y=%u\n", rx, ry);

    // RAW anzeigen
    lcd.fillRect(0, 170, 320, 40, TFT_BLACK);
    lcd.setTextColor(TFT_WHITE);
    lcd.setTextSize(2);
    lcd.setCursor(10, 180);
    lcd.printf("RAW x=%u y=%u", rx, ry);

    // Grob auf Screen mappen (kann gedreht/gespiegelt sein – egal, Hauptsache lebt)
    int sx = mapClamp((int)rx, RAW_MIN, RAW_MAX, 0, 319);
    int sy = mapClamp((int)ry, RAW_MIN, RAW_MAX, 0, 479);

    lcd.fillCircle(sx, sy, 8, TFT_RED);
    delay(30);
  }
}
