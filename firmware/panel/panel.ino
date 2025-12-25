#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>

#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

/* ---------------- WLAN AP ---------------- */
static const char* AP_SSID = "ESP32-Panel";
static const char* AP_PASS = "12345678";

/* ---------------- TFT ---------------- */
TFT_eSPI tft = TFT_eSPI();

/* ---------------- TOUCH (XPT2046) ----------------
   WICHTIG: Diese Pins musst du ggf. an deine Verdrahtung anpassen!
*/
#define TOUCH_CS   15   // <- DEIN Touch CS Pin
#define TOUCH_IRQ  27   // <- DEIN Touch IRQ Pin (wenn nicht angeschlossen: -1)

#if (TOUCH_IRQ >= 0)
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);
#else
XPT2046_Touchscreen ts(TOUCH_CS);
#endif

/* ---- Touch Kalibrierwerte (erstmal Rohwerte anzeigen, dann eintragen) ----
   Nach dem Test trägst du hier echte Werte ein und aktivierst USE_CALIB = 1
*/
#define USE_CALIB 0
// Diese 4 Werte wirst du nach dem Test ersetzen:
static int TS_MINX = 200;
static int TS_MAXX = 3800;
static int TS_MINY = 200;
static int TS_MAXY = 3800;

WebServer server(80);

/* ---------------- HTML OTA ---------------- */
static String page() {
  String s;
  s += "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  s += "<title>ESP32 OTA</title></head><body>";
  s += "<h2>ESP32 OTA Update</h2>";
  s += "<form method='POST' action='/update' enctype='multipart/form-data'>";
  s += "<input type='file' name='update'><input type='submit' value='Update'></form>";
  s += "<p>Nach Upload rebootet das Board.</p>";
  s += "</body></html>";
  return s;
}

static void drawTouchMarker(int x, int y) {
  tft.fillCircle(x, y, 6, TFT_WHITE);
  tft.drawCircle(x, y, 10, TFT_BLACK);
}

static void drawUIFrame() {
  tft.fillScreen(TFT_BLACK);

  // Querformat wie bei dir funktionierend
  tft.setRotation(1);

  // einfache Statusleiste oben
  tft.fillRect(0, 0, 480, 30, TFT_DARKGREY);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
  tft.setTextSize(2);
  tft.setCursor(10, 6);
  tft.print("Touch Test (XPT2046)");

  // Info unten
  tft.fillRect(0, 300, 480, 20, TFT_DARKGREY);
  tft.setCursor(10, 304);
  tft.print("Tippe Bildschirm: Punkte + Werte");
}

static bool mapTouchToScreen(int rawx, int rawy, int &sx, int &sy) {
#if USE_CALIB
  // Rohwerte auf 480x320 mappen
  // Achtung: Je nach Rotation kann X/Y getauscht oder invertiert sein.
  // Startannahme fuer setRotation(1):
  sx = map(rawx, TS_MINX, TS_MAXX, 0, 480);
  sy = map(rawy, TS_MINY, TS_MAXY, 0, 320);

  // Clamp
  if (sx < 0) sx = 0; if (sx > 479) sx = 479;
  if (sy < 0) sy = 0; if (sy > 319) sy = 319;
  return true;
#else
  // Ohne Kalib: nur grob, damit du Rohwerte siehst (kein echtes Mapping)
  sx = map(rawx, 0, 4095, 0, 480);
  sy = map(rawy, 0, 4095, 0, 320);
  if (sx < 0) sx = 0; if (sx > 479) sx = 479;
  if (sy < 0) sy = 0; if (sy > 319) sy = 319;
  return true;
#endif
}

void setup() {
  Serial.begin(115200);
  delay(200);

  // WLAN AP
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  delay(200);

  // TFT
  tft.init();
  tft.setRotation(1);
  drawUIFrame();

  // Touch SPI initialisieren
  // Wichtig: Display und Touch teilen sich SPI Leitungen, aber haben eigene CS Pins.
  SPI.begin(18, 19, 23);   // SCK, MISO, MOSI
  ts.begin();
  ts.setRotation(1);       // passt meist zu tft.setRotation(1), ggf. ändern

  // OTA Web
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", page());
  });

  server.on("/update", HTTP_POST,
    []() {
      server.sendHeader("Connection", "close");
      server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
      delay(500);
      ESP.restart();
    },
    []() {
      HTTPUpload& upload = server.upload();
      if (upload.status == UPLOAD_FILE_START) {
        Update.begin(UPDATE_SIZE_UNKNOWN);
      } else if (upload.status == UPLOAD_FILE_WRITE) {
        Update.write(upload.buf, upload.currentSize);
      } else if (upload.status == UPLOAD_FILE_END) {
        Update.end(true);
      }
    }
  );

  server.begin();

  // Kurz Farbtest (damit du sofort siehst: Display OK)
  int w = tft.width() / 5;
  int h = tft.height();
  tft.fillRect(0*w, 30, w, h-50, TFT_MAGENTA);
  tft.fillRect(1*w, 30, w, h-50, TFT_YELLOW);
  tft.fillRect(2*w, 30, w, h-50, TFT_BLUE);
  tft.fillRect(3*w, 30, w, h-50, TFT_GREEN);
  tft.fillRect(4*w, 30, w, h-50, 0xFD20); // orange-ish
  drawUIFrame();

  Serial.println("Touch Test ready. Tippe auf Display.");
}

void loop() {
  server.handleClient();

  if (ts.touched()) {
    TS_Point p = ts.getPoint();

    // Rohwerte
    int rx = p.x;
    int ry = p.y;
    int rz = p.z;

    int sx, sy;
    mapTouchToScreen(rx, ry, sx, sy);

    // Display Marker
    drawTouchMarker(sx, sy);

    // Text unten aktualisieren
    tft.fillRect(0, 300, 480, 20, TFT_DARKGREY);
    tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
    tft.setTextSize(2);
    tft.setCursor(10, 304);
    tft.printf("RAW x=%4d y=%4d z=%4d", rx, ry, rz);

    // Serial auch
    Serial.printf("RAW x=%d y=%d z=%d -> SCR x=%d y=%d\n", rx, ry, rz, sx, sy);

    delay(80); // entprellen
  }
}
