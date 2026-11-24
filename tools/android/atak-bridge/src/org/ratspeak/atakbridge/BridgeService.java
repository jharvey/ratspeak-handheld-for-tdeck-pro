package org.ratspeak.atakbridge;

import android.annotation.SuppressLint;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothProfile;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.MulticastSocket;
import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.UUID;

/**
 * The bridge itself: keeps the Bluetooth LE link to one radio and moves Cursor-on-Target events
 * between it and ATAK on this phone.
 *
 * Radio -> ATAK: the radio notifies the events cut into pieces; they are joined, split on
 * "</event>" and each one is sent as a UDP datagram to ATAK's local input (127.0.0.1:4242 by
 * default, ATAK listens there out of the box).
 * ATAK -> radio: ATAK's own position goes out on the SA multicast 239.2.3.1:6969; markers and
 * more go to any "output" added in ATAK (Settings, Network, Manage Outputs: 127.0.0.1:4349 UDP).
 * Both are read here and written to the radio. Events that came from a radio (uid RatSpeak-...)
 * are not sent back.
 * The radio asks for pairing on the first connection: Android shows its pairing dialog, type the
 * 6-digit code the radio shows. After that the phone is bonded and reconnects by itself.
 */
@SuppressLint("MissingPermission")   // MainActivity asks for the Bluetooth permissions first
public class BridgeService extends Service {

    static final UUID SVC = UUID.fromString("6e400001-b5a3-f393-e0a9-e50e24dcca9e");
    static final UUID RX = UUID.fromString("6e400002-b5a3-f393-e0a9-e50e24dcca9e");   // phone writes
    static final UUID TX = UUID.fromString("6e400003-b5a3-f393-e0a9-e50e24dcca9e");   // radio notifies
    static final UUID CCCD = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb");

    static final String EXTRA_ADDR = "addr", EXTRA_NAME = "name", EXTRA_ATAK = "atak";
    static final int SA_PORT = 6969, OUT_PORT = 4349;
    static final String SA_GROUP = "239.2.3.1";

    // Shown by MainActivity (read every second).
    static volatile String state = "stopped";
    static volatile String radioName = "";
    static volatile int toAtak = 0, fromAtak = 0, dropped = 0;
    static volatile boolean running = false;

    private final Handler main = new Handler(Looper.getMainLooper());
    private BluetoothGatt gatt;
    private BluetoothGattCharacteristic rxChar;
    private String addr;
    private InetSocketAddress atak = new InetSocketAddress("127.0.0.1", 4242);
    private int mtu = 23;
    private boolean ready = false, stopping = false;
    private int cccdTries = 0;
    private final StringBuilder inBuf = new StringBuilder();
    private final ArrayDeque<byte[]> outQ = new ArrayDeque<>();   // pieces waiting for the radio
    private boolean writing = false;
    private DatagramSocket txSock;
    private Thread rxThread, saThread;
    private DatagramSocket outSock;
    private MulticastSocket saSock;
    private WifiManager.MulticastLock mcLock;

