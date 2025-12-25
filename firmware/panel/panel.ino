#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>

#include <SPI.h>
#include <TFT_eSPI.h>

// ===================== AP / OTA =====================
static const char* AP_SSID = "ESP32-OTA";
static const char* AP_PASS = "12345678";

WebServer server(80);

// ===================== Pins =====================
// Backlight: laut dir GPIO32
static constexpr int TFT_BL = 32;

// ===================== TFT =====================
TFT_eSPI tft = TFT_eSPI();   // nutzt User_Setup.h aus TFT_eSPI

// --------------------- HTML ---------------------
static const char OTA_PAGE[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
  <meta charset="utf-8"/>
  <meta name="viewport" content="width=device-width,initial-scale=1"/>
  <title>ESP32 OTA</title>
  <style>
    body{font-family:system-ui,Arial;margin:24px}
    .card{max-width:520px;padding:16px;border:1px solid #ddd;border-radius:12px}
    .row{margin:12px 0}
    button{padding:10px 14px;border:0;border-radius:10px;cursor:pointer}
    input{width:100%}
    progress{width:100%}
    code{background:#f4f4f4;padding:2px 6px;border-radius:6px}
  </style>
</head>
<body>
  <div class="card">
    <h2>ESP32 Web-AP OTA</h2>
    <div class="row">Upload <code>.bin</code> und flashen.</div>
    <div class="row">
      <input id="file" type="file" accept=".bin"/>
    </div>
    <div class="row">
      <button onclick="upload()">Upload</button>
    </div>
    <div class="row">
      <progress id="p" value="0" max="100"></progress>
      <div id="s"></div>
    </div>
  </div>

<script>
function upload(){
  const f=document.getElementById('file').files[0];
  if(!f){alert('Bitte .bin auswählen');return;}
  const xhr=new XMLHttpRequest();
  xhr.open('POST','/update',true);

  xhr.upload.onprogress = (e)=>{
    if(e.lengthComputable){
      const pct=Math.round((e.loaded/e.total)*100);
      document.getElementById('p').value=pct;
      document.getElementById('s').innerText=pct+'%';
    }
  };

  xhr.onload=()=>{
    document.getElementById('s').innerText = xhr.responseText || 'OK';
  };

  const fd=new FormData();
  fd.append('update',f,f.name);
  xhr.send(fd);
}
</script>
</body>
</html>
)HTML";

// ===================== Helpers =====================
void drawHomeScreen() {
  tft.fillScreen(TFT_BLACK);

  tft.setTextSize(2);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(12, 12);
  tft.println("ESP32 TFT + OTA");

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(12, 42);
  tft.print("AP: ");
  tft.println(AP_SSID);

  tft.setCursor(12, 66);
  tft.print("IP: ");
  tft.println(WiFi.softAPIP());

  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setCursor(12, 96);
  tft.print("http://");
  tft.print(WiFi.softAPIP());
  tft.println("/update");

  // "sieht nach was aus": simple Demo-Grafik
  tft.drawRoundRect(8, 130, 304, 60, 10, TFT_DARKGREY);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setCursor(16, 148);
  tft.println("Display OK (ST7796)");

  // Farb-Balken unten
  int y = 210;
  tft.fillRect(0, y,   64, 30, TFT_RED);
  tft.fillRect(64, y,  64, 30, TFT_GREEN);
  tft.fillRect(128, y, 64, 30, TFT_BLUE);
  tft.fillRect(192, y, 64, 30, TFT_YELLOW);
  tft.fillRect(256, y, 64, 30, TFT_MAGENTA);

  // kleiner Rahmen unten
  tft.drawFastHLine(0, 479, 320, TFT_DARKGREY);
}

// ===================== OTA Handlers =====================
void handleRoot() {
  server.send(200, "text/html", FPSTR(OTA_PAGE));
}

void handleUpdate() {
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    // OTA Start
    // WICHTIG: Größe ist bekannt, sonst Update.begin() mit UPDATE_SIZE_UNKNOWN
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      server.send(200, "text/plain", "OK. Rebooting...");
      delay(200);
      ESP.restart();
    } else {
      Update.printError(Serial);
      server.send(500, "text/plain", "FAIL");
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.end();
    server.send(500, "text/plain", "ABORTED");
  }
}

// ===================== Setup/Loop =====================
void setup() {
  Serial.begin(115200);

  // Backlight
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);  // HIGH = an (falls invertiert: LOW)

  // TFT init
  tft.init();
  tft.setRotation(1); // 0/1/2/3 -> 1 = Landscape üblich bei 480x320
  tft.fillScreen(TFT_BLACK);

  // AP starten
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);

  // Webserver
  server.on("/", HTTP_GET, handleRoot);
  server.on("/update", HTTP_GET, handleRoot);
  server.on("/update", HTTP_POST,
            []() { /* Antwort kommt bei UPLOAD_FILE_END */ },
            handleUpdate);

  server.begin();

  drawHomeScreen();
}

void loop() {
  server.handleClient();
}
