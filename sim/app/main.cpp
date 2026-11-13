// RatSpeak T-Deck Pro simulator.
//   ratspeak-sim [--name device1] [--data DIR] [--fast] [--script FILE [--quit]] [--shots DIR]
//                [--serial-tcp PORT] [--live COM8 | --live tcp:127.0.0.1:PORT]
//                [--config FILE] [--sd 32G|none] [--battery 61] [--usb] [--charged]
//                [--gps fix|nofix|LAT,LON] [--sats N] [--flashkb KB]
// Hardware setup: <data>/sim.ini (if present), then --config FILE, then the options above.
// Run two with different --name values to message between simulated devices.
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QMap>
#include <QRegularExpression>
#include <QTextStream>

#include <cstdio>
#include <cstdlib>
#include <unistd.h>

#include "app/FirmwareRunner.h"
#include "app/LiveLink.h"
#include "app/MainWindow.h"
#include "hw/SimHw.h"

namespace {

// "key = value" / "key value" lines, # comments. Later files and options override earlier ones.
bool readConfig(const QString& path, QMap<QString, QString>& cfg) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
    QTextStream in(&f);
    while (!in.atEnd()) {
        QString line = in.readLine();
        const int hash = line.indexOf('#');
        if (hash >= 0) line.truncate(hash);
        line = line.trimmed();
        if (line.isEmpty()) continue;
        const QRegularExpressionMatch m = QRegularExpression("^([A-Za-z_]+)\\s*[=\\s]\\s*(.*)$").match(line);
        if (m.hasMatch()) cfg[m.captured(1).toLower()] = m.captured(2).trimmed();
        else cfg[line.toLower()] = "on";                         // bare flag: "usb"
    }
    return true;
}

[[noreturn]] void badConfig(const QString& key, const QString& value, const char* want) {
    fprintf(stderr, "ratspeak-sim: bad %s \"%s\" (want %s)\n", key.toLocal8Bit().constData(),
            value.toLocal8Bit().constData(), want);
    exit(2);
}

bool onOff(const QString& key, const QString& v) {
    const QString s = v.toLower();
    if (s == "on" || s == "yes" || s == "true" || s == "1") return true;
    if (s == "off" || s == "no" || s == "false" || s == "0") return false;
    badConfig(key, v, "on / off");
}

// "32G", "32GB", "512M", "2T"; "none" / "out" / "0" = empty slot -> bytes (0 = no card).
uint64_t sdSize(const QString& v) {
    const QString s = v.trimmed().toUpper();
    if (s == "NONE" || s == "OUT" || s == "NO" || s == "OFF" || s == "0") return 0;
    const QRegularExpressionMatch m = QRegularExpression("^(\\d+(?:\\.\\d+)?)\\s*([MGT])B?$").match(s);
    if (!m.hasMatch()) badConfig("sd", v, "a size like 32G or 512M, or none");
    const double n = m.captured(1).toDouble();
    const int sh = m.captured(2) == "M" ? 20 : m.captured(2) == "G" ? 30 : 40;
    const uint64_t bytes = (uint64_t)(n * (double)(1ull << sh));
    if (bytes < (8ull << 20)) badConfig("sd", v, "at least 8M");
    return bytes;
}

