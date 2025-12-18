#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <LovyanGFX.hpp>

// ===================== AP =====================
const char* AP_SSID = "ESP32-OTA";
const char* AP_PASS = "12345678";   // >=8 Zeichen oder "" für offen

WebServer server(80);

// ===================== Pins =====================
// TFT
static constexpr int TFT_SCK  = 18;
static constexpr int TFT_MOSI = 23;
static constexpr int TFT_MISO = 19;
static constexpr int TFT_CS   = 5;
static constexpr int TFT_DC   = 21;
static constexpr int TFT_RST  = 22;
static constexpr int TFT_BL   = 32;   // Backlight GPIO

// Touch (XPT2046)
static constexpr int TOUCH_CS  = 27;
static constexpr int TOUCH_IRQ = 33;  // wenn nicht angeschlossen: -1

// ===================== LovyanGFX: ILI9488 + XPT2046 =====================
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9488 _panel;
  lgfx::Bus_SPI       _bus;
  lgfx::Touch_XPT2046 _touch;

public:
  LGFX() {
    { // --- SPI Bus (VSPI) ---
      auto cfg = _bus.config();
      cfg.spi_host   = VSPI_HOST;
      cfg.spi_mode   = 0;
      cfg.freq_write = 27000000;   // stabil. Wenn sauber läuft: 40000000 testen
      cfg.freq_read  = 16000000;
      cfg.pin_sclk   = TFT_SCK;
      cfg.pin_mosi   = TFT_MOSI;
      cfg.pin_miso   = TFT_MISO;
      cfg.pin_dc     = TFT_DC;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }

    { // --- Panel ---
      auto cfg = _panel.config();
      cfg.pin_cs   = TFT_CS;
      cfg.pin_rst  = TFT_RST;
      cfg.pin_busy = -1;

      cfg.panel_width  = 320;
      cfg.panel_height = 480;

      cfg.offset_x = 0;
      cfg.offset_y = 0;

      cfg.invert   = false;  // bei komischen Farben: true/false wechseln
      _panel.config(cfg);
    }

    { // --- Touch XPT2046 (shared SPI) ---
      auto cfg = _touch.config();
      cfg.x_min = 0;  cfg.x_max = 319;
      cfg.y_min = 0;  cfg.y_max = 479;

      cfg.pin_int = TOUCH_IRQ;     // -1 wenn nicht genutzt
      cfg.bus_shared = true;

      // Muss zur lcd.setRotation() passen. Startwert:
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

// ===================== HTML =====================
static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html>
<head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 OTA</title></head>
<body style="font-family:sans-serif">
<h2>ESP32 TFT OTA (AP)</h2>
<p><a href="/update">Firmware hochladen</a></p>
</body></html>
)HTML";

static const char UPDATE_HTML[] PROGMEM = R"HTML(
<!doctype html><html>
<head><meta name="viewport" content="width=device-width,initial-scale=1">
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

// ===================== Web + OTA =====================
void setupWeb() {
  server.on("/", HTTP_GET, []() {
    server.send_P(200, "text/html", INDEX_HTML);
  });

  server.on("/update", HTTP_GET, []() {
    server.send_P(200, "text/html", UPDATE_HTML);
  });

  server.on("/update", HTTP_POST,
    []() { // done
      server.send(200, "text/plain", Update.hasError() ? "FAIL" : "OK. Reboot...");
      delay(300);
      ESP.restart();
    },
    []() { // upload handler
      HTTPUpload& up = server.upload();

      if (up.status == UPLOAD_FILE_START) {
        lcd.fillRect(0, 200, 320, 70, TFT_BLACK);
        lcd.setCursor(10, 200);
        lcd.setTextColor(TFT_CYAN);
        lcd.setTextSize(2);
        lcd.println("OTA Upload...");

        // Start OTA
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
          lcd.setTextColor(TFT_RED);
          lcd.println("Update.begin FAIL");
        }
      } else if (up.status == UPLOAD_FILE_WRITE) {
        // Chunk schreiben
        size_t w = Update.write(up.buf, up.currentSize);
        (void)w;
      } else if (up.status == UPLOAD_FILE_END) {
        if (Update.end(true)) {
          lcd.setTextColor(TFT_GREEN);
          lcd.println("Upload OK");
        } else {
          lcd.setTextColor(TFT_RED);
          lcd.println("Upload FAIL");
        }
      } else if (up.status == UPLOAD_FILE_ABORTED) {
        Update.end();
        lcd.setTextColor(TFT_RED);
        lcd.println("Upload ABORT");
      }
      delay(0);
    }
  );

  server.begin();
}

// ===================== Setup / Loop =====================
void setup() {
  Serial.begin(115200);

  // Backlight manuell (versionsunabhaengig)
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);  // falls dunkel: LOW testen (manche Boards sind active-low)

  lcd.init();
  lcd.setRotation(1);
  lcd.fillScreen(TFT_BLACK);

  // Farbtest
  lcd.fillRect(0, 280, 320, 40, TFT_RED);
  lcd.fillRect(0, 320, 320, 40, TFT_GREEN);
  lcd.fillRect(0, 360, 320, 40, TFT_BLUE);

  lcd.setTextSize(2);
  lcd.setTextColor(TFT_WHITE);
  lcd.setCursor(10, 10);
  lcd.println("ESP32 TFT + OTA");

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  delay(100);

  lcd.println("AP:");
  lcd.println(AP_SSID);
  lcd.println("IP:");
  lcd.println(WiFi.softAPIP());

  lcd.setTextColor(TFT_YELLOW);
  lcd.println("http://192.168.4.1/update");

  setupWeb();
}

void loop() {
  server.handleClient();

  // Touch-Punkt zeichnen
  uint16_t x, y;
  if (lcd.getTouch(&x, &y)) {
    lcd.fillCircle(x, y, 3, TFT_WHITE);
    delay(10);
  }
}
