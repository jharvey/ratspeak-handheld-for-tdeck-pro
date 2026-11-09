// RatSpeak T-Deck Pro simulator.
//   ratspeak-sim [--name device1] [--data DIR] [--fast] [--script FILE [--quit]] [--shots DIR]
//                [--serial-tcp PORT] [--live COM8 | --live tcp:127.0.0.1:PORT]
// Run two with different --name values to message between simulated devices.
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>

#include <cstdio>
#include <cstdlib>
#include <unistd.h>

#include "app/FirmwareRunner.h"
#include "app/LiveLink.h"
#include "app/MainWindow.h"
#include "hw/SimHw.h"

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
    p.addOptions({name, data, fast, script, quit, shots, live, serialTcp});
    p.process(app);

    SimOptions opt;
    opt.name = p.value(name);
    opt.dataDir = p.isSet(data) ? p.value(data)
                                : QDir(QCoreApplication::applicationDirPath()).filePath("simdata/" + opt.name);
    opt.dataDir = QDir(opt.dataDir).absolutePath();
    opt.fast = p.isSet(fast);
    opt.script = p.value(script);
    opt.quitAfterScript = p.isSet(quit);
    opt.shotDir = p.value(shots);
    opt.live = p.value(live);
    opt.serialTcp = (quint16)p.value(serialTcp).toUInt();
    QDir().mkpath(opt.dataDir);

    {
        std::lock_guard<std::mutex> l(sim::hw().mu);
        sim::hw().dataDir = opt.dataDir.toStdString();
        sim::hw().deviceName = opt.name.toStdString();
        sim::hw().realisticPanel = !opt.fast;
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
