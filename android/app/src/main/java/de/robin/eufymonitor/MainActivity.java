package de.robin.eufymonitor;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.database.Cursor;
import android.graphics.Color;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.OpenableColumns;
import android.text.InputType;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import org.json.JSONArray;
import org.json.JSONObject;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.Iterator;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;

public class MainActivity extends Activity {
    private static final int FILE_REQUEST = 1001;
    private static final int BACKGROUND = Color.rgb(12, 19, 31);
    private static final int CARD = Color.rgb(29, 40, 57);
    private static final int TEXT = Color.rgb(231, 237, 248);
    private static final int MUTED = Color.rgb(164, 183, 204);
    private static final int ACCENT = Color.rgb(72, 195, 194);
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private final AtomicBoolean busy = new AtomicBoolean(false);
    private EditText hostEdit, tokenEdit;
    private TextView status;
    private LinearLayout sections;
    private Button autoButton;
    private boolean auto = true;
    private boolean running = false;
    private final Runnable ticker = new Runnable() {
        @Override public void run() {
            if (auto && !busy.get()) refresh();
            if (running) handler.postDelayed(this, 1250);
        }
    };

    private int dp(int n) { return (int)(getResources().getDisplayMetrics().density * n + .5f); }
    private LinearLayout vertical() {
        LinearLayout l = new LinearLayout(this);
        l.setOrientation(LinearLayout.VERTICAL);
        return l;
    }
    private TextView text(String value, int size, int color, boolean bold) {
        TextView t = new TextView(this);
        t.setText(value);
        t.setTextColor(color);
        t.setTextSize(size);
        if (bold) t.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        t.setPadding(dp(1), dp(4), dp(1), dp(4));
        return t;
    }
    private Button button(String label) {
        Button b = new Button(this);
        b.setText(label);
        b.setAllCaps(false);
        return b;
    }
    private void state(String s) { status.setText(s); }
    private String host() {
        String h = hostEdit.getText().toString().trim();
        h = h.replace("http://", "").replace("https://", "");
        int slash = h.indexOf('/'); if (slash >= 0) h = h.substring(0, slash);
        int colon = h.indexOf(':'); if (colon >= 0) h = h.substring(0, colon);
        return h;
    }
    @Override public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setBackgroundColor(BACKGROUND);
        LinearLayout root = vertical();
        root.setPadding(dp(16), dp(14), dp(16), dp(22));
        scroll.addView(root);
        setContentView(scroll);
        root.addView(text("EUFY 15C · LIVE DIAGNOSE", 23, TEXT, true));
        root.addView(text("ESP32-WROOM · GD32 OEM 1.1.8 · nur lesender SWD-Zugriff", 12, MUTED, false));
        root.addView(text("ESP32-Adresse", 13, MUTED, true));
        hostEdit = new EditText(this);
        hostEdit.setSingleLine(true);
        hostEdit.setTextColor(TEXT);
        hostEdit.setHintTextColor(MUTED);
        hostEdit.setText(getPreferences(MODE_PRIVATE).getString("host", "192.168.4.1"));
        hostEdit.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        root.addView(hostEdit);
        LinearLayout controls = new LinearLayout(this);
        controls.setOrientation(LinearLayout.HORIZONTAL);
        Button refresh = button("Aktualisieren");
        autoButton = button("Live: AN");
        controls.addView(refresh, new LinearLayout.LayoutParams(0, dp(52), 1f));
        controls.addView(autoButton, new LinearLayout.LayoutParams(0, dp(52), 1f));
        root.addView(controls);
        status = text("Verbinde …", 13, ACCENT, true);
        root.addView(status);
        refresh.setOnClickListener(v -> refresh());
        autoButton.setOnClickListener(v -> {
            auto = !auto;
            autoButton.setText(auto ? "Live: AN" : "Live: AUS");
            if (auto) refresh();
        });
        root.addView(text("WLAN-FIRMWAREUPDATE · nur ESP32", 17, TEXT, true));
        root.addView(text("Einmalig USB-Vollflash mit OTA-Partitionstabelle erforderlich. Danach BIN über WLAN. Token = Passwort des Eufy-SWD-WROOM-AP.", 12, MUTED, false));
        tokenEdit = new EditText(this);
        tokenEdit.setSingleLine(true);
        tokenEdit.setTextColor(TEXT);
        tokenEdit.setHint("OTA-Token (AP-Passwort)");
        tokenEdit.setHintTextColor(MUTED);
        tokenEdit.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD);
        root.addView(tokenEdit);
        LinearLayout otaControls = new LinearLayout(this);
        Button otaStatus = button("OTA-Status");
        Button ota = button("BIN auswählen / OTA");
        otaControls.addView(otaStatus, new LinearLayout.LayoutParams(0, dp(52), 1f));
        otaControls.addView(ota, new LinearLayout.LayoutParams(0, dp(52), 1.35f));
        root.addView(otaControls);
        otaStatus.setOnClickListener(v -> requestOtaStatus());
        ota.setOnClickListener(v -> chooseBinary());
        root.addView(text("Alle Messwerte (Rohdaten, unbekannte Einheiten unverändert)", 16, TEXT, true));
        sections = vertical();
        root.addView(sections);
    }
    private static byte[] readAll(InputStream input) throws Exception {
        try (InputStream in = input; ByteArrayOutputStream out = new ByteArrayOutputStream()) {
            byte[] buf = new byte[2048];
            int n;
            while ((n = in.read(buf)) != -1) out.write(buf, 0, n);
            return out.toByteArray();
        }
    }
    private static String httpGet(String address) throws Exception {
        HttpURLConnection c = (HttpURLConnection)new URL(address).openConnection();
        try {
            c.setConnectTimeout(3500);
            c.setReadTimeout(3500);
            c.setRequestMethod("GET");
            if (c.getResponseCode() != 200) throw new Exception("HTTP " + c.getResponseCode());
            return new String(readAll(c.getInputStream()), StandardCharsets.UTF_8);
        } finally { c.disconnect(); }
    }
    private void refresh() {
        if (!busy.compareAndSet(false, true)) return;
        final String ip = host();
        getPreferences(MODE_PRIVATE).edit().putString("host", ip).apply();
        if (ip.isEmpty()) { busy.set(false); state("Keine IP eingetragen"); return; }
        worker.execute(() -> {
            try {
                JSONObject data = new JSONObject(httpGet("http://" + ip + ":8080/telemetry"));
                runOnUiThread(() -> {
                    state("ESP " + ip + " · " + data.optString("status", "?") +
                            " · gültig: " + data.optBoolean("valid", false) +
                            " · Alter: " + data.optLong("sample_age_ms", -1) + " ms");
                    showData(data);
                });
            } catch (Exception e) {
                runOnUiThread(() -> state("Verbindung fehlgeschlagen: " + e.getMessage()));
            } finally { busy.set(false); }
        });
    }
    private void addObjectLines(LinearLayout parent, JSONObject obj, String prefix, int depth) {
        Iterator<String> it = obj.keys();
        while (it.hasNext()) {
            String key = it.next();
            Object val = obj.opt(key);
            String label = prefix.isEmpty() ? key : prefix + "." + key;
            if (val instanceof JSONObject) addObjectLines(parent, (JSONObject)val, label, depth + 1);
            else if (val instanceof JSONArray) parent.addView(text(label + " = " + val, 13, TEXT, false));
            else parent.addView(text(label + " :  " + String.valueOf(val), 13, TEXT, false));
        }
    }
    private void showData(JSONObject obj) {
        sections.removeAllViews();
        Iterator<String> keys = obj.keys();
        while (keys.hasNext()) {
            String name = keys.next();
            Object val = obj.opt(name);
            LinearLayout card = vertical();
            card.setBackgroundColor(CARD);
            card.setPadding(dp(12), dp(9), dp(12), dp(10));
            TextView heading = text(name.toUpperCase(Locale.ROOT).replace('_', ' '), 15, ACCENT, true);
            card.addView(heading);
            if (val instanceof JSONObject) addObjectLines(card, (JSONObject)val, "", 0);
            else card.addView(text(String.valueOf(val), 13, TEXT, false));
            LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(-1, -2);
            p.setMargins(0, dp(5), 0, dp(7));
            sections.addView(card, p);
        }
    }
    private void requestOtaStatus() {
        final String ip = host();
        worker.execute(() -> {
            try {
                String info = httpGet("http://" + ip + ":8081/status");
                runOnUiThread(() -> new AlertDialog.Builder(this)
                        .setTitle("OTA-Status").setMessage(info).setPositiveButton("OK", null).show());
            } catch (Exception e) {
                runOnUiThread(() -> state("OTA-Status: " + e.getMessage()));
            }
        });
    }
    private void chooseBinary() {
        if (tokenEdit.getText().toString().isEmpty()) {
            state("Bitte OTA-Token eingeben (AP-Passwort).");
            return;
        }
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        startActivityForResult(i, FILE_REQUEST);
    }
    @Override protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != FILE_REQUEST || resultCode != RESULT_OK || data == null) return;
        Uri uri = data.getData();
        if (uri == null) return;
        new AlertDialog.Builder(this)
            .setTitle("ESP32-OTA")
            .setMessage("Nur Eufy-WROOM-OTA-app-only.bin aus demselben Firmware-Workflow verwenden. Die GD32-Firmware wird nicht geschrieben. ESP32 wird nach erfolgreichem Update neu gestartet.")
            .setNegativeButton("Abbrechen", null)
            .setPositiveButton("Jetzt installieren", (d, w) -> upload(uri))
            .show();
    }
    private void upload(Uri uri) {
        if (!busy.compareAndSet(false, true)) {
            state("Noch eine Anfrage aktiv");
            return;
        }
        final String ip = host();
        final String token = tokenEdit.getText().toString();
        auto = false;
        autoButton.setText("Live: AUS");
        state("Übertrage BIN-Datei …");
        worker.execute(() -> {
            HttpURLConnection c = null;
            try {
                long size = -1;
                try (Cursor cur = getContentResolver().query(uri,
                        new String[] { OpenableColumns.SIZE }, null, null, null)) {
                    if (cur != null && cur.moveToFirst() && !cur.isNull(0))
                        size = cur.getLong(0);
                }
                if (size < 8192 || size > 0x160000)
                    throw new Exception("Ungültige BIN-Größe: " + size);
                c = (HttpURLConnection)new URL("http://" + ip + ":8081/upload").openConnection();
                c.setConnectTimeout(5000);
                c.setReadTimeout(180000);
                c.setRequestMethod("POST");
                c.setRequestProperty("Content-Type", "application/octet-stream");
                c.setRequestProperty("X-OTA-Token", token);
                c.setDoOutput(true);
                c.setFixedLengthStreamingMode((int)size);
                try (InputStream input = getContentResolver().openInputStream(uri);
                     OutputStream output = c.getOutputStream()) {
                    if (input == null) throw new Exception("Datei nicht lesbar");
                    byte[] buf = new byte[4096];
                    int n;
                    long count = 0;
                    while ((n = input.read(buf)) != -1) {
                        output.write(buf, 0, n);
                        count += n;
                    }
                    if (count != size) throw new Exception("Dateigröße hat sich geändert");
                }
                int code = c.getResponseCode();
                if (code != 200) throw new Exception("OTA HTTP " + code);
                final String result = new String(readAll(c.getInputStream()), StandardCharsets.UTF_8);
                runOnUiThread(() -> state("OTA angenommen: " + result + " · ESP32 startet neu."));
            } catch (Exception e) {
                final String msg = e.getMessage();
                runOnUiThread(() -> state("OTA fehlgeschlagen: " + msg));
            } finally {
                if (c != null) c.disconnect();
                busy.set(false);
            }
        });
    }
    @Override protected void onStart() {
        super.onStart();
        if (!running) { running = true; handler.post(ticker); }
    }
    @Override protected void onStop() {
        running = false;
        handler.removeCallbacks(ticker);
        super.onStop();
    }
    @Override protected void onDestroy() {
        worker.shutdownNow();
        super.onDestroy();
    }
}
