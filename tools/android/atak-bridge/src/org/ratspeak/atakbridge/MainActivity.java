package org.ratspeak.atakbridge;

import android.Manifest;
import android.annotation.SuppressLint;
import android.app.Activity;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothManager;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanFilter;
import android.bluetooth.le.ScanResult;
import android.bluetooth.le.ScanSettings;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.graphics.Typeface;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelUuid;
import android.text.InputType;
import android.view.View;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.ScrollView;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/** One screen: find a radio, connect, see what goes through. Built in code (no resources). */
@SuppressLint("MissingPermission")
public class MainActivity extends Activity {

    private final Handler ui = new Handler(Looper.getMainLooper());
    private TextView status, stats;
    private EditText atak;
    private Button scanBtn, stopBtn;
    private ArrayAdapter<String> adapter;
    private final List<BluetoothDevice> found = new ArrayList<>();
    private final List<String> labels = new ArrayList<>();
    private BluetoothLeScanner scanner;
    private boolean scanning = false;
    private SharedPreferences prefs;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        prefs = getSharedPreferences("bridge", MODE_PRIVATE);
        int pad = (int) (16 * getResources().getDisplayMetrics().density);

        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding(pad, pad, pad, pad);

        TextView title = new TextView(this);
        title.setText("RatSpeak ATAK bridge");
        title.setTextSize(22);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        col.addView(title);

        TextView help = new TextView(this);
        help.setText("On the radio: M, S, 3 (WiFi): ATAK feed ON, ATAK link to phone = Bluetooth. "
                + "Then scan here and tap the radio. The first time, Android asks for a pairing code: "
                + "type the 6 digits the radio shows.\n\n"
                + "For your markers to reach the radio, add an output in ATAK once: Settings, Network "
                + "Preferences, Manage Outputs, add 127.0.0.1 port 4349, UDP.");
        help.setPadding(0, pad / 2, 0, pad / 2);
        col.addView(help);

        TextView al = new TextView(this);
        al.setText("Send to ATAK at (address:port)");
        col.addView(al);
        atak = new EditText(this);
        atak.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        atak.setText(prefs.getString("atak", "127.0.0.1:4242"));
        col.addView(atak);

        status = new TextView(this);
        status.setTextSize(16);
        status.setTypeface(Typeface.DEFAULT_BOLD);
        status.setPadding(0, pad / 2, 0, 0);
        col.addView(status);
        stats = new TextView(this);
        col.addView(stats);

        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        scanBtn = new Button(this);
        scanBtn.setText("Scan for radios");
        scanBtn.setOnClickListener(v -> toggleScan());
        row.addView(scanBtn);
        stopBtn = new Button(this);
        stopBtn.setText("Disconnect");
        stopBtn.setOnClickListener(v -> {
            stopService(new Intent(this, BridgeService.class));
            prefs.edit().remove("addr").apply();
        });
        row.addView(stopBtn);
        col.addView(row);

        ListView list = new ListView(this);
        adapter = new ArrayAdapter<>(this, android.R.layout.simple_list_item_1, labels);
        list.setAdapter(adapter);
        list.setOnItemClickListener((p, v, pos, id) -> start(found.get(pos)));
        col.addView(list, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, pad * 16));

        ScrollView sv = new ScrollView(this);
        sv.addView(col);
        setContentView(sv);
        askPermissions();
        refresh();
    }

    @Override protected void onDestroy() { stopScan(); ui.removeCallbacksAndMessages(null); super.onDestroy(); }

    private void refresh() {
        status.setText(BridgeService.running ? BridgeService.state : "Not connected");
        stats.setText(BridgeService.running
                ? String.format("Radio %s\nto ATAK %d   from ATAK %d   dropped %d", BridgeService.radioName,
                        BridgeService.toAtak, BridgeService.fromAtak, BridgeService.dropped)
                : "");
        stopBtn.setEnabled(BridgeService.running);
        ui.postDelayed(this::refresh, 1000);
    }

    // ---- permissions -------------------------------------------------------------------------
    private String[] needed() {
        List<String> p = new ArrayList<>();
        if (Build.VERSION.SDK_INT >= 31) {
            p.add(Manifest.permission.BLUETOOTH_SCAN);
            p.add(Manifest.permission.BLUETOOTH_CONNECT);
        } else {
            p.add(Manifest.permission.ACCESS_FINE_LOCATION);
        }
        if (Build.VERSION.SDK_INT >= 33) p.add(Manifest.permission.POST_NOTIFICATIONS);
        List<String> missing = new ArrayList<>();
        for (String s : p) if (checkSelfPermission(s) != PackageManager.PERMISSION_GRANTED) missing.add(s);
        return missing.toArray(new String[0]);
    }

    private void askPermissions() {
        String[] m = needed();
        if (m.length > 0) requestPermissions(m, 1);
    }

    private boolean havePermissions() {
        for (String s : needed())
            if (!s.equals(Manifest.permission.POST_NOTIFICATIONS)) return false;
        return true;
    }

    // ---- scan --------------------------------------------------------------------------------
    private void toggleScan() {
        if (scanning) { stopScan(); return; }
        if (!havePermissions()) { askPermissions(); status.setText("Allow Bluetooth (and Nearby devices) first"); return; }
        BluetoothManager bm = (BluetoothManager) getSystemService(Context.BLUETOOTH_SERVICE);
        BluetoothAdapter ad = bm != null ? bm.getAdapter() : null;
        if (ad == null || !ad.isEnabled()) { status.setText("Turn Bluetooth on first"); return; }
        scanner = ad.getBluetoothLeScanner();
        found.clear();
        labels.clear();
        adapter.notifyDataSetChanged();
        ScanFilter f = new ScanFilter.Builder().setServiceUuid(new ParcelUuid(BridgeService.SVC)).build();
        ScanSettings s = new ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build();
        scanner.startScan(Collections.singletonList(f), s, scanCb);
        scanning = true;
        scanBtn.setText("Stop scan");
        ui.postDelayed(this::stopScan, 15000);
    }

    private void stopScan() {
        if (!scanning) return;
        try { scanner.stopScan(scanCb); } catch (Exception ignored) { }
        scanning = false;
        scanBtn.setText("Scan for radios");
    }

    private final ScanCallback scanCb = new ScanCallback() {
        @Override
        public void onScanResult(int type, ScanResult r) {
            BluetoothDevice d = r.getDevice();
            for (BluetoothDevice x : found) if (x.getAddress().equals(d.getAddress())) return;
            String name = r.getScanRecord() != null ? r.getScanRecord().getDeviceName() : null;
            if (name == null) name = d.getName();
            found.add(d);
            labels.add((name != null ? name : "RatSpeak radio") + "   " + d.getAddress() + "   " + r.getRssi() + " dBm");
            adapter.notifyDataSetChanged();
        }
    };

    private void start(BluetoothDevice d) {
        stopScan();
        String a = atak.getText().toString().trim();
        prefs.edit().putString("atak", a).putString("addr", d.getAddress()).apply();
        String name = d.getName();
        Intent i = new Intent(this, BridgeService.class)
                .putExtra(BridgeService.EXTRA_ADDR, d.getAddress())
                .putExtra(BridgeService.EXTRA_NAME, name != null ? name : d.getAddress())
                .putExtra(BridgeService.EXTRA_ATAK, a);
        stopService(new Intent(this, BridgeService.class));
        if (Build.VERSION.SDK_INT >= 26) startForegroundService(i); else startService(i);
    }
}
