#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <LovyanGFX.hpp>

// ===================== UI MODE (MUSS GANZ NACH OBEN) =====================
enum Mode : uint8_t { MODE_DEMO = 0, MODE_PAINT = 1, MODE_INFO = 2 };
Mode mode = MODE_DEMO;

// Forward declarations (wichtig für Arduino CLI!)
void setMode(Mode m);
void handleTouch();
void demoTick();

// ===================== AP / OTA =====================
const char* AP_SSID = "ESP32-OTA";
const char* AP_PASS = "12345678";
WebServer server(80);

// ===================== Pins =====================
#define TFT_SCK   18
#define TFT_MOSI  23
#define TFT_MISO  19
#define TFT_CS    5
#define TFT_DC    21
#define TFT_RST   22
#define TFT_BL    32

#define TOUCH_CS  27
#define TOUCH_IRQ -1   // Polling

// ===================== LovyanGFX =====================
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9488 _panel;
  lgfx::Bus_SPI _bus;
  lgfx::Touch_XPT2046 _touch;

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
      cfg.invert = false;
      _panel.config(cfg);
    }

    { // Touch
      auto cfg = _touch.config();
      cfg.x_min = 0; cfg.x_max = 319;
      cfg.y_min = 0; cfg.y_max = 479;
      cfg.pin_int = TOUCH_IRQ;
      cfg.bus_shared = true;
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

// ===================== OTA WEB =====================
const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html><body>
<h2>ESP32 TFT OTA</h2>
<a href="/update">Firmware upload</a>
</body></html>
)HTML";

const char UPDATE_HTML[] PROGMEM = R"HTML(
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
    [](){ server.send(200,"text/plain", Update.hasError()?"FAIL":"OK"); ESP.restart(); },
    [](){
      HTTPUpload& u = server.upload();
      if (u.status == UPLOAD_FILE_START) Update.begin(UPDATE_SIZE_UNKNOWN);
      else if (u.status == UPLOAD_FILE_WRITE) Update.write(u.buf,u.currentSize);
      else if (u.status == UPLOAD_FILE_END) Update.end(true);
    }
  );
  server.begin();
}

// ===================== UI =====================
void drawHeader(const char* t) {
  lcd.fillRect(0,0,320,50,TFT_DARKGREY);
  lcd.setTextColor(TFT_WHITE);
  lcd.setTextSize(2);
  lcd.setCursor(10,15);
  lcd.print(t);
}

void setMode(Mode m) {
  mode = m;
  lcd.fillScreen(TFT_BLACK);
  if (m == MODE_DEMO)  drawHeader("DEMO");
  if (m == MODE_PAINT) drawHeader("PAINT");
  if (m == MODE_INFO)  drawHeader("INFO");
}

void handleTouch() {
  uint16_t x,y;
  if (!lcd.getTouch(&x,&y)) return;

  if (y < 50) {
    if (x < 106) setMode(MODE_DEMO);
    else if (x < 213) setMode(MODE_PAINT);
    else setMode(MODE_INFO);
    return;
  }

  if (mode == MODE_PAINT) lcd.fillCircle(x,y,6,TFT_CYAN);
  if (mode == MODE_INFO) {
    lcd.fillRect(0,430,320,50,TFT_BLACK);
    lcd.setCursor(10,440);
    lcd.printf("x=%u y=%u",x,y);
  }
}

int bx=50, by=120, vx=3, vy=2;
void demoTick() {
  if (mode != MODE_DEMO) return;
  lcd.fillCircle(bx,by,8,TFT_BLACK);
  bx+=vx; by+=vy;
  if (bx<10||bx>310) vx=-vx;
  if (by<60||by>420) vy=-vy;
  lcd.fillCircle(bx,by,8,TFT_WHITE);
}

// ===================== SETUP / LOOP =====================
void setup() {
  pinMode(TFT_BL,OUTPUT);
  digitalWrite(TFT_BL,HIGH);

  lcd.init();
  lcd.setRotation(1);
  lcd.fillScreen(TFT_BLACK);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  setupWebOTA();

  setMode(MODE_DEMO);
}

void loop() {
  server.handleClient();
  handleTouch();
  demoTick();
}
