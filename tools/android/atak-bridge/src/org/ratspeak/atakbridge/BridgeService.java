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
import android.content.SharedPreferences;
import android.content.pm.ServiceInfo;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;

import java.io.ByteArrayOutputStream;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.MulticastSocket;
import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.Arrays;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * The bridge itself: keeps the Bluetooth LE link to one radio and moves Cursor-on-Target events
 * between it and ATAK on this phone.
 *
 * Radio -> ATAK: the radio notifies the events cut into pieces; the bytes are joined, split on
 * "</event>" and each event is sent as a UDP datagram to ATAK's local input (127.0.0.1:4242 by
 * default, ATAK listens there out of the box).
 * ATAK -> radio: ATAK's own position goes out on the SA multicast 239.2.3.1:6969; markers and
 * more go to any "output" added in ATAK (Settings, Network, Manage Outputs: 127.0.0.1:4349 UDP).
 * Both are read here and written to the radio, one GATT write at a time. Events that came from a
 * radio (uid RatSpeak-...) are not sent back; the same event seen on both sockets goes once.
 * The radio asks for pairing on the first connection: Android shows its pairing dialog, type the
 * 6-digit PIN the radio shows. After that the phone is bonded and reconnects by itself.
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
    static final long SETUP_TIMEOUT_MS = 60000;    // connected but not ready by then: start over
    private static final byte[] END = "</event>".getBytes(StandardCharsets.UTF_8);

    // Shown by MainActivity (read every second).
    static volatile String state = "stopped";
    static volatile String radioName = "";
    static volatile String saNote = "";
    static final AtomicInteger toAtak = new AtomicInteger(), fromAtak = new AtomicInteger(),
            dropped = new AtomicInteger();
    static volatile boolean running = false;

    private final Handler main = new Handler(Looper.getMainLooper());
    private volatile boolean alive = false;        // this instance (old threads stop on their own)
    private BluetoothGatt gatt;
    private BluetoothGattCharacteristic rxChar;
    private String addr;
    private InetSocketAddress atak = new InetSocketAddress("127.0.0.1", 4242);
    private int mtu = 23;
    private boolean ready = false, stopping = false, discovering = false;
    private int cccdTries = 0, bondWaits = 0;
    private final ByteArrayOutputStream inBuf = new ByteArrayOutputStream();
    private final ArrayDeque<byte[]> outQ = new ArrayDeque<>();   // pieces waiting for the radio
    private byte[] inFlight;                       // the piece being written (put back on failure)
    private int writeFails = 0;
    private boolean writing = false;
    private String lastEv = "";
    private long lastEvAt = 0;
    private DatagramSocket txSock, outSock;
    private MulticastSocket saSock;
    private WifiManager.MulticastLock mcLock;
    private final Runnable setupWatchdog = () -> {
        if (!ready && gatt != null) {
            state = "setup timed out, retrying";
            gatt.disconnect();
            retry(3000);
        }
    };
    private final Runnable reconnect = this::connect;

    @Override public IBinder onBind(Intent i) { return null; }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        SharedPreferences prefs = getSharedPreferences("bridge", MODE_PRIVATE);
        String a;
        if (intent != null && intent.getStringExtra(EXTRA_ADDR) != null) {
            addr = intent.getStringExtra(EXTRA_ADDR);
            radioName = intent.getStringExtra(EXTRA_NAME) != null ? intent.getStringExtra(EXTRA_NAME) : addr;
            a = intent.getStringExtra(EXTRA_ATAK);
        } else {                                   // restarted by Android after it killed us
            addr = prefs.getString("addr", null);
            radioName = prefs.getString("name", addr);
            a = prefs.getString("atak", null);
        }
        startInForeground();
        if (addr == null) { stopSelf(); return START_NOT_STICKY; }
        if (a != null && a.contains(":")) {
            try {
                atak = new InetSocketAddress(a.substring(0, a.lastIndexOf(':')),
                        Integer.parseInt(a.substring(a.lastIndexOf(':') + 1).trim()));
            } catch (Exception ignored) { }
        }
        if (!alive) {
            alive = true;
            running = true;
            stopping = false;
            toAtak.set(0); fromAtak.set(0); dropped.set(0);
            openUdp();
            connect();
        }
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        stopping = true;
        alive = false;
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
        discovering = false;
        mtu = 23;
        cccdTries = 0;
        bondWaits = 0;
        if (gatt != null) { gatt.close(); gatt = null; }
        gatt = dev.connectGatt(this, false, cb, BluetoothDevice.TRANSPORT_LE);
        if (gatt == null) { state = "could not start the connection"; retry(5000); }
    }

    private void retry(long ms) {
        if (stopping) return;
        main.removeCallbacks(reconnect);
        main.removeCallbacks(setupWatchdog);
        main.postDelayed(reconnect, ms);
    }

    private void discover(BluetoothGatt g) {
        if (discovering || g != gatt) return;      // Android 14 may report the MTU twice
        discovering = true;
        if (!g.discoverServices()) { state = "could not read the radio's services"; g.disconnect(); }
    }

    private final BluetoothGattCallback cb = new BluetoothGattCallback() {
        @Override
        public void onConnectionStateChange(BluetoothGatt g, int status, int newState) {
            main.post(() -> {
                if (g != gatt) return;
                if (newState == BluetoothProfile.STATE_CONNECTED) {
                    state = "connected, setting up";
                    main.removeCallbacks(setupWatchdog);
                    main.postDelayed(setupWatchdog, SETUP_TIMEOUT_MS);
                    if (!g.requestMtu(247)) discover(g);
                } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                    ready = false;
                    synchronized (outQ) { outQ.clear(); writing = false; inFlight = null; }
                    inBuf.reset();
                    if (stopping) { state = "stopped"; return; }
                    // 5 / 15 / 137 (insufficient authentication / encryption, SMP failure): the
                    // pairing on one side was removed
                    if (status == 5 || status == 15 || status == 137)
                        state = "pairing does not match: remove RatSpeak in Android's Bluetooth settings and pair again";
                    else
                        state = "radio not in reach, retrying" + (status != 0 ? " (code " + status + ")" : "");
                    retry(5000);
                }
            });
        }

        @Override
        public void onMtuChanged(BluetoothGatt g, int m, int status) {
            main.post(() -> {
                if (status == BluetoothGatt.GATT_SUCCESS) mtu = m;
                discover(g);
            });
        }

        @Override
        public void onServicesDiscovered(BluetoothGatt g, int status) {
            main.post(() -> {
                if (g != gatt) return;
                BluetoothGattService s = g.getService(SVC);
                if (s == null) { state = "not a RatSpeak radio"; g.disconnect(); return; }
                rxChar = s.getCharacteristic(RX);
                BluetoothGattCharacteristic tx = s.getCharacteristic(TX);
                if (rxChar == null || tx == null) { state = "not a RatSpeak radio"; g.disconnect(); return; }
                state = "pairing: type the PIN the radio shows";
                subscribe(g, tx);
            });
        }

        @Override
        public void onDescriptorWrite(BluetoothGatt g, BluetoothGattDescriptor d, int status) {
            main.post(() -> {
                if (g != gatt) return;
                if (status == BluetoothGatt.GATT_SUCCESS) { waitBonded(g); return; }
                // Not paired yet (Android pairs and the radio asks for its PIN): try again.
                if (++cccdTries < 20) main.postDelayed(() -> subscribe(g, d.getCharacteristic()), 3000);
                else { state = "pairing failed"; g.disconnect(); }
            });
        }

        @Override
        @SuppressWarnings("deprecation")
        public void onCharacteristicChanged(BluetoothGatt g, BluetoothGattCharacteristic c) {
            if (Build.VERSION.SDK_INT < 33) {
                final byte[] v = c.getValue();
                if (v != null) { final byte[] copy = Arrays.copyOf(v, v.length); main.post(() -> fromRadio(copy)); }
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
                synchronized (outQ) {
                    writing = false;
                    if (status != BluetoothGatt.GATT_SUCCESS && inFlight != null && ++writeFails <= 5) {
                        outQ.addFirst(inFlight);                    // not secure yet / busy: again
                    } else {
                        if (status != BluetoothGatt.GATT_SUCCESS) dropped.incrementAndGet();
                        writeFails = 0;
                    }
                    inFlight = null;
                }
                main.postDelayed(BridgeService.this::writeNext, status == BluetoothGatt.GATT_SUCCESS ? 0 : 500);
            });
        }
    };

    // The CCCD itself may not need encryption, so its write can succeed before the pairing is
    // done; the radio only takes writes on a secure link. Ready = subscribed and bonded.
    private void waitBonded(BluetoothGatt g) {
        if (g != gatt) return;
        if (g.getDevice().getBondState() == BluetoothDevice.BOND_BONDED) {
            ready = true;
            main.removeCallbacks(setupWatchdog);
            state = "connected to " + radioName;
            writeNext();
            return;
        }
        state = "pairing: type the PIN the radio shows";
        if (++bondWaits < 60) main.postDelayed(() -> waitBonded(g), 1000);   // the watchdog ends it otherwise
    }

    @SuppressWarnings("deprecation")
    private void subscribe(BluetoothGatt g, BluetoothGattCharacteristic tx) {
        if (g != gatt) return;
        g.setCharacteristicNotification(tx, true);
        BluetoothGattDescriptor d = tx.getDescriptor(CCCD);
        if (d == null) { waitBonded(g); return; }
        boolean ok;
        if (Build.VERSION.SDK_INT >= 33) {
            ok = g.writeDescriptor(d, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE) == BluetoothGatt.GATT_SUCCESS;
        } else {
            d.setValue(BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE);
            ok = g.writeDescriptor(d);
        }
        if (!ok && ++cccdTries < 20) main.postDelayed(() -> subscribe(g, tx), 1000);   // busy (bonding)
    }

    // ---- radio -> ATAK -----------------------------------------------------------------------
    // Bytes, not text, until a whole event is in: a character cut between two notifications
    // must not turn into U+FFFD.
    private void fromRadio(byte[] v) {
        inBuf.write(v, 0, v.length);
        byte[] b = inBuf.toByteArray();
        int start = 0, end;
        while ((end = indexOf(b, END, start)) >= 0) {
            String ev = new String(b, start, end + END.length - start, StandardCharsets.UTF_8);
            start = end + END.length;
            int s = ev.indexOf("<?xml");
            if (s < 0) s = ev.indexOf("<event");
            if (s >= 0) sendToAtak(ev.substring(s).trim());
        }
        inBuf.reset();
        if (b.length - start <= 8192) inBuf.write(b, start, b.length - start);   // else broken: start over
    }

    private static int indexOf(byte[] h, byte[] n, int from) {
        outer:
        for (int i = from; i <= h.length - n.length; i++) {
            for (int j = 0; j < n.length; j++) if (h[i + j] != n[j]) continue outer;
            return i;
        }
        return -1;
    }

    private void sendToAtak(String ev) {
        final byte[] b = ev.getBytes(StandardCharsets.UTF_8);
        final DatagramSocket s = txSock;
        new Thread(() -> {
            try {
                s.send(new DatagramPacket(b, b.length, atak));
                toAtak.incrementAndGet();
            } catch (Exception e) {
                dropped.incrementAndGet();
            }
        }).start();
    }

    // ---- ATAK -> radio -----------------------------------------------------------------------
    // Sockets are made here, before their threads start, so closeUdp() always finds them.
    private void openUdp() {
        try { txSock = new DatagramSocket(); } catch (Exception e) { state = "UDP failed: " + e.getMessage(); }
        WifiManager wm = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
        if (wm != null) { mcLock = wm.createMulticastLock("ratspeak-sa"); mcLock.setReferenceCounted(false); mcLock.acquire(); }
        try {
            outSock = new DatagramSocket(null);
            outSock.setReuseAddress(true);
            outSock.bind(new InetSocketAddress(OUT_PORT));
            final DatagramSocket s = outSock;
            new Thread(() -> listen(s)).start();
        } catch (Exception e) {
            state = "port " + OUT_PORT + " is in use: markers from ATAK will not reach the radio";
        }
        new Thread(this::saLoop).start();
    }

    // ATAK's own position on the SA multicast. With no WiFi (mobile data only, or nothing at all)
    // the group cannot be joined: try again every 30 s, so it starts once WiFi is there.
    private void saLoop() {
        while (alive) {
            try {
                MulticastSocket s = new MulticastSocket(SA_PORT);
                s.joinGroup(InetAddress.getByName(SA_GROUP));
                saSock = s;
                saNote = "";
                listen(s);
                return;
            } catch (Exception e) {
                saNote = "ATAK position: waiting for WiFi (or add the 127.0.0.1:4349 output in ATAK)";
                try { Thread.sleep(30000); } catch (InterruptedException ie) { return; }
            }
        }
    }

    private void listen(DatagramSocket s) {
        byte[] buf = new byte[8192];
        try {
            while (alive) {
                DatagramPacket p = new DatagramPacket(buf, buf.length);
                s.receive(p);
                String ev = new String(p.getData(), 0, p.getLength(), StandardCharsets.UTF_8).trim();
                if (!ev.contains("<event") || !ev.contains("</event>")) continue;
                if (ev.contains("uid=\"RatSpeak-")) continue;          // from a radio: not back again
                main.post(() -> toRadio(ev));
            }
        } catch (Exception ignored) {              // socket closed: the service is stopping
        }
    }

    private void toRadio(String ev) {
        long now = System.currentTimeMillis();
        if (ev.equals(lastEv) && now - lastEvAt < 2000) return;   // the same event on both sockets
        lastEv = ev;
        lastEvAt = now;
        if (!ready || gatt == null || rxChar == null) { dropped.incrementAndGet(); return; }
        byte[] b = (ev + "\n").getBytes(StandardCharsets.UTF_8);
        int chunk = Math.max(20, Math.min(mtu - 3, 244));
        synchronized (outQ) {
            if (outQ.size() > 400) { dropped.incrementAndGet(); return; }   // the radio is slow: skip
            for (int o = 0; o < b.length; o += chunk) outQ.add(Arrays.copyOfRange(b, o, Math.min(b.length, o + chunk)));
        }
        fromAtak.incrementAndGet();
        writeNext();
    }

    @SuppressWarnings("deprecation")
    private void writeNext() {
        byte[] piece;
        synchronized (outQ) {
            if (!ready || writing || outQ.isEmpty() || gatt == null || rxChar == null) return;
            piece = outQ.poll();
            writing = true;
            inFlight = piece;
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
            synchronized (outQ) { outQ.addFirst(piece); writing = false; inFlight = null; }
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