// Apply the hardware setup (caller holds sim::hw().mu).
void applyConfig(const QMap<QString, QString>& cfg) {
    auto& h = sim::hw();
    for (auto it = cfg.begin(); it != cfg.end(); ++it) {
        const QString& k = it.key();
        const QString& v = it.value();
        bool ok = true;
        if (k == "sd") {
            const uint64_t b = sdSize(v);
            h.sdPresent = b != 0;
            if (b) h.sdBytes = b;
        } else if (k == "battery") {
            const int pct = QString(v).remove('%').trimmed().toInt(&ok);
            if (!ok || pct < 0 || pct > 100) badConfig(k, v, "0-100");
            h.batterySoc = pct;
        } else if (k == "usb") {
            h.usb = onOff(k, v);
        } else if (k == "charged") {
            h.chargeDone = onOff(k, v);
        } else if (k == "gps") {
            const QString s = v.toLower();
            if (s == "fix" || s == "on") {
                h.gpsFix = true;
            } else if (s == "nofix" || s == "off" || s == "none") {
                h.gpsFix = false;
            } else {
                const QStringList ll = v.split(',');
                bool okLon = false;
                const double lat = ll.size() == 2 ? ll[0].trimmed().toDouble(&ok) : 0;
                const double lon = ll.size() == 2 ? ll[1].trimmed().toDouble(&okLon) : 0;
                if (ll.size() != 2 || !ok || !okLon || lat < -90 || lat > 90 || lon < -180 || lon > 180)
                    badConfig(k, v, "fix, nofix or LAT,LON");
                h.gpsFix = true;
                h.lat = lat;
                h.lon = lon;
            }
        } else if (k == "sats") {
            const int n = v.toInt(&ok);
            if (!ok || n < 0 || n > 40) badConfig(k, v, "0-40");
            h.sats = n;
        } else if (k == "flashkb") {
            const qulonglong kb = v.toULongLong(&ok);
            if (!ok || kb < 64) badConfig(k, v, "at least 64");
            h.flashBytes = kb * 1024;
        } else if (k == "fast") {
            h.realisticPanel = !onOff(k, v);
        } else {
            fprintf(stderr, "ratspeak-sim: unknown setting \"%s\" (sd battery usb charged gps sats flashkb fast)\n",
                    k.toLocal8Bit().constData());
            exit(2);
        }
    }
    if (!h.usb) h.chargeDone = false;
    h.charging = h.usb && !h.chargeDone;
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("ratspeak-sim");

    QCommandLineParser p;
    p.setApplicationDescription("RatSpeak T-Deck Pro simulator (real firmware on simulated hardware)");
    p.addHelpOption();
    QCommandLineOption name("name", "Simulated device name (selects its data folder and MAC).", "name", "device1");
    QCommandLineOption data("data", "Data folder (NVS, LittleFS, SD). Default: <exe>/simdata/<name>.", "dir");
    QCommandLineOption fast("fast", "Instant e-ink refresh (no ~650 ms panel delay).");
    QCommandLineOption script("script", "Run a key/screenshot script (see sim/README.md).", "file");
    QCommandLineOption quit("quit", "Exit when the script ends (exit code 1 on a failed expect).");
    QCommandLineOption shots("shots", "Folder for screenshots. Default: <data>/shots.", "dir");
    QCommandLineOption live("live", "Live-device mode: mirror a real T-Deck Pro on a USB serial port "
                                    "(COM8) or tcp:host:port, and send it the keys. No firmware runs here.",
                            "port");
    QCommandLineOption serialTcp("serial-tcp", "Expose this simulator's firmware USB serial on "
                                               "127.0.0.1:<port> (a second simulator can --live to it).",
                                 "port");
    QCommandLineOption noSd("nosd", "Start with the SD card slot empty (same as --sd none).");
    QCommandLineOption flashKb("flashkb", "Flash (LittleFS) size in KB; writes fail when it is full. "
                               "Default 8064 (the device partition).", "kb");
    QCommandLineOption config("config", "Hardware setup file (key = value lines: sd, battery, usb, charged, "
                              "gps, sats, flashkb, fast). <data>/sim.ini is read first if it exists.", "file");
    QCommandLineOption sd("sd", "SD card: a size (32G, 512M, 1T) or none. Default 16G.", "size");
    QCommandLineOption battery("battery", "Battery charge in percent (0-100). Default 72.", "pct");
    QCommandLineOption usb("usb", "Start plugged into USB (charging).");
    QCommandLineOption charged("charged", "With --usb: charging finished.");
    QCommandLineOption gps("gps", "GPS: fix, nofix, or LAT,LON (a fix there). Default: fix in Toronto.", "fix");
    QCommandLineOption sats("sats", "Satellites in the fix (0-40). Default 7.", "n");
    p.addOptions({name, data, fast, script, quit, shots, live, serialTcp, noSd, flashKb, config, sd, battery,
                  usb, charged, gps, sats});
    p.process(app);

    SimOptions opt;
    opt.name = p.value(name);
    opt.dataDir = p.isSet(data) ? p.value(data)
                                : QDir(QCoreApplication::applicationDirPath()).filePath("simdata/" + opt.name);
    opt.dataDir = QDir(opt.dataDir).absolutePath();
    opt.script = p.value(script);
    opt.quitAfterScript = p.isSet(quit);
    opt.shotDir = p.value(shots);
    opt.live = p.value(live);
    opt.serialTcp = (quint16)p.value(serialTcp).toUInt();
    QDir().mkpath(opt.dataDir);

    // Hardware setup: <data>/sim.ini, then --config, then command-line options.
    QMap<QString, QString> cfg;
    QStringList from;
    if (readConfig(QDir(opt.dataDir).filePath("sim.ini"), cfg)) from << "sim.ini";
    if (p.isSet(config)) {
        if (!readConfig(p.value(config), cfg)) {
            fprintf(stderr, "ratspeak-sim: cannot read --config %s\n", p.value(config).toLocal8Bit().constData());
            return 2;
        }
        from << p.value(config);
    }
    if (p.isSet(fast)) cfg["fast"] = "on";
    if (p.isSet(noSd)) cfg["sd"] = "none";
    if (p.isSet(sd)) cfg["sd"] = p.value(sd);
    if (p.isSet(flashKb)) cfg["flashkb"] = p.value(flashKb);
    if (p.isSet(battery)) cfg["battery"] = p.value(battery);
    if (p.isSet(usb)) cfg["usb"] = "on";
    if (p.isSet(charged)) cfg["charged"] = "on";
    if (p.isSet(gps)) cfg["gps"] = p.value(gps);
    if (p.isSet(sats)) cfg["sats"] = p.value(sats);
    {
        std::lock_guard<std::mutex> l(sim::hw().mu);
        auto& h = sim::hw();
        h.dataDir = opt.dataDir.toStdString();
        h.deviceName = opt.name.toStdString();
        applyConfig(cfg);
        opt.fast = !h.realisticPanel;
        const QString sdText = h.sdPresent ? QString("%1 GB").arg(h.sdBytes / double(1ull << 30), 0, 'g', 4) : "none";
        printf("[SIM] hardware: SD %s, battery %d%%, %s, GPS %s %.5f,%.5f %d sats, flash %llu KB%s%s\n",
               sdText.toLocal8Bit().constData(), h.batterySoc,
               h.usb ? (h.chargeDone ? "USB (charged)" : "USB (charging)") : "on battery",
               h.gpsFix ? "fix" : "no fix", h.lat, h.lon, h.sats, (unsigned long long)(h.flashBytes / 1024),
               from.isEmpty() ? "" : ", from ", from.join(" + ").toLocal8Bit().constData());
        fflush(stdout);
    }

    MainWindow w(opt);
    w.show();

    FirmwareRunner fw;
    SerialTcpBridge* bridge = nullptr;
    if (opt.live.isEmpty()) {
        if (opt.serialTcp) bridge = new SerialTcpBridge(opt.serialTcp, &w);
        fw.start();
    }
    (void)bridge;
    const int rc = app.exec();
    // The firmware thread never returns (like the ESP32 loop task): leave without
    // running static destructors underneath it.
    fflush(stdout);
    fflush(stderr);
    _exit(rc);
}
