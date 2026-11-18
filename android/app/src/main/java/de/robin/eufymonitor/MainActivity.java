package de.robin.eufymonitor;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.database.Cursor;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.OpenableColumns;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.HorizontalScrollView;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import org.json.JSONObject;
import org.json.JSONArray;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.SocketTimeoutException;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.Iterator;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;

/** Eufy RoboVac 15C – read-only GD32 diagnostics via independently powered ESP32-WROOM. */
public class MainActivity extends Activity {
    private static final int FILE_REQUEST = 1001;
    private static final int BG = Color.rgb(10, 16, 27);
    private static final int PANEL = Color.rgb(24, 35, 52);
    private static final int PANEL_2 = Color.rgb(33, 47, 67);
    private static final int FG = Color.rgb(238, 245, 253);
    private static final int SECONDARY = Color.rgb(165, 187, 207);
    private static final int GREEN = Color.rgb(93, 220, 187);
    private static final int AMBER = Color.rgb(250, 194, 102);
    private static final int RED = Color.rgb(251, 130, 131);
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private final AtomicBoolean busy = new AtomicBoolean(false);

    private final String[] tabs = {"Übersicht", "Sensoren", "Diagnose", "Verbindung & OTA"};
    private TextView connectionBanner, heading, subtitle;
    private LinearLayout content, navigation;
    private EditText hostEdit, tokenEdit, ssidEdit, wifiPassEdit;
    private JSONObject current = null;
    private String currentHost = "192.168.4.1";
    private String lastMessage = "Noch keine Telemetrie empfangen";
    private boolean live = true, active = false, advanced = false;
    private int tab = 0;
    private final Runnable ticker = new Runnable() {
        @Override public void run() {
            if (live && tab != 3 && !busy.get()) refresh();
            if (active) handler.postDelayed(this, 1750);
        }
    };

    private int dp(float n) {
        return (int) (getResources().getDisplayMetrics().density * n + .5f);
    }
    private LinearLayout vertical() {
        LinearLayout l = new LinearLayout(this);
        l.setOrientation(LinearLayout.VERTICAL);
        return l;
    }
    private LinearLayout horizontal() {
        LinearLayout l = new LinearLayout(this);
        l.setOrientation(LinearLayout.HORIZONTAL);
        l.setGravity(Gravity.CENTER_VERTICAL);
        return l;
    }
    private TextView text(String value, int sp, int color, boolean bold) {
        TextView t = new TextView(this);
        t.setText(value);
        t.setTextSize(sp);
        t.setTextColor(color);
        t.setGravity(Gravity.CENTER_VERTICAL);
        t.setIncludeFontPadding(true);
        if (bold) t.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        return t;
    }
    private GradientDrawable background(int color, int radius) {
        GradientDrawable g = new GradientDrawable();
        g.setColor(color);
        g.setCornerRadius(dp(radius));
        return g;
    }
    private Button button(String label) {
        Button b = new Button(this);
        b.setAllCaps(false);
        b.setText(label);
        b.setTextColor(FG);
        b.setTextSize(13);
        b.setBackground(background(PANEL_2, 12));
        b.setPadding(dp(12), dp(6), dp(12), dp(6));
        return b;
    }
    private void spaced(LinearLayout parent, View v, int bottom) {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(-1, -2);
        p.bottomMargin = dp(bottom);
        parent.addView(v, p);
    }
    private LinearLayout card(String title) {
        LinearLayout l = vertical();
        l.setPadding(dp(16), dp(15), dp(16), dp(16));
        l.setBackground(background(PANEL, 16));
        if (title != null && !title.isEmpty()) {
            TextView label = text(title, 15, FG, true);
            LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(-1, -2);
            p.bottomMargin = dp(11);
            l.addView(label, p);
        }
        spaced(content, l, 12);
        return l;
    }
    private void line(LinearLayout parent, String name, String value) {
        LinearLayout r = horizontal();
        TextView left = text(name, 13, SECONDARY, false);
        TextView right = text(value, 15, FG, true);
        right.setGravity(Gravity.END | Gravity.CENTER_VERTICAL);
        r.addView(left, new LinearLayout.LayoutParams(0, -2, 1.35f));
        r.addView(right, new LinearLayout.LayoutParams(0, -2, 1f));
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(-1, -2);
        p.topMargin = dp(9);
        parent.addView(r, p);
    }
    private void info(LinearLayout parent, String message) {
        TextView t = text(message, 13, SECONDARY, false);
        t.setLineSpacing(dp(2), 1f);
        spaced(parent, t, 8);
    }
    private String numeric(JSONObject object, String key) {
        if (object == null || object.isNull(key) || !object.has(key)) return "–";
        return String.valueOf(object.opt(key));
    }
    private JSONObject section(String name) {
        return current == null ? null : current.optJSONObject(name);
    }
    private boolean valid() {
        return current != null && current.optBoolean("valid", false)
            && current.optLong("sample_age_ms", -1) >= 0
            && current.optLong("sample_age_ms", -1) < 5000;
    }
    private String value(JSONObject obj, String name) {
        return valid() ? numeric(obj, name) : "–";
    }
    private String raw(JSONObject obj, String name) {
        String val = value(obj, name);
        return "–".equals(val) ? val : val + " raw";
    }

