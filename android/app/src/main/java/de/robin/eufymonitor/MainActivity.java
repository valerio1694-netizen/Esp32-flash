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
import java.io.DataInputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.Socket;
import java.net.InetSocketAddress;
import java.net.SocketTimeoutException;
import java.net.URL;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.NetworkInterface;
import java.util.Enumeration;
import java.util.concurrent.ExecutorCompletionService;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;
import java.nio.charset.StandardCharsets;
import java.util.Iterator;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;

/** Eufy RoboVac 15C – read-only GD32 diagnostics via independently powered ESP32-WROOM. */
public class MainActivity extends Activity {
    private static final int FILE_REQUEST = 1001;
    private static final int BG = Color.rgb(9, 15, 27);
    private static final int PANEL = Color.rgb(21, 33, 52);
    private static final int PANEL_2 = Color.rgb(32, 48, 71);
    private static final int FG = Color.rgb(238, 245, 253);
    private static final int SECONDARY = Color.rgb(165, 187, 207);
    private static final int GREEN = Color.rgb(87, 223, 191);
    private static final int AMBER = Color.rgb(250, 194, 102);
    private static final int RED = Color.rgb(251, 130, 131);
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private final AtomicBoolean busy = new AtomicBoolean(false);

    private final String[] tabs = {"Übersicht", "Sensoren", "Diagnose", "Einstellungen"};
    private TextView connectionBanner, heading, subtitle;
    private LinearLayout content, navigation;
    private ScrollView mainScroll;
    private Button settingsButton;
    private EditText hostEdit, tokenEdit, ssidEdit, wifiPassEdit;
    private JSONObject current = null;
    private String currentHost = "192.168.4.1";
    private String lastMessage = "Noch keine Telemetrie empfangen";
    private boolean live = true, active = false, advanced = false;
    private int tab = 0;
    private volatile long lastDiscoveryMs = 0;
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

    private String appVersion() {
        try {
            return getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
        } catch (Exception e) {
            return "?";
        }
    }

    @Override public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        currentHost = getPreferences(MODE_PRIVATE).getString("host", "192.168.4.1");

        LinearLayout root = vertical();
        root.setBackgroundColor(BG);
        root.setPadding(dp(18), dp(12), dp(18), dp(8));
        setContentView(root);

        LinearLayout header = horizontal();
        LinearLayout brandCol = vertical();
        TextView brand = text("EUFY  /  15C", 25, FG, true);
        brandCol.addView(brand);
        TextView edition = text("DIAGNOSE-CENTER    v" + appVersion(), 11, SECONDARY, true);
        spaced(brandCol, edition, 1);
        header.addView(brandCol, new LinearLayout.LayoutParams(0, -2, 1f));
        settingsButton = button("⚙");
        settingsButton.setTextSize(23);
        settingsButton.setBackground(background(PANEL, 13));
        header.addView(settingsButton, new LinearLayout.LayoutParams(dp(56), dp(49)));
        settingsButton.setOnClickListener(v -> openTab(3));
        spaced(root, header, 18);

        connectionBanner = text(lastMessage, 13, AMBER, true);
        connectionBanner.setPadding(dp(15), dp(12), dp(15), dp(12));
        connectionBanner.setBackground(background(PANEL, 13));
        spaced(root, connectionBanner, 16);

        heading = text("", 22, FG, true);
        spaced(root, heading, 4);
        subtitle = text("", 12, SECONDARY, false);
        spaced(root, subtitle, 16);

        mainScroll = new ScrollView(this);
        mainScroll.setFillViewport(true);
        mainScroll.setVerticalScrollBarEnabled(false);
        content = vertical();
        content.setPadding(0, dp(2), 0, dp(20));
        mainScroll.addView(content);
        root.addView(mainScroll, new LinearLayout.LayoutParams(-1, 0, 1f));

