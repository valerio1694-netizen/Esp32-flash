#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <LovyanGFX.hpp>

// ===================== AP =====================
const char* AP_SSID = "ESP32-OTA";
const char* AP_PASS = "12345678";

WebServer server(80);

// ===================== Pins =====================
// TFT
#define TFT_SCK   18
#define TFT_MOSI  23
#define TFT_MISO  19
#define TFT_CS    5
#define TFT_DC    21
#define TFT_RST   22
#define TFT_BL    32

// Touch
#define TOUCH_CS  27
#define TOUCH_IRQ -1   // IRQ bewusst AUS (Polling)

// ===================== LovyanGFX =====================
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9488 _panel;
  lgfx::Bus_SPI       _bus;
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
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.invert   = false;
      _panel.config(cfg);
    }

    { // Touch (XPT2046, shared SPI)
      auto cfg = _touch.config();
      cfg.x_min = 0;   cfg.x_max = 319;
      cfg.y_min = 0;   cfg.y_max = 479;

      cfg.pin_int = TOUCH_IRQ;     // -1 = polling
      cfg.bus_shared = true;
      cfg.offset_rotation = 1;     // ggf. später ändern

      cfg.spi_host = VSPI_HOST;
      cfg.freq     = 2000000;
      cfg.pin_sclk = TFT_SCK;
      cfg.pin_mosi = TFT_MOSI;
      cfg.pin_miso = TFT_MISO;     // WICHTIG
      cfg.pin_cs   = TOUCH_CS;

      _touch.config(cfg);
      _panel.setTouch(&_touch);
    }

    setPanel(&_panel);
  }
};

LGFX lcd;

// ===================== HTML =====================
const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html>
<head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 OTA</title></head>
<body>
<h2>ESP32 TFT OTA</h2>
<a href="/update">Firmware hochladen</a>
</body></html>
)HTML";

const char UPDATE_HTML[] PROGMEM = R"HTML(
<!doctype html><html>
<body>
<h2>OTA Upload</h2>
<form method="POST" action="/update" enctype="multipart/form-data">
<input type="file" name="update">
<input type="submit" value="Upload">
</form>
</body></html>
)HTML";

// ===================== Web + OTA =====================
void setupWeb() {
  server.on("/", HTTP_GET, []() {
    server.send_P(200, "text/html", INDEX_HTML);
  });

  server.on("/update", HTTP_GET, []() {
    server.send_P(200, "text/html", UPDATE_HTML);
  });

  server.on("/update", HTTP_POST,
    []() {
      server.send(200, "text/plain", Update.hasError() ? "FAIL" : "OK");
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
      }
    }
  );

  server.begin();
}

// ===================== SETUP =====================
void setup() {
  Serial.begin(115200);

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);   // Backlight an

  lcd.init();
  lcd.setRotation(1);
  lcd.fillScreen(TFT_BLACK);

  lcd.setTextSize(2);
  lcd.setTextColor(TFT_WHITE);
  lcd.setCursor(10, 10);
  lcd.println("TOUCH DEBUG");

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);

  lcd.println("AP:");
  lcd.println(AP_SSID);
  lcd.println(WiFi.softAPIP());

  setupWeb();
}

// ===================== LOOP =====================
void loop() {
  server.handleClient();

  uint16_t x, y;
  if (lcd.getTouch(&x, &y)) {
    Serial.printf("TOUCH x=%u y=%u\n", x, y);

    lcd.fillRect(0, 220, 320, 40, TFT_BLACK);
    lcd.setCursor(10, 220);
    lcd.setTextColor(TFT_YELLOW);
    lcd.printf("x=%u y=%u", x, y);

    lcd.fillCircle(x, y, 10, TFT_RED); // GROSSER Punkt
    delay(50);
  }
}