    @Override public IBinder onBind(Intent i) { return null; }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent == null) return START_NOT_STICKY;
        addr = intent.getStringExtra(EXTRA_ADDR);
        radioName = intent.getStringExtra(EXTRA_NAME) != null ? intent.getStringExtra(EXTRA_NAME) : addr;
        String a = intent.getStringExtra(EXTRA_ATAK);
        if (a != null && a.contains(":")) {
            try {
                atak = new InetSocketAddress(a.substring(0, a.lastIndexOf(':')),
                        Integer.parseInt(a.substring(a.lastIndexOf(':') + 1).trim()));
            } catch (Exception ignored) { }
        }
        startInForeground();
        running = true;
        stopping = false;
        toAtak = fromAtak = dropped = 0;
        openUdp();
        connect();
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        stopping = true;
        running = false;
        main.removeCallbacksAndMessages(null);
        if (gatt != null) { gatt.disconnect(); gatt.close(); gatt = null; }
        closeUdp();
        state = "stopped";
        super.onDestroy();
    }

    // ---- foreground notification -------------------------------------------------------------
    private void startInForeground() {
        NotificationManager nm = (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
        if (Build.VERSION.SDK_INT >= 26)
            nm.createNotificationChannel(new NotificationChannel("bridge", "ATAK bridge", NotificationManager.IMPORTANCE_LOW));
        PendingIntent pi = PendingIntent.getActivity(this, 0, new Intent(this, MainActivity.class),
                PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        Notification n = new Notification.Builder(this, "bridge")
                .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
                .setContentTitle("RatSpeak ATAK bridge")
                .setContentText("Radio " + radioName)
                .setContentIntent(pi)
                .setOngoing(true)
                .build();
        if (Build.VERSION.SDK_INT >= 29) startForeground(1, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE);
        else startForeground(1, n);
    }

    // ---- Bluetooth ---------------------------------------------------------------------------
    private void connect() {
        if (stopping) return;
        BluetoothManager bm = (BluetoothManager) getSystemService(Context.BLUETOOTH_SERVICE);
        BluetoothAdapter ad = bm != null ? bm.getAdapter() : null;
        if (ad == null || !ad.isEnabled()) { state = "Bluetooth is off"; retry(5000); return; }
        BluetoothDevice dev = ad.getRemoteDevice(addr);
        state = "connecting to " + radioName;
        ready = false;
        mtu = 23;
        cccdTries = 0;
        if (gatt != null) { gatt.close(); gatt = null; }
        gatt = dev.connectGatt(this, false, cb, BluetoothDevice.TRANSPORT_LE);
    }

    private void retry(long ms) {
        if (stopping) return;
        main.removeCallbacksAndMessages(null);
        main.postDelayed(this::connect, ms);
    }

    private final BluetoothGattCallback cb = new BluetoothGattCallback() {
        @Override
        public void onConnectionStateChange(BluetoothGatt g, int status, int newState) {
            main.post(() -> {
                if (newState == BluetoothProfile.STATE_CONNECTED) {
                    state = "connected, setting up";
                    g.requestMtu(247);
                } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                    ready = false;
                    synchronized (outQ) { outQ.clear(); writing = false; }
                    inBuf.setLength(0);
                    state = stopping ? "stopped" : "radio not in reach, retrying";
                    retry(5000);
                }
            });
        }

        @Override
        public void onMtuChanged(BluetoothGatt g, int m, int status) {
            main.post(() -> {
                if (status == BluetoothGatt.GATT_SUCCESS) mtu = m;
                g.discoverServices();
            });
        }

        @Override
        public void onServicesDiscovered(BluetoothGatt g, int status) {
            main.post(() -> {
                BluetoothGattService s = g.getService(SVC);
                if (s == null) { state = "not a RatSpeak radio"; g.disconnect(); return; }
                rxChar = s.getCharacteristic(RX);
                BluetoothGattCharacteristic tx = s.getCharacteristic(TX);
                if (rxChar == null || tx == null) { state = "not a RatSpeak radio"; g.disconnect(); return; }
                state = "pairing: type the code the radio shows";
                subscribe(g, tx);
            });
        }

        @Override
        public void onDescriptorWrite(BluetoothGatt g, BluetoothGattDescriptor d, int status) {
            main.post(() -> {
                if (status == BluetoothGatt.GATT_SUCCESS) {
                    ready = true;
                    state = "connected to " + radioName;
                    return;
                }
                // Not paired yet (Android pairs and the radio asks for its code): try again.
                if (++cccdTries < 20) main.postDelayed(() -> subscribe(g, d.getCharacteristic()), 3000);
                else { state = "pairing failed"; g.disconnect(); }
            });
        }

        @Override
        @SuppressWarnings("deprecation")
        public void onCharacteristicChanged(BluetoothGatt g, BluetoothGattCharacteristic c) {
            if (Build.VERSION.SDK_INT < 33) {
                final byte[] v = c.getValue();
                if (v != null) main.post(() -> fromRadio(v));
            }
        }

        @Override
        public void onCharacteristicChanged(BluetoothGatt g, BluetoothGattCharacteristic c, byte[] value) {
            final byte[] v = value;
            main.post(() -> fromRadio(v));
        }

        @Override
        public void onCharacteristicWrite(BluetoothGatt g, BluetoothGattCharacteristic c, int status) {
            main.post(() -> {
                synchronized (outQ) { writing = false; }
                writeNext();
            });
        }
    };

    @SuppressWarnings("deprecation")
    private void subscribe(BluetoothGatt g, BluetoothGattCharacteristic tx) {
        if (g != gatt) return;
        g.setCharacteristicNotification(tx, true);
        BluetoothGattDescriptor d = tx.getDescriptor(CCCD);
        if (d == null) { ready = true; state = "connected to " + radioName; return; }
        if (Build.VERSION.SDK_INT >= 33) {
            g.writeDescriptor(d, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE);
        } else {
            d.setValue(BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE);
            g.writeDescriptor(d);
        }
    }

    // ---- radio -> ATAK -----------------------------------------------------------------------
    private void fromRadio(byte[] v) {
        inBuf.append(new String(v, StandardCharsets.UTF_8));
        int end;
        while ((end = inBuf.indexOf("</event>")) >= 0) {
            String ev = inBuf.substring(0, end + 8);
            inBuf.delete(0, end + 8);
            int start = ev.indexOf("<?xml");
            if (start < 0) start = ev.indexOf("<event");
            if (start < 0) continue;
            sendToAtak(ev.substring(start).trim());
        }
        if (inBuf.length() > 8192) inBuf.setLength(0);   // broken stream: start over
    }

    private void sendToAtak(String ev) {
        final byte[] b = ev.getBytes(StandardCharsets.UTF_8);
        new Thread(() -> {
            try {
                txSock.send(new DatagramPacket(b, b.length, atak));
                toAtak++;
            } catch (Exception e) {
                dropped++;
            }
        }).start();
    }

    // ---- ATAK -> radio -----------------------------------------------------------------------
    private void openUdp() {
        try { txSock = new DatagramSocket(); } catch (Exception e) { state = "UDP failed: " + e.getMessage(); }
        WifiManager wm = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
        if (wm != null) { mcLock = wm.createMulticastLock("ratspeak-sa"); mcLock.setReferenceCounted(false); mcLock.acquire(); }
        rxThread = new Thread(() -> {
            try {
                outSock = new DatagramSocket(null);
                outSock.setReuseAddress(true);
                outSock.bind(new InetSocketAddress(OUT_PORT));
                listen(outSock);
            } catch (Exception ignored) { }
        });
        rxThread.start();
        saThread = new Thread(() -> {
            try {
                saSock = new MulticastSocket(SA_PORT);
                saSock.joinGroup(InetAddress.getByName(SA_GROUP));
                listen(saSock);
            } catch (Exception ignored) { }
        });
        saThread.start();
    }

    private void listen(DatagramSocket s) throws Exception {
        byte[] buf = new byte[8192];
        String last = "";
        long lastAt = 0;
        while (running) {
            DatagramPacket p = new DatagramPacket(buf, buf.length);
            s.receive(p);
            String ev = new String(p.getData(), 0, p.getLength(), StandardCharsets.UTF_8).trim();
            if (!ev.contains("<event") || !ev.contains("</event>")) continue;
            if (ev.contains("uid=\"RatSpeak-")) continue;          // from a radio: not back again
            long now = System.currentTimeMillis();
            if (ev.equals(last) && now - lastAt < 2000) continue;  // the same event on both sockets
            last = ev;
            lastAt = now;
            final byte[] b = (ev + "\n").getBytes(StandardCharsets.UTF_8);
            main.post(() -> toRadio(b));
        }
    }

    private void toRadio(byte[] ev) {
        if (!ready || gatt == null || rxChar == null) { dropped++; return; }
        int chunk = Math.max(20, Math.min(mtu - 3, 244));
        synchronized (outQ) {
            if (outQ.size() > 400) { dropped++; return; }            // the radio is slow: skip
            for (int o = 0; o < ev.length; o += chunk) {
                int n = Math.min(chunk, ev.length - o);
                byte[] piece = new byte[n];
                System.arraycopy(ev, o, piece, 0, n);
                outQ.add(piece);
            }
        }
        fromAtak++;
        writeNext();
    }

    @SuppressWarnings("deprecation")
    private void writeNext() {
        byte[] piece;
        synchronized (outQ) {
            if (writing || outQ.isEmpty() || gatt == null || rxChar == null) return;
            piece = outQ.poll();
            writing = true;
        }
        boolean ok;
        if (Build.VERSION.SDK_INT >= 33) {
            ok = gatt.writeCharacteristic(rxChar, piece, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT)
                    == BluetoothGatt.GATT_SUCCESS;
        } else {
            rxChar.setWriteType(BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
            rxChar.setValue(piece);
            ok = gatt.writeCharacteristic(rxChar);
        }
        if (!ok) {                                                   // busy: try again shortly
            synchronized (outQ) { outQ.addFirst(piece); writing = false; }
            main.postDelayed(this::writeNext, 50);
        }
    }

    private void closeUdp() {
        try { if (outSock != null) outSock.close(); } catch (Exception ignored) { }
        try { if (saSock != null) saSock.close(); } catch (Exception ignored) { }
        try { if (txSock != null) txSock.close(); } catch (Exception ignored) { }
        if (mcLock != null && mcLock.isHeld()) mcLock.release();
    }
}