        navigation = horizontal();
        navigation.setPadding(dp(4), dp(8), dp(4), dp(4));
        navigation.setBackground(background(PANEL, 16));
        root.addView(navigation, new LinearLayout.LayoutParams(-1, dp(67)));

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
        String[] navLabels = {"⌂  Start", "◉  Sensoren", "▤  Diagnose"};
        for (int i = 0; i < 3; i++) {
            final int choice = i;
            Button b = button(navLabels[i]);
            b.setTextSize(12);
            b.setTextColor(i == index ? BG : SECONDARY);
            b.setBackground(background(i == index ? GREEN : PANEL, 12));
            LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(0, dp(47), 1f);
            p.setMargins(dp(2), 0, dp(2), 0);
            navigation.addView(b, p);
            b.setOnClickListener(v -> openTab(choice));
        }
        if (settingsButton != null)
            settingsButton.setTextColor(index == 3 ? GREEN : FG);
        String[] titles = {"Dein Roboter", "Sensoren", "Diagnose", "Einstellungen"};
        String[] sub = {
            "Live-Übersicht  ·  Daten direkt vom ESP32",
            "Messwerte der originalen Steuerung",
            "Technische Details & Fehleranalyse",
            "WLAN, Verbindung und Firmwareupdate"
        };
        heading.setText(titles[index]);
        subtitle.setText(sub[index]);
        if (mainScroll != null) mainScroll.scrollTo(0, 0);
        renderContent();
    }

    private void renderContent() {
        if (content == null) return;
        int y = mainScroll != null ? mainScroll.getScrollY() : 0;
        content.removeAllViews();
        if (tab == 0) overview();
        if (tab == 1) sensors();
        if (tab == 2) diagnostics();
        if (tab == 3) settings();
        if (mainScroll != null) mainScroll.post(() -> mainScroll.scrollTo(0, y));
    }

    private LinearLayout statTile(String heading, String amount, String foot) {
        LinearLayout tile = vertical();
        tile.setPadding(dp(13), dp(14), dp(12), dp(12));
        tile.setBackground(background(PANEL_2, 13));
        TextView label = text(heading.toUpperCase(Locale.ROOT), 11, SECONDARY, true);
        spaced(tile, label, 9);
        TextView number = text(amount, 22, FG, true);
        spaced(tile, number, 4);
        tile.addView(text(foot, 11, SECONDARY, false));
        return tile;
    }

    private void overview() {
        LinearLayout hero = card(null);
        TextView overline = text("LIVE-VERBINDUNG", 11, SECONDARY, true);
        spaced(hero, overline, 11);
        boolean alive = valid();
        TextView state = text(alive ? "●  Verbunden" : "●  Keine Live-Daten",
                24, alive ? GREEN : AMBER, true);
        spaced(hero, state, 9);
        info(hero, alive ?
                "GD32 über ESP32-SWD erreichbar. Letzte Messung: " +
                numeric(current, "sample_age_ms") + " ms." :
                "ESP32-Verbindung oder SWD-Messung noch nicht bestätigt.");
        if (current != null && !alive)
            line(hero, "Diagnosecode", current.optString("status", "nicht verfügbar"));
        if (current != null && current.optBoolean("openocd_connected", false))
            info(hero, "OpenOCD nutzt SWD gerade exklusiv.");

        LinearLayout battery = card("AKKU");
        JSONObject b = section("battery");
        int charge = valid() && b != null ? b.optInt("percent_reported", -1) : -1;
        if (charge > 100 || charge < 0) charge = -1;
        TextView amount = text(charge >= 0 ? (charge + " %") : "— %", 39, FG, true);
        spaced(battery, amount, 10);
        ProgressBar bar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(100);
        bar.setProgress(charge >= 0 ? charge : 0);
        bar.setProgressTintList(android.content.res.ColorStateList.valueOf(GREEN));
        bar.setProgressBackgroundTintList(android.content.res.ColorStateList.valueOf(PANEL_2));
        spaced(battery, bar, 9);
        info(battery, "Ladestand laut Original-Firmware");
        line(battery, "Batterie ADC", raw(b, "adc"));
        line(battery, "Laden ADC", raw(b, "charge_adc"));

        LinearLayout drive = card("RADENCODER");
        JSONObject wheel = section("wheel");
        LinearLayout wheelRow = horizontal();
        LinearLayout l = statTile("Links", value(wheel, "left_ticks"), "Encoder-Ticks");
        LinearLayout r = statTile("Rechts", value(wheel, "right_ticks"), "Encoder-Ticks");
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, -2, 1f);
        lp.rightMargin = dp(8);
        wheelRow.addView(l, lp);
        wheelRow.addView(r, new LinearLayout.LayoutParams(0, -2, 1f));
        spaced(drive, wheelRow, 12);
        line(drive, "Fahrmodus (OEM-Code)", value(section("state"), "run_model"));
        info(drive, "Keine berechnete Strecke: Encoder-Skalierung noch nicht kalibriert.");

        LinearLayout sensorsCard = card("HINDERNISSENSOREN");
        JSONObject obs = section("obstacle_adc");
        LinearLayout row = horizontal();
        LinearLayout.LayoutParams a = new LinearLayout.LayoutParams(0, -2, 1f);
        a.rightMargin = dp(5);
        row.addView(statTile("Links", value(obs, "left"), "ADC"), a);
        LinearLayout.LayoutParams m = new LinearLayout.LayoutParams(0, -2, 1f);
        m.rightMargin = dp(5);
        row.addView(statTile("Vorne", value(obs, "front"), "ADC"), m);
        row.addView(statTile("Rechts", value(obs, "right"), "ADC"),
                new LinearLayout.LayoutParams(0, -2, 1f));
        spaced(sensorsCard, row, 12);
        info(sensorsCard, "Rohwerte, noch keine kalibrierten Abstände.");

        Button more = button("Alle Sensoren ansehen");
        spaced(sensorsCard, more, 2);
        more.setOnClickListener(v -> openTab(1));
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

        LinearLayout swdCheck = card("Direkte SWD-Prüfung");
        info(swdCheck, "Prüft den CMSIS-DAP-TCP-Zugang (Port 4441) und liest nur die Debug-Port-ID des GD32. Kein Reset, kein Flash, kein Anhalten der CPU.");
        Button testSwd = button("SWD jetzt direkt prüfen");
        spaced(swdCheck, testSwd, 3);
        testSwd.setOnClickListener(v -> checkSwdDirect());

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
        spaced(host, saveHost, 7);
        Button findHost = button("ESP automatisch finden");
        spaced(host, findHost, 3);
        findHost.setOnClickListener(v -> discoverFromButton());
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
    // Passive signature check on the ESP OTA/status endpoint.
    private static boolean isEufy(String host) {
        HttpURLConnection c = null;
        try {
            c = (HttpURLConnection)new URL("http://" + host + ":8081/status").openConnection();
            c.setConnectTimeout(550);
            c.setReadTimeout(750);
            c.setUseCaches(false);
            if (c.getResponseCode() != 200) return false;
            JSONObject j = new JSONObject(new String(readAll(c.getInputStream()), StandardCharsets.UTF_8));
            return j.optBoolean("ota", false)
                && "/upload".equals(j.optString("upload_path", ""))
                && "X-OTA-Token".equals(j.optString("auth", ""));
        } catch (Exception ignored) {
            return false;
        } finally {
            if (c != null) c.disconnect();
        }
    }
    // Current Wi-Fi interface subnet; do not scan cellular, VPN or the internet.
    private String wifiPrefix() {
        try {
            NetworkInterface wlan = NetworkInterface.getByName("wlan0");
            if (wlan == null || !wlan.isUp()) return null;
            Enumeration<InetAddress> ips = wlan.getInetAddresses();
            while (ips.hasMoreElements()) {
                InetAddress a = ips.nextElement();
                if (a instanceof Inet4Address && a.isSiteLocalAddress()) {
                    String ip = a.getHostAddress();
                    return ip.substring(0, ip.lastIndexOf('.') + 1);
                }
            }
        } catch (Exception ignored) {}
        return null;
    }
    private String findEspOnWifi() {
        String prefix = wifiPrefix();
        if (prefix == null) return null;
        if (currentHost.startsWith(prefix) && isEufy(currentHost)) return currentHost;
        ExecutorService pool = Executors.newFixedThreadPool(20);
        ExecutorCompletionService<String> results = new ExecutorCompletionService<>(pool);
        int count = 0;
        try {
            for (int i = 1; i <= 254; i++) {
                final String candidate = prefix + i;
                if (candidate.equals(currentHost)) continue;
                results.submit(() -> isEufy(candidate) ? candidate : null);
                count++;
            }
            long deadline = System.currentTimeMillis() + 11000;
            for (int i = 0; i < count; i++) {
                long remain = deadline - System.currentTimeMillis();
                if (remain <= 0) break;
                Future<String> done = results.poll(remain, TimeUnit.MILLISECONDS);
                if (done == null) break;
                String found = done.get();
                if (found != null) return found;
            }
        } catch (Exception ignored) { 
            // A disconnected Wi-Fi network yields no result; never change settings.
        } finally {
            pool.shutdownNow();
        }
        return null;
    }
    private void rememberHost(String ip) {
        currentHost = ip;
        getPreferences(MODE_PRIVATE).edit().putString("host", ip).apply();
        if (hostEdit != null) hostEdit.setText(ip);
    }
    private void discoverFromButton() {
        if (!busy.compareAndSet(false, true)) return;
        updateBanner("Suche ESP im aktuellen WLAN …", false);
        worker.execute(() -> {
            try {
                String found = findEspOnWifi();
                runOnUiThread(() -> {
                    if (found == null) updateBanner("ESP im aktuellen WLAN nicht gefunden", false);
                    else {
                        rememberHost(found);
                        updateBanner("ESP gefunden: " + found, true);
                    }
                });
            } finally { busy.set(false); }
        });
    }

    private void refresh() {
        if (!busy.compareAndSet(false, true)) return;
        final String ip = currentHost;
        worker.execute(() -> {
            try {
                String selectedIp = ip;
                JSONObject data;
                try {
                    data = new JSONObject(get(ip, 8080, "/telemetry"));
                } catch (Exception initial) {
                    boolean espAtOldAddress = isEufy(ip);
                    if (espAtOldAddress) {
                        throw new Exception("ESP-OTA erreichbar, Telemetrie antwortet nicht");
                    }
                    long now = System.currentTimeMillis();
                    if (now - lastDiscoveryMs < 60000) throw initial;
                    lastDiscoveryMs = now;
                    runOnUiThread(() -> updateBanner("Suche ESP automatisch im WLAN …", false));
                    String found = findEspOnWifi();
                    if (found == null) throw new Exception("ESP im aktuellen WLAN nicht gefunden");
                    selectedIp = found;
                    final String discovered = found;
                    runOnUiThread(() -> rememberHost(discovered));
                    try {
                        data = new JSONObject(get(found, 8080, "/telemetry"));
                    } catch (Exception noTelemetry) {
                        throw new Exception("ESP " + found + " gefunden, aber Telemetrie antwortet nicht");
                    }
                }
                final JSONObject result = data;
                final String address = selectedIp;
                runOnUiThread(() -> {
                    current = result;
                    boolean good = result.optBoolean("valid", false)
                            && result.optLong("sample_age_ms", -1) < 5000;
                    updateBanner("ESP " + address + " · " +
                        (good ? "SWD-Daten verfügbar" : "SWD-Daten nicht gültig"), good);
                    if (tab != 3) renderContent();
                });
            } catch (Exception problem) {
                final String message = problem.getMessage();
                runOnUiThread(() -> updateBanner(message, false));
            } finally {
                busy.set(false);
            }
        });
    }

    // CMSIS-DAP TCP framed protocol implemented by the pinned upstream firmware.
    // Header: "DAP\0", uint16 payload length LE, type 1=request, type 2=response, reserved=0.
    // Only safe CMSIS-DAP commands: Connect, Clock, TransferConfig, SWDConfig,
    // SWJ line reset/mode switch, and DP IDCODE read (DP address 0).
    // Never HALT, RESET, write the target, or access AP memory.
    private static byte[] dapTcpCommand(DataInputStream input, OutputStream output,
            byte... command) throws Exception {
        if (command.length < 1 || command.length > 1024)
            throw new Exception("Ungültige CMSIS-DAP-Befehlslänge");
        byte[] header = new byte[] {
            'D', 'A', 'P', 0, (byte) command.length,
            (byte) (command.length >> 8), 1, 0
        };
        output.write(header);
        output.write(command);
        output.flush();

        byte[] responseHeader = new byte[8];
        input.readFully(responseHeader);
        if (responseHeader[0] != 'D' || responseHeader[1] != 'A' ||
                responseHeader[2] != 'P' || responseHeader[3] != 0 ||
                responseHeader[6] != 2) {
            throw new Exception("CMSIS-DAP: Ungültiger TCP-Antwortheader");
        }
        int size = (responseHeader[4] & 0xff) | ((responseHeader[5] & 0xff) << 8);
        if (size < 2 || size > 1024)
            throw new Exception("CMSIS-DAP: Ungültige Antwortlänge " + size);
        byte[] response = new byte[size];
        input.readFully(response);
        if (response[0] != command[0])
            throw new Exception("CMSIS-DAP: Falsche Befehlsantwort");
        return response;
    }

    private static String swdAckName(int ack) {
        switch (ack) {
            case 1: return "OK";
            case 2: return "WAIT (Ziel antwortet noch nicht)";
            case 4: return "FAULT (SWD-Zugriff abgelehnt)";
            case 7: return "NO-ACK (keine gültige SWD-Antwort)";
            default: return "0x" + Integer.toHexString(ack);
        }
    }

    private static String checkSwdCommands(String host) throws Exception {
        try (Socket socket = new Socket()) {
            socket.connect(new InetSocketAddress(host, 4441), 3500);
            socket.setSoTimeout(4000);
            socket.setTcpNoDelay(true);
            DataInputStream input = new DataInputStream(socket.getInputStream());
            OutputStream output = socket.getOutputStream();
            StringBuilder message = new StringBuilder();
            message.append("ESP32-CMSIS-DAP (Port 4441): erreichbar\n");
            try {
                byte[] connect = dapTcpCommand(input, output, (byte) 0x02, (byte) 0x01);
                if (connect.length < 2 || connect[1] != 1) {
                    return message + "DAP_Connect: FEHLER, Antwortcode " +
                            (connect.length >= 2 ? (connect[1] & 0xff) : -1);
                }
                message.append("SWD-Protokoll aktiviert: Ja\n");

                final int[] clocks = {100000, 25000};
                for (int hz : clocks) {
                    byte[] clock = dapTcpCommand(input, output, (byte) 0x11,
                        (byte) hz, (byte)(hz >>> 8), (byte)(hz >>> 16), (byte)(hz >>> 24));
                    if (clock.length < 2 || clock[1] != 0)
                        return message + "SWJ_Clock: FEHLER";

                    byte[] transferCfg = dapTcpCommand(input, output,
                        (byte) 0x04, (byte)0, (byte)100, (byte)0, (byte)0, (byte)0);
                    if (transferCfg.length < 2 || transferCfg[1] != 0)
                        return message + "Transfer-Konfiguration: FEHLER";

                    byte[] swdCfg = dapTcpCommand(input, output, (byte) 0x13, (byte)0);
                    if (swdCfg.length < 2 || swdCfg[1] != 0)
                        return message + "SWD-Konfiguration: FEHLER";

                    byte[][] seq = new byte[][] {
                        {(byte)0x12, 56, -1, -1, -1, -1, -1, -1, -1},
                        {(byte)0x12, 16, (byte)0x9e, (byte)0xe7},
                        {(byte)0x12, 56, -1, -1, -1, -1, -1, -1, -1},
                        {(byte)0x12, 8, 0},
                    };
                    for (byte[] cmd : seq) {
                        byte[] answer = dapTcpCommand(input, output, cmd);
                        if (answer.length < 2 || answer[1] != 0)
                            return message + "SWJ-Sequenz: FEHLER";
                    }

                    // CMSIS-DAP_Transfer: index=0, count=1,
                    // request=0x02: DP read, address 0 (DPIDR). No writes.
                    byte[] answer = dapTcpCommand(input, output,
                        (byte)0x05, (byte)0, (byte)1, (byte)0x02);
                    if (answer.length < 3)
                        return message + "DPIDR: ungültige CMSIS-DAP-Antwort";
                    int ack = answer[2] & 0x07;
                    message.append("SWD ").append(hz / 1000).append(" kHz: ")
                        .append(swdAckName(ack)).append("\n");
                    if (ack == 1 && answer.length >= 7) {
                        long id = (answer[3] & 0xffL) | ((answer[4] & 0xffL) << 8)
                            | ((answer[5] & 0xffL) << 16) | ((answer[6] & 0xffL) << 24);
                        message.append(String.format(Locale.ROOT,
                            "Gelesene DPIDR: 0x%08X\n", id));
                        message.append(id == 0x1ba01477L
                            ? "ARM-Debug-Port erkannt. Die Verbindung funktioniert.\n"
                            : "DPIDR weicht vom erwarteten Wert 0x1BA01477 ab.\n");
                        return message.toString();
                    }
                }
                message.append("Keine gültige Debug-Port-ID empfangen.\n");
                message.append("Das grenzt das Problem auf SWD-Signale, Zielversorgung oder SWD-Protokoll ein; Ursache nicht belegt.");
                return message.toString();
            } finally {
                try { dapTcpCommand(input, output, (byte) 0x03); }
                catch (Exception ignored) { }
            }
        }
    }

    private void checkSwdDirect() {
        if (!busy.compareAndSet(false, true)) {
            showResult("SWD-Prüfung", "Eine andere Anfrage läuft. Bitte erneut drücken.");
            return;
        }
        final String ip = currentHost;
        updateBanner("Direkte SWD-Prüfung über ESP32 läuft …", false);
        worker.execute(() -> {
            try {
                String result = checkSwdCommands(ip);
                showResult("SWD-Diagnose · " + ip, result);
            } catch (Exception error) {
                showResult("SWD-Diagnose · " + ip,
                    "Direkte SWD-Abfrage nicht abgeschlossen: " +
                    error.getClass().getSimpleName() + ": " + error.getMessage());
            } finally {
                busy.set(false);
                runOnUiThread(() -> refresh());
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
                    description = "ESP32-Firmware: " +
                            obj.optString("firmware_version", "alte Version / nicht angegeben") +
                            "\nOTA aktiv: " + obj.optBoolean("ota", false) +
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
