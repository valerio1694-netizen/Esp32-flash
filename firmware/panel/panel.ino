#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <LovyanGFX.hpp>

// ===================== AP + Web Login =====================
const char* AP_SSID = "ESP32-OTA";
const char* AP_PASS = "12345678";       // >= 8 Zeichen oder "" (offen)

const char* WWW_USER = "admin";         // Web-Login
const char* WWW_PASS = "admin123";

WebServer server(80);

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
      cfg.freq_write = 27000000;   // 27MHz = stabil; wenn's sicher läuft -> 40000000 testen
      cfg.freq_read  = 16000000;
      cfg.pin_sclk   = 18;
      cfg.pin_mosi   = 23;
      cfg.pin_miso   = 19;         // empfohlen
      cfg.pin_dc     = 21;         // D/C
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }

    { // --- Panel ---
      auto cfg = _panel.config();
      cfg.pin_cs   = 5;
      cfg.pin_rst  = 22;
      cfg.pin_busy = -1;

      cfg.panel_width  = 320;
      cfg.panel_height = 480;

      cfg.offset_x = 0;
      cfg.offset_y = 0;

      cfg.rotation = 1;     // falls falsch: 0..3 testen
      cfg.invert   = false; // falls Farben komisch: true/false wechseln

      _panel.config(cfg);
    }

    { // --- Touch XPT2046 (shared SPI) ---
      auto cfg = _touch.config();
      cfg.x_min = 0;  cfg.x_max = 319;
      cfg.y_min = 0;  cfg.y_max = 479;

      cfg.pin_int = 33;          // T_IRQ; wenn nicht angeschlossen: -1 setzen
      cfg.bus_shared = true;
      cfg.offset_rotation = 1;   // muss meist zur Display-Rotation passen

      cfg.spi_host = VSPI_HOST;
      cfg.freq     = 2000000;
      cfg.pin_sclk = 18;
      cfg.pin_mosi = 23;
      cfg.pin_miso = 19;
      cfg.pin_cs   = 27;         // T_CS

      _touch.config(cfg);
      _panel.setTouch(&_touch);
    }

    setPanel(&_panel);
  }
};

LGFX lcd;

// ===================== Webseiten =====================
static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 TFT OTA</title></head>
<body style="font-family:sans-serif">
<h2>ESP32 TFT + Touch + OTA (AP)</h2>
<ul>
  <li><a href="/update">Firmware hochladen</a></li>
  <li><a href="/info">Info</a></li>
</ul>
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
<p>Nach erfolgreichem Upload rebootet der ESP32.</p>
</body></html>
)HTML";

bool checkAuth() {
  if (!server.authenticate(WWW_USER, WWW_PASS)) {
    server.requestAuthentication();
    return false;
  }
  return true;
}

// ===================== Setup AP =====================
void setupAP() {
  WiFi.mode(WIFI_AP);
  bool ok = WiFi.softAP(AP_SSID, AP_PASS);
  delay(100);

  lcd.fillScreen(TFT_BLACK);
  lcd.setTextSize(2);
  lcd.setTextColor(TFT_WHITE);
  lcd.setCursor(10, 10);

  if (!ok) {
    lcd.println("AP Start FAIL");
    return;
  }

  IPAddress ip = WiFi.softAPIP(); // meist 192.168.4.1
  lcd.println("AP aktiv:");
  lcd.println(AP_SSID);
  lcd.println("IP:");
  lcd.println(ip);

  lcd.setTextColor(TFT_YELLOW);
  lcd.setCursor(10, 120);
  lcd.println("Browser:");
  lcd.println("http://192.168.4.1");
  lcd.println("/update");
}

// ===================== Webserver Routen + OTA =====================
void setupWeb() {
  server.on("/", HTTP_GET, []() {
    if (!checkAuth()) return;
    server.send_P(200, "text/html", INDEX_HTML);
  });

  server.on("/info", HTTP_GET, []() {
    if (!checkAuth()) return;
    String txt;
    txt += "AP SSID: " + String(AP_SSID) + "\n";
    txt += "AP IP: " + WiFi.softAPIP().toString() + "\n";
    txt += "Clients: " + String(WiFi.softAPgetStationNum()) + "\n";
    txt += "Uptime(ms): " + String(millis()) + "\n";
    txt += "Sketch size: " + String(ESP.getSketchSize()) + "\n";
    txt += "Free sketch: " + String(ESP.getFreeSketchSpace()) + "\n";
    server.send(200, "text/plain", txt);
  });

  server.on("/update", HTTP_GET, []() {
    if (!checkAuth()) return;
    server.send_P(200, "text/html", UPDATE_HTML);
  });

  server.on("/update", HTTP_POST,
    []() { // fertig
      if (!checkAuth()) return;
      bool ok = !Update.hasError();
      server.send(200, "text/plain", ok ? "OK. Reboot..." : "FAIL. Update error.");
      delay(300);
      ESP.restart();
    },
    []() { // upload handler
      if (!checkAuth()) return;
      HTTPUpload& up = server.upload();

      if (up.status == UPLOAD_FILE_START) {
        // Display kurz Hinweis
        lcd.fillRect(0, 200, 320, 60, TFT_BLACK);
        lcd.setCursor(10, 200);
        lcd.setTextColor(TFT_CYAN);
        lcd.println("OTA Upload...");

        // Start OTA
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
          // Fehler -> auf Display/Serial
          lcd.setTextColor(TFT_RED);
          lcd.println("Update.begin FAIL");
        }
      }
      else if (up.status == UPLOAD_FILE_WRITE) {
        // Chunk schreiben
        size_t w = Update.write(up.buf, up.currentSize);
        if (w != up.currentSize) {
          // Schreibfehler
        }
      }
      else if (up.status == UPLOAD_FILE_END) {
        if (Update.end(true)) {
          lcd.setTextColor(TFT_GREEN);
          lcd.println("Upload OK");
        } else {
          lcd.setTextColor(TFT_RED);
          lcd.println("Upload FAIL");
        }
      }
      else if (up.status == UPLOAD_FILE_ABORTED) {
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

  lcd.init();
  lcd.setRotation(1);
  lcd.fillScreen(TFT_BLACK);

  // simpler Farbtest
  lcd.fillRect(0, 280, 320, 40, TFT_RED);
  lcd.fillRect(0, 320, 320, 40, TFT_GREEN);
  lcd.fillRect(0, 360, 320, 40, TFT_BLUE);

  setupAP();
  setupWeb();
}

void loop() {
  server.handleClient();

  // Touch-Visualisierung (läuft auch neben WebServer, außer während Upload blockiert)
  uint16_t x, y;
  if (lcd.getTouch(&x, &y)) {
    lcd.fillCircle(x, y, 3, TFT_WHITE);
    delay(10);
  }
}