    @Override public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        currentHost = getPreferences(MODE_PRIVATE).getString("host", "192.168.4.1");
        LinearLayout root = vertical();
        root.setBackgroundColor(BG);
        root.setPadding(dp(16), dp(14), dp(16), dp(10));
        setContentView(root);

        TextView brand = text("EUFY 15C", 26, FG, true);
        root.addView(brand);
        TextView details = text("ROBOTER-DIAGNOSE  •  ESP32 / GD32", 11, SECONDARY, true);
        spaced(root, details, 12);

        connectionBanner = text(lastMessage, 12, AMBER, true);
        connectionBanner.setPadding(dp(12), dp(10), dp(12), dp(10));
        connectionBanner.setBackground(background(PANEL, 11));
        spaced(root, connectionBanner, 12);

        HorizontalScrollView scrollTabs = new HorizontalScrollView(this);
        scrollTabs.setHorizontalScrollBarEnabled(false);
        navigation = horizontal();
        scrollTabs.addView(navigation);
        spaced(root, scrollTabs, 12);

        heading = text("", 22, FG, true);
        spaced(root, heading, 1);
        subtitle = text("", 12, SECONDARY, false);
        spaced(root, subtitle, 14);

        ScrollView mainScroll = new ScrollView(this);
        mainScroll.setFillViewport(true);
        content = vertical();
        content.setPadding(0, 0, 0, dp(22));
        mainScroll.addView(content);
        root.addView(mainScroll, new LinearLayout.LayoutParams(-1, 0, 1f));
        openTab(0);
    }
    private void updateBanner(String message, boolean good) {
        lastMessage = message;
        if (connectionBanner != null) {
            connectionBanner.setText(message);
            connectionBanner.setTextColor(good ? GREEN : AMBER);
        }
    }
    private void openTab(int index) {
        tab = index;
        navigation.removeAllViews();
        for (int i = 0; i < tabs.length; i++) {
            final int selected = i;
            Button b = button(tabs[i]);
            b.setBackground(background(i == tab ? GREEN : PANEL_2, 11));
            b.setTextColor(i == tab ? BG : FG);
            LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(-2, dp(43));
            p.rightMargin = dp(7);
            navigation.addView(b, p);
            b.setOnClickListener(v -> openTab(selected));
        }
        content.removeAllViews();
        String[] names = {"Auf einen Blick", "Sensoren & Antrieb", "Technische Diagnose", "WLAN & Firmware"};
        String[] descriptions = {
            "Die wichtigsten Werte ohne Registeradressen",
            "Unkalibrierte Rohwerte der Originalsteuerung",
            "SWD, Datenqualität und erweiterte Rohdaten",
            "ESP-Adresse, Heimnetz und ESP32-OTA"
        };
        heading.setText(names[tab]);
        subtitle.setText(descriptions[tab]);
        if (tab == 0) overview();
        if (tab == 1) sensors();
        if (tab == 2) diagnostics();
        if (tab == 3) settings();
    }
    private void overview() {
        LinearLayout state = card("Verbindungsstatus");
        if (current == null) {
            info(state, "Noch keine Messwerte. Die App versucht, den ESP32 über WLAN zu erreichen.");
        } else if (valid()) {
            TextView ok = text("SWD-Daten verfügbar", 20, GREEN, true);
            spaced(state, ok, 7);
            line(state, "Letzte Messung", numeric(current, "sample_age_ms") + " ms");
        } else {
            TextView bad = text("Keine gültigen SWD-Daten", 19, AMBER, true);
            spaced(state, bad, 6);
            info(state, "Der ESP32 kann erreichbar sein, auch wenn der GD32 nicht ausgelesen wird.");
            line(state, "Diagnosestatus", current.optString("status", "unbekannt"));
        }
        if (current != null && current.optBoolean("openocd_connected", false))
            info(state, "OpenOCD greift gerade auf SWD zu. Die interne Telemetrie pausiert.");

        LinearLayout battery = card("Akku & Laden");
        JSONObject b = section("battery");
        if (valid() && b != null && b.optInt("percent_reported", -1) >= 0
                && b.optInt("percent_reported", -1) <= 100) {
            int pct = b.optInt("percent_reported");
            TextView big = text(pct + " %", 30, GREEN, true);
            spaced(battery, big, 8);
            ProgressBar p = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
            p.setMax(100);
            p.setProgress(pct);
            p.setProgressTintList(android.content.res.ColorStateList.valueOf(GREEN));
            spaced(battery, p, 9);
            info(battery, "Ladestand laut Original-Firmware. Kein selbst berechneter Messwert.");
        } else info(battery, "Ladestand derzeit nicht verfügbar.");
        line(battery, "Akku-ADC", raw(b, "adc"));
        line(battery, "Lade-ADC", raw(b, "charge_adc"));

        LinearLayout movement = card("Fahrwerk");
        JSONObject wheel = section("wheel");
        line(movement, "Rad links", value(wheel, "left_ticks") + " Ticks");
        line(movement, "Rad rechts", value(wheel, "right_ticks") + " Ticks");
        JSONObject st = section("state");
        line(movement, "Fahrmodus-Code", value(st, "run_model"));
        info(movement, "Die Codebedeutung ist noch nicht bestätigt. Die Ticks sind keine Strecke in Metern.");

        LinearLayout sensors = card("Hindernissensoren");
        JSONObject obs = section("obstacle_adc");
        line(sensors, "Links", raw(obs, "left"));
        line(sensors, "Vorne", raw(obs, "front"));
        line(sensors, "Rechts", raw(obs, "right"));
        info(sensors, "ADC-Rohwerte: keine bestätigten Abstandswerte.");
    }
    private void sensors() {
        if (!valid()) {
            LinearLayout missing = card("Keine aktuellen Messwerte");
            info(missing, "Für die Anzeige werden gültige SWD-Samples benötigt. Unter „Diagnose“ siehst du, ob der ESP32 oder der GD32 die Ursache ist.");
        }
        LinearLayout obstacle = card("Hinderniserkennung");
        JSONObject o = section("obstacle_adc");
        line(obstacle, "Links", raw(o, "left"));
        line(obstacle, "Vorne", raw(o, "front"));
        line(obstacle, "Rechts", raw(o, "right"));
        info(obstacle, "Der Zahlenwert ist ein ADC-Rohwert, keine Entfernung in cm.");

        LinearLayout cliff = card("Absturzsensoren");
        JSONObject c = section("cliff_adc");
        line(cliff, "Links", raw(c, "left"));
        line(cliff, "Vorne", raw(c, "front"));
        line(cliff, "Rechts", raw(c, "right"));
        info(cliff, "Noch keine Schaltschwellen oder Abgrund-Erkennung bestätigt.");

        LinearLayout wheels = card("Radencoder & Motoren");
        JSONObject w = section("wheel");
        line(wheels, "Encoder links", value(w, "left_ticks") + " Ticks");
        line(wheels, "Encoder rechts", value(w, "right_ticks") + " Ticks");
        line(wheels, "Links / Messfenster", value(w, "left_window"));
        line(wheels, "Rechts / Messfenster", value(w, "right_window"));
        line(wheels, "Motorstrom links", raw(w, "left_current_adc"));
        line(wheels, "Motorstrom rechts", raw(w, "right_current_adc"));
        info(wheels, "Motorstrom: ADC-Zähler, nicht Ampere. Die Drehrichtung muss separat validiert werden.");

        LinearLayout brushes = card("Bürsten");
        JSONObject b = section("brush_adc");
        line(brushes, "Hauptbürste", raw(b, "main"));
        line(brushes, "Links", raw(b, "left"));
        line(brushes, "Rechts", raw(b, "right"));
        JSONObject pwm = section("pwm_raw");
        line(brushes, "Bürsten-PWM", raw(pwm, "brush"));

        LinearLayout bumper = card("Stoßsensoren");
        JSONObject bmr = section("bumper");
        line(bumper, "Wandkontakt-Zähler", value(bmr, "wall_bump_count_raw"));
        info(bumper, "Einzelne Bumper-Kanäle sind noch nicht verifiziert. Keine erfundenen Links-/Rechts-Zustände.");
    }
    private void diagnostics() {
        LinearLayout health = card("SWD / Datenqualität");
        if (current == null) {
            info(health, "Telemetrie noch nicht empfangen.");
        } else {
            line(health, "Status", current.optString("status", "–"));
            line(health, "Gültiger Datensatz", current.optBoolean("valid", false) ? "Ja" : "Nein");
            line(health, "Alter", numeric(current, "sample_age_ms") + " ms");
            line(health, "Fehlerzähler", numeric(current, "errors"));
            line(health, "SWD-ID (DPIDR)", current.optString("dpidr", "–"));
            line(health, "OpenOCD verbunden", current.optBoolean("openocd_connected", false) ? "Ja" : "Nein");
            JSONObject diag = section("diagnostics");
            line(health, "OEM-Diagnosedaten", diag != null && diag.optBoolean("valid", false) ? "Gültig" : "Nicht bestätigt");
            JSONObject gpio = section("gpio_inputs");
            line(health, "GPIO-Abfrage", gpio != null && gpio.optBoolean("valid", false) ? "Gültig" : "Nicht bestätigt");
        }
        LinearLayout commands = card("Erreichbarkeit prüfen");
        Button checkHttp = button("HTTP-Server /health");
        spaced(commands, checkHttp, 9);
        Button checkOta = button("OTA-Server /status");
        spaced(commands, checkOta, 6);
        checkHttp.setOnClickListener(v -> checkEndpoint(8080, "/health", "Telemetrie-Server"));
        checkOta.setOnClickListener(v -> checkEndpoint(8081, "/status", "OTA-Server"));

        LinearLayout ext = card("Erweiterte Diagnose");
        info(ext, "Hier stehen unveränderte Registerwerte. Die Einheiten und Bitbedeutungen sind teilweise unbekannt.");
        Button toggle = button(advanced ? "Rohdaten verbergen" : "Alle Rohdaten anzeigen");
        spaced(ext, toggle, 7);
        toggle.setOnClickListener(v -> { advanced = !advanced; openTab(2); });
        if (advanced && current != null) {
            rawLines(ext, current, "", 0);
        }
    }
    private void rawLines(LinearLayout parent, JSONObject object, String prefix, int depth) {
        Iterator<String> keys = object.keys();
        while (keys.hasNext()) {
            String key = keys.next();
            Object val = object.opt(key);
            String label = prefix.isEmpty() ? key : prefix + "." + key;
            if (val instanceof JSONObject && depth < 4) {
                rawLines(parent, (JSONObject) val, label, depth+1);
            } else if (val instanceof JSONArray) {
                line(parent, label, String.valueOf(val));
            } else {
                line(parent, label, String.valueOf(val));
            }
        }
    }

    private EditText field(String value, String hint, boolean password) {
        EditText e = new EditText(this);
        e.setSingleLine(true);
        e.setTextSize(16);
        e.setTextColor(FG);
        e.setHintTextColor(SECONDARY);
        e.setHint(hint);
        e.setInputType(password
                ? InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD
                : InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD);
        if (value != null) e.setText(value);
        return e;
    }
    private void settings() {
        LinearLayout host = card("ESP32-Adresse");
        info(host, "Im ESP-AP normalerweise 192.168.4.1. Im Heimnetz die vom Router vergebene ESP-IP eintragen.");
        hostEdit = field(currentHost, "ESP32-IP-Adresse", false);
        spaced(host, hostEdit, 10);
        Button saveHost = button("Adresse speichern & verbinden");
        spaced(host, saveHost, 3);
        saveHost.setOnClickListener(v -> {
            currentHost = normalizeHost(hostEdit.getText().toString());
            getPreferences(MODE_PRIVATE).edit().putString("host", currentHost).apply();
            updateBanner("Neue Adresse: " + currentHost, false);
            refresh();
        });

        LinearLayout net = card("ESP ins Heimnetz einbinden");
        info(net, "Benötigt die neue AP+STA-Firmware. Der ESP-AP bleibt als Rückweg bestehen.");
        ssidEdit = field("Peng", "WLAN-Name (2,4 GHz)", false);
        spaced(net, ssidEdit, 7);
        wifiPassEdit = field("", "Passwort des Heim-WLANs", true);
        spaced(net, wifiPassEdit, 8);
        Button wifiStatus = button("WLAN-Status abfragen");
        spaced(net, wifiStatus, 7);
        Button wifiSave = button("WLAN am ESP speichern");
        spaced(net, wifiSave, 7);
        wifiStatus.setOnClickListener(v -> checkEndpoint(8081, "/wifi/status", "ESP-WLAN"));
        wifiSave.setOnClickListener(v -> configureWifi());

        LinearLayout ota = card("ESP32-Firmwareupdate (OTA)");
        info(ota, "Nur eine ESP32 App-only-BIN mit gültiger OTA-Partitionstabelle hochladen. Die GD32-Firmware bleibt unverändert.");
        tokenEdit = field("", "OTA-Token = aktuelles ESP-AP-Passwort", true);
        spaced(ota, tokenEdit, 9);
        info(ota, "Das Token wird nicht in den App-Einstellungen gespeichert.");
        Button otaStatus = button("OTA-Bereitschaft prüfen");
        spaced(ota, otaStatus, 7);
        Button select = button("Firmware-BIN auswählen und installieren");
        spaced(ota, select, 7);
        otaStatus.setOnClickListener(v -> checkEndpoint(8081, "/status", "OTA-Server"));
        select.setOnClickListener(v -> chooseFirmware());

        LinearLayout refresh = card("Live-Anzeige");
        info(refresh, "Die App liest nur. Eine aktive OpenOCD-Sitzung pausiert die automatische SWD-Abfrage.");
        Button toggle = button(live ? "Live-Abfrage: EIN – ausschalten" : "Live-Abfrage: AUS – einschalten");
        spaced(refresh, toggle, 7);
        toggle.setOnClickListener(v -> {
            live = !live;
            toggle.setText(live ? "Live-Abfrage: EIN – ausschalten" : "Live-Abfrage: AUS – einschalten");
            if (live) refresh();
        });
        Button refreshNow = button("Telemetrie jetzt prüfen");
        spaced(refresh, refreshNow, 5);
        refreshNow.setOnClickListener(v -> refresh());
    }
    private static String normalizeHost(String h) {
        h = h.trim().replace("http://", "").replace("https://", "");
        int slash = h.indexOf('/');
        if (slash >= 0) h = h.substring(0, slash);
        int colon = h.indexOf(':');
        if (colon >= 0) h = h.substring(0, colon);
        return h.trim();
    }
    private static byte[] readAll(InputStream in) throws Exception {
        try (InputStream input = in; ByteArrayOutputStream out = new ByteArrayOutputStream()) {
            byte[] buf = new byte[2048];
            int n;
            while ((n = input.read(buf)) != -1) out.write(buf, 0, n);
            return out.toByteArray();
        }
    }
    private static String get(String host, int port, String route) throws Exception {
        HttpURLConnection conn = (HttpURLConnection) new URL("http://" + host + ":" + port + route).openConnection();
        try {
            conn.setConnectTimeout(2500);
            conn.setReadTimeout(3000);
            conn.setUseCaches(false);
            conn.setRequestMethod("GET");
            if (conn.getResponseCode() != 200) throw new Exception("HTTP " + conn.getResponseCode());
            return new String(readAll(conn.getInputStream()), StandardCharsets.UTF_8);
        } finally {
            conn.disconnect();
        }
    }
    private void refresh() {
        if (!busy.compareAndSet(false, true)) return;
        final String ip = currentHost;
        if (ip.isEmpty()) {
            busy.set(false);
            updateBanner("Bitte ESP32-Adresse eingeben", false);
            return;
        }
        worker.execute(() -> {
            try {
                JSONObject data = new JSONObject(get(ip, 8080, "/telemetry"));
                runOnUiThread(() -> {
                    current = data;
                    String state = data.optBoolean("valid", false) ? "SWD verfügbar" :
                            "ESP erreichbar · SWD: " + data.optString("status", "keine Daten");
                    long age = data.optLong("sample_age_ms", -1);
                    updateBanner(state + (age >= 0 ? " · " + age + " ms" : ""),
                            data.optBoolean("valid", false) && age < 5000);
                    if (tab != 3) openTab(tab);
                });
            } catch (Exception problem) {
                String diagnostic;
                try {
                    get(ip, 8080, "/health");
                    diagnostic = "HTTP erreichbar, Telemetrie fehlgeschlagen: " + problem.getMessage();
                } catch (Exception ignored) {
                    try {
                        get(ip, 8081, "/status");
                        diagnostic = "ESP OTA erreichbar · Telemetrie-Server antwortet nicht";
                    } catch (Exception unknown) {
                        diagnostic = "ESP nicht erreichbar: " + problem.getClass().getSimpleName();
                    }
                }
                final String msg = diagnostic;
                runOnUiThread(() -> updateBanner(msg, false));
            } finally {
                busy.set(false);
            }
        });
    }
    private void showResult(String title, String description) {
        runOnUiThread(() -> new AlertDialog.Builder(this).setTitle(title)
                .setMessage(description).setPositiveButton("OK", null).show());
    }
    private void checkEndpoint(int port, String route, String label) {
        if (!busy.compareAndSet(false, true)) {
            updateBanner("Bitte laufende Anfrage beenden", false);
            return;
        }
        final String ip = currentHost;
        worker.execute(() -> {
            try {
                String result = get(ip, port, route);
                JSONObject obj = new JSONObject(result);
                String description;
                if ("/wifi/status".equals(route)) {
                    description = "Modus: " + obj.optString("mode", "unbekannt") +
                            "\nHeimnetz verbunden: " + (obj.optBoolean("sta_connected", false) ? "Ja" : "Nein") +
                            "\nSSID: " + obj.optString("sta_ssid", "–") +
                            "\nIP: " + obj.optString("sta_ip", "–") +
                            "\nESP-AP: " + obj.optString("fallback_ap", "–");
                } else if ("/status".equals(route)) {
                    description = "OTA aktiv: " + obj.optBoolean("ota", false) +
                            "\nAktiv: " + obj.optString("running", "–") +
                            "\nNächste Partition: " + obj.optString("next", "–") +
                            "\nMaximale BIN: " + obj.optLong("next_size", -1) + " Byte";
                } else description = result;
                showResult(label, description);
            } catch (Exception e) {
                final String error = e.getMessage();
                showResult(label, "Keine gültige Antwort: " + error +
                        "\n\nBei /wifi/status: Zuerst AP+STA-Firmware per OTA installieren.");
            } finally {
                busy.set(false);
            }
        });
    }
    private void configureWifi() {
        if (ssidEdit == null || wifiPassEdit == null || tokenEdit == null) return;
        final String ssid = ssidEdit.getText().toString().trim();
        final String password = wifiPassEdit.getText().toString();
        final String token = tokenEdit.getText().toString();
        if (ssid.isEmpty() || ssid.getBytes(StandardCharsets.UTF_8).length > 32
                || password.length() < 8 || password.length() > 63 || token.isEmpty()) {
            showResult("WLAN-Konfiguration", "SSID (1–32 Bytes), Heimnetz-Passwort (8–63 Zeichen) und OTA-Token eingeben.");
            return;
        }
        new AlertDialog.Builder(this)
            .setTitle("ESP32 mit " + ssid + " verbinden")
            .setMessage("Das Passwort wird nur an den ESP32 übertragen und dort im NVS gespeichert. Der ESP-AP bleibt verfügbar.\n\nDas ist keine Einstellung des Handy-WLANs.")
            .setNegativeButton("Abbrechen", null)
            .setPositiveButton("Speichern", (d, w) -> {
                if (!busy.compareAndSet(false, true)) return;
                final String ip = currentHost;
                worker.execute(() -> {
                    HttpURLConnection c = null;
                    try {
                        c = (HttpURLConnection) new URL("http://" + ip + ":8081/wifi/config").openConnection();
                        c.setRequestMethod("POST");
                        c.setConnectTimeout(4000);
                        c.setReadTimeout(9000);
                        c.setRequestProperty("X-OTA-Token", token);
                        c.setRequestProperty("X-STA-SSID", ssid);
                        c.setRequestProperty("X-STA-PASSWORD", password);
                        c.setDoOutput(true);
                        c.setFixedLengthStreamingMode(0);
                        try (OutputStream out = c.getOutputStream()) { out.flush(); }
                        int code = c.getResponseCode();
                        if (code != 200) throw new Exception("HTTP " + code);
                        final String info = new String(readAll(c.getInputStream()), StandardCharsets.UTF_8);
                        showResult("WLAN-Konfiguration angenommen",
                            "ESP32 speichert die Daten und versucht, sich zu verbinden.\n" +
                            "Bitte danach „WLAN-Status abfragen“ drücken.\n\n" + info);
                    } catch (Exception e) {
                        showResult("WLAN-Konfiguration fehlgeschlagen",
                                "HTTP-Anfrage nicht bestätigt: " + e.getMessage() +
                                "\nKeine Aussage über eine erfolgreiche Verbindung möglich.");
                    } finally {
                        if (c != null) c.disconnect();
                        busy.set(false);
                    }
                });
            }).show();
    }
    private void chooseFirmware() {
        if (tokenEdit == null || tokenEdit.getText().toString().isEmpty()) {
            showResult("OTA-Token fehlt", "Unter „Verbindung & OTA“ das aktuelle ESP-AP-Passwort eintragen.");
            return;
        }
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.setType("*/*");
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        startActivityForResult(intent, FILE_REQUEST);
    }
    @Override protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != FILE_REQUEST || resultCode != RESULT_OK || data == null) return;
        Uri uri = data.getData();
        if (uri == null) return;
        new AlertDialog.Builder(this)
            .setTitle("ESP32-OTA installieren")
            .setMessage("Nur die Eufy-WROOM-OTA-app-only.bin verwenden, keine Full-Flash-BIN. Die ESP32-Partition wird aktualisiert; der GD32 wird nicht beschrieben.")
            .setNegativeButton("Abbrechen", null)
            .setPositiveButton("OTA starten", (d, w) -> upload(uri))
            .show();
    }
    private void upload(Uri uri) {
        if (!busy.compareAndSet(false, true)) return;
        final String ip = currentHost;
        final String token = tokenEdit.getText().toString();
        live = false;
        updateBanner("OTA-Upload läuft – App geöffnet lassen", false);
        worker.execute(() -> {
            HttpURLConnection c = null;
            try {
                long size = -1;
                try (Cursor cursor = getContentResolver().query(uri,
                        new String[] {OpenableColumns.SIZE}, null, null, null)) {
                    if (cursor != null && cursor.moveToFirst() && !cursor.isNull(0))
                        size = cursor.getLong(0);
                }
                if (size < 8192 || size > 0x160000)
                    throw new Exception("App-BIN-Größe ungültig: " + size);
                try (InputStream check = getContentResolver().openInputStream(uri)) {
                    if (check == null || check.read() != 0xE9)
                        throw new Exception("Kein ESP32-Image (Header 0xE9 fehlt)");
                }
                c = (HttpURLConnection) new URL("http://" + ip + ":8081/upload").openConnection();
                c.setRequestMethod("POST");
                c.setConnectTimeout(4500);
                c.setReadTimeout(180000);
                c.setRequestProperty("Content-Type", "application/octet-stream");
                c.setRequestProperty("X-OTA-Token", token);
                c.setFixedLengthStreamingMode((int)size);
                c.setDoOutput(true);
                try (InputStream input = getContentResolver().openInputStream(uri);
                     OutputStream output = c.getOutputStream()) {
                    if (input == null) throw new Exception("Datei nicht lesbar");
                    byte[] buf = new byte[4096];
                    int n;
                    long total = 0, last = 0;
                    while ((n = input.read(buf)) != -1) {
                        output.write(buf, 0, n);
                        total += n;
                        if (total - last > 50000) {
                            int percent = (int)(total * 100 / size);
                            runOnUiThread(() -> updateBanner("Firmware übertragen: " + percent + " %", false));
                            last = total;
                        }
                    }
                    if (total != size) throw new Exception("Abweichende Dateigröße");
                }
                int code = c.getResponseCode();
                if (code != 200) throw new Exception("HTTP " + code);
                JSONObject result = new JSONObject(new String(readAll(c.getInputStream()), StandardCharsets.UTF_8));
                if (!"accepted".equals(result.optString("result")))
                    throw new Exception("ESP-OTA nicht bestätigt");
                runOnUiThread(() -> updateBanner("OTA angenommen – ESP32 startet neu. WLAN-Verbindung prüfen.", true));
                showResult("OTA bestätigt", "Die neue ESP32-App wurde angenommen. ESP32 startet neu.\n\nDanach /wifi/status unter „Verbindung & OTA“ prüfen. Der AP kann ein neues Passwort haben.");
            } catch (Exception e) {
                final String error = e.getMessage();
                runOnUiThread(() -> updateBanner("OTA nicht bestätigt: " + error, false));
            } finally {
                if (c != null) c.disconnect();
                busy.set(false);
            }
        });
    }

    @Override protected void onStart() {
        super.onStart();
        active = true;
        handler.post(ticker);
    }
    @Override protected void onStop() {
        active = false;
        handler.removeCallbacks(ticker);
        super.onStop();
    }
    @Override protected void onDestroy() {
        worker.shutdownNow();
        super.onDestroy();
    }
}
