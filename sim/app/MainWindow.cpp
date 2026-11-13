#include "app/MainWindow.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QDesktopServices>
#include <QInputDialog>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProcess>
#include <QUrl>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSlider>
#include <QSplitter>
#include <QStatusBar>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <unistd.h>

#include "app/DeviceWidget.h"
#include "app/LiveLink.h"
#include "hw/SimHw.h"

namespace {
std::mutex g_logMu;
QStringList g_logQueue;

// Rough Li-ion open-circuit curve: SOC % -> mV.
int socToMv(int soc) {
    static const int pts[][2] = {{0, 3300}, {5, 3500}, {10, 3600}, {20, 3700}, {40, 3780},
                                 {60, 3870}, {80, 4000}, {90, 4080}, {100, 4180}};
    for (size_t i = 1; i < sizeof(pts) / sizeof(pts[0]); i++)
        if (soc <= pts[i][0]) {
            const double f = double(soc - pts[i - 1][0]) / (pts[i][0] - pts[i - 1][0]);
            return int(pts[i - 1][1] + f * (pts[i][1] - pts[i - 1][1]));
        }
    return 4180;
}

// Screen-mirror protocol lines (remote/ScreenMirror.h) are not shown in the console.
bool isMirrorLine(const QString& s) {
    return s.startsWith("@FB ") || s.startsWith("@ST ") || s.startsWith("@HELLO ");
}
}  // namespace

MainWindow::MainWindow(const SimOptions& opt, QWidget* parent) : QMainWindow(parent), opt_(opt) {
    setWindowTitle(opt.live.isEmpty() ? QString("RatSpeak T-Deck Pro simulator - %1").arg(opt.name)
                                      : QString("RatSpeak T-Deck Pro - LIVE device on %1").arg(opt.live));
    {
        std::lock_guard<std::mutex> l(sim::hw().mu);
        sim::hw().serialOut = [](const std::string& line) {
            std::lock_guard<std::mutex> g(g_logMu);
            g_logQueue << QString::fromStdString(line);
        };
    }
    auto* split = new QSplitter(this);
    device_ = new DeviceWidget(split);
    auto* panel = new QWidget(split);
    split->addWidget(device_);
    split->addWidget(panel);
    sidePanel_ = panel;
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    setCentralWidget(split);
    buildPanel(panel);
    buildMenus();
    status_ = new QLabel(this);
    statusBar()->addWidget(status_);
    status_->setText(QString("data: %1").arg(opt.dataDir));
    resize(1100, 920);

    if (!opt.live.isEmpty()) {
        powerBox_->setEnabled(false);
        envBox_->setEnabled(false);
        liveInfo_->setVisible(true);
        device_->setFooter("live: " + opt.live);
        live_ = new LiveLink(opt.live, this);
        connect(live_, &LiveLink::stateChanged, this, [this](const QString& s) {
            liveInfo_->setText(s);
            status_->setText(s);
        });
    }

    auto* t = new QTimer(this);
    connect(t, &QTimer::timeout, this, &MainWindow::drainSerial);
    t->start(50);

    if (!opt.script.isEmpty()) loadScript(opt.script);
    device_->setFocus();
}

void MainWindow::buildPanel(QWidget* panel) {
    auto* v = new QVBoxLayout(panel);

    liveInfo_ = new QLabel("LIVE: connecting");
    liveInfo_->setWordWrap(true);
    liveInfo_->setStyleSheet("QLabel { background: #fff4d6; color: #5a4000; padding: 6px; border-radius: 4px; }");
    liveInfo_->setVisible(false);
    v->addWidget(liveInfo_);

    auto* power = new QGroupBox("Power (BQ27220 gauge / BQ25896 charger)");
    powerBox_ = power;
    auto* pf = new QFormLayout(power);
    soc_ = new QSlider(Qt::Horizontal);
    soc_->setRange(0, 100);
    { std::lock_guard<std::mutex> l(sim::hw().mu); soc_->setValue(sim::hw().batterySoc); }
    socLabel_ = new QLabel();
    auto* socRow = new QHBoxLayout();
    socRow->addWidget(soc_);
    socRow->addWidget(socLabel_);
    pf->addRow("Gauge SOC", socRow);
    usb_ = new QCheckBox("USB connected (charging)");
    full_ = new QCheckBox("Charge done");
    {
        std::lock_guard<std::mutex> l(sim::hw().mu);
        usb_->setChecked(sim::hw().usb);
        full_->setChecked(sim::hw().chargeDone);
    }
    pf->addRow(usb_);
    pf->addRow(full_);
    powerState_ = new QLabel("mode: (booting)");
    powerState_->setWordWrap(true);
    powerState_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    pf->addRow("Now", powerState_);
    connect(soc_, &QSlider::valueChanged, this, &MainWindow::applyBattery);
    connect(usb_, &QCheckBox::toggled, this, &MainWindow::applyBattery);
    connect(full_, &QCheckBox::toggled, this, &MainWindow::applyBattery);
    v->addWidget(power);

    auto* env = new QGroupBox("Environment");
    envBox_ = env;
    auto* ef = new QVBoxLayout(env);
    gps_ = new QCheckBox("GPS fix");
    QString sdText;
    bool sdIn = false;
    {
        std::lock_guard<std::mutex> l(sim::hw().mu);
        gps_->setChecked(sim::hw().gpsFix);
        sdIn = sim::hw().sdPresent;
        sdText = QString("SD card inserted (%1 GB)").arg(sim::hw().sdBytes / double(1ull << 30), 0, 'g', 4);
    }
    sd_ = new QCheckBox(sdText);
    sd_->setChecked(sdIn);
    connect(sd_, &QCheckBox::toggled, this, [](bool on) {
        std::lock_guard<std::mutex> l(sim::hw().mu); sim::hw().sdPresent = on;
    });
    connect(gps_, &QCheckBox::toggled, this, [](bool on) {
        std::lock_guard<std::mutex> l(sim::hw().mu); sim::hw().gpsFix = on;
    });
    realistic_ = new QCheckBox("Real e-ink timing (~650 ms partial refresh)");
    realistic_->setChecked(!opt_.fast);
    connect(realistic_, &QCheckBox::toggled, this, [](bool on) {
        std::lock_guard<std::mutex> l(sim::hw().mu); sim::hw().realisticPanel = on;
    });
    ef->addWidget(gps_);
    ef->addWidget(sd_);
    ef->addWidget(realistic_);
    QString nets;
    {
        std::lock_guard<std::mutex> l(sim::hw().mu);
        for (auto& n : sim::hw().wifiNets)
            nets += QString("%1  %2 dBm  %3\n").arg(QString::fromStdString(n.ssid)).arg(n.rssi)
                        .arg(n.secure ? QString("password: %1").arg(QString::fromStdString(n.password)) : "open");
    }
    auto* wl = new QLabel("WiFi access points:\n" + nets.trimmed());
    wl->setTextInteractionFlags(Qt::TextSelectableByMouse);
    ef->addWidget(wl);
    v->addWidget(env);

    auto* shots = new QHBoxLayout();
    auto* shotScreen = new QPushButton("Screenshot (screen)");
    auto* shotDevice = new QPushButton("Screenshot (device)");
    connect(shotScreen, &QPushButton::clicked, this, [this] {
        saveShot(QDateTime::currentDateTime().toString("'screen-'yyyyMMdd-HHmmss'.png'"), false);
    });
    connect(shotDevice, &QPushButton::clicked, this, [this] {
        saveShot(QDateTime::currentDateTime().toString("'device-'yyyyMMdd-HHmmss'.png'"), true);
    });
    shots->addWidget(shotScreen);
    shots->addWidget(shotDevice);
    v->addLayout(shots);

    auto* help = new QLabel(
        "Keys: letters as-is - Enter - Backspace - Space - Shift (tap = Back/^) - "
        "Esc = Back - Alt = ALT - Ctrl = SYM - Tab/F5 = MIC - digits & symbols are sent as SYM+key. "
        "Click keys with the mouse; right-click latches a key (hold SYM). Click the screen to touch.");
    help->setWordWrap(true);
    v->addWidget(help);

    log_ = new QPlainTextEdit();
    log_->setReadOnly(true);
    log_->setMaximumBlockCount(5000);
    QFont mono("Consolas");
    mono.setStyleHint(QFont::Monospace);
    mono.setPointSize(9);
    log_->setFont(mono);
    v->addWidget(new QLabel("USB serial (115200)"));
    v->addWidget(log_, 1);
    serialIn_ = new QLineEdit();
    serialIn_->setPlaceholderText("send a serial command (e.g. a, h, i, s) + Enter");
    connect(serialIn_, &QLineEdit::returnPressed, this, [this] {
        const std::string s = serialIn_->text().toStdString() + "\n";
        { std::lock_guard<std::mutex> l(sim::hw().mu); for (char c : s) sim::hw().serialIn.push_back(c); }
        serialIn_->clear();
        device_->setFocus();
    });
    v->addWidget(serialIn_);
    applyBattery();
}

// ---- Menu bar: the same hardware controls as the side panel (kept in step with it), the
// two side buttons, screenshots, and the setup file.
namespace {
QString gbText(uint64_t bytes) { return QString("%1 GB").arg(bytes / double(1ull << 30), 0, 'g', 4); }

// A checkable menu item that mirrors a side-panel checkbox both ways.
QAction* mirror(QMenu* m, const QString& text, QCheckBox* box) {
    QAction* a = m->addAction(text);
    a->setCheckable(true);
    a->setChecked(box->isChecked());
    QObject::connect(a, &QAction::toggled, box, &QCheckBox::setChecked);
    QObject::connect(box, &QCheckBox::toggled, a, &QAction::setChecked);
    return a;
}
}  // namespace

void MainWindow::buildMenus() {
    const bool live = !opt_.live.isEmpty();
    QMenuBar* mb = menuBar();

    // Device: side buttons, screenshots, setup file
    QMenu* dev = mb->addMenu("&Device");
    QAction* reset = dev->addAction("&Reset (bottom side button)", this, &MainWindow::resetDevice);
    reset->setShortcut(QKeySequence("Ctrl+R"));
    reset->setToolTip("Restart the simulated device like a power cut: unsaved data is lost.");
    QAction* boot = dev->addAction("Hold &BOOT (top side button)");
    boot->setCheckable(true);
    connect(boot, &QAction::toggled, this, [](bool on) {
        std::lock_guard<std::mutex> l(sim::hw().mu); sim::hw().bootHeld = on;
    });
    dev->addSeparator();
    dev->addAction("Screenshot: &screen", this, [this] {
        saveShot(QDateTime::currentDateTime().toString("'screen-'yyyyMMdd-HHmmss'.png'"), false);
    })->setShortcut(QKeySequence("Ctrl+P"));
    dev->addAction("Screenshot: &device", this, [this] {
        saveShot(QDateTime::currentDateTime().toString("'device-'yyyyMMdd-HHmmss'.png'"), true);
    });
    dev->addAction("Open &data folder", this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(opt_.dataDir));
    });
    dev->addSeparator();
    dev->addAction("Save this setup as the device's &default (sim.ini)", this, &MainWindow::saveSetup);
    dev->addSeparator();
    dev->addAction("&Quit", this, [] { fflush(stdout); _exit(0); })->setShortcut(QKeySequence("Ctrl+Q"));
    if (live) { reset->setEnabled(false); boot->setEnabled(false); }

    // Power: battery level, USB, charge done
    QMenu* pw = mb->addMenu("&Power");
    QMenu* lvl = pw->addMenu("&Battery level");
    for (int pct : {100, 90, 75, 61, 50, 25, 15, 10, 5, 1, 0})
        lvl->addAction(QString("%1 %").arg(pct), this, [this, pct] { soc_->setValue(pct); });
    pw->addSeparator();
    mirror(pw, "&USB connected (charging)", usb_);
    mirror(pw, "Charge &done", full_);
    pw->setEnabled(!live);

    // SD card: in / out, size
    QMenu* sdm = mb->addMenu("&SD card");
    mirror(sdm, "&Inserted", sd_);
    QMenu* size = sdm->addMenu("Card &size (seen at the next mount or reset)");
    auto* sizes = new QActionGroup(size);
    uint64_t cur;
    { std::lock_guard<std::mutex> l(sim::hw().mu); cur = sim::hw().sdBytes; }
    for (int gb : {1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024}) {
        const uint64_t b = (uint64_t)gb << 30;
        QAction* a = size->addAction(gb >= 1024 ? "1 TB" : QString("%1 GB").arg(gb), this, [this, b] { setSdSize(b); });
        a->setCheckable(true);
        a->setChecked(b == cur);
        sizes->addAction(a);
    }
    sdm->addAction("&Open card folder", this, [this] {
        const QString p = QDir(opt_.dataDir).filePath("sd");
        QDir().mkpath(p);
        QDesktopServices::openUrl(QUrl::fromLocalFile(p));
    });
    sdm->setEnabled(!live);

    // GPS: fix, position, satellites
    QMenu* gm = mb->addMenu("&GPS");
    mirror(gm, "&Fix", gps_);
    gm->addAction("Set &position...", this, [this] {
        double lat, lon;
        { std::lock_guard<std::mutex> l(sim::hw().mu); lat = sim::hw().lat; lon = sim::hw().lon; }
        bool ok = false;
        const QString s = QInputDialog::getText(this, "GPS position", "Latitude, longitude (decimal degrees):",
                                                QLineEdit::Normal, QString("%1, %2").arg(lat, 0, 'f', 5).arg(lon, 0, 'f', 5), &ok);
        if (!ok) return;
        const QStringList ll = s.split(',');
        bool a = false, b = false;
        const double nlat = ll.size() == 2 ? ll[0].trimmed().toDouble(&a) : 0;
        const double nlon = ll.size() == 2 ? ll[1].trimmed().toDouble(&b) : 0;
        if (!a || !b || nlat < -90 || nlat > 90 || nlon < -180 || nlon > 180) {
            QMessageBox::warning(this, "GPS position", "Type two numbers, e.g. 43.556, -70.711");
            return;
        }
        { std::lock_guard<std::mutex> l(sim::hw().mu); sim::hw().lat = nlat; sim::hw().lon = nlon; }
        gps_->setChecked(true);
        status_->setText(QString("GPS position %1, %2").arg(nlat, 0, 'f', 5).arg(nlon, 0, 'f', 5));
    });
    QMenu* sm = gm->addMenu("&Satellites");
    auto* satGroup = new QActionGroup(sm);
    int curSats;
    { std::lock_guard<std::mutex> l(sim::hw().mu); curSats = sim::hw().sats; }
    for (int n : {0, 3, 4, 5, 7, 9, 12}) {
        QAction* a = sm->addAction(QString::number(n), this, [n] {
            std::lock_guard<std::mutex> l(sim::hw().mu); sim::hw().sats = n;
        });
        a->setCheckable(true);
        a->setChecked(n == curSats);
        satGroup->addAction(a);
    }
    gm->setEnabled(!live);

    // View
    QMenu* vw = mb->addMenu("&View");
    QAction* side = vw->addAction("&Side panel");
    side->setCheckable(true);
    side->setChecked(true);
    connect(side, &QAction::toggled, sidePanel_, &QWidget::setVisible);
    mirror(vw, "Real e-ink &timing (~650 ms refresh)", realistic_);

    // Help
    QMenu* hm = mb->addMenu("&Help");
    hm->addAction("&Keys", this, [this] {
        QMessageBox::information(this, "Keys",
            "PC keyboard: letters as they are, Enter, Backspace, Space.\n"
            "Shift tapped alone = Back (^); Esc = Back.\n"
            "Alt = ALT, Ctrl = SYM, Tab or F5 = MIC.\n"
            "Digits and symbols are sent as SYM + key.\n\n"
            "Mouse: click the keys; right-click latches a key (hold SYM).\n"
            "Click the screen to touch it.");
    });
    hm->addAction("&Start-up options", this, [this] {
        QMessageBox::information(this, "Start-up options",
            "ratspeak-sim --name field --sd 32G --battery 61 --usb --gps nofix\n\n"
            "--sd 32G | none     SD card size, or an empty slot\n"
            "--battery 61        charge in percent\n"
            "--usb  --charged    on USB power / charging finished\n"
            "--gps fix | nofix | LAT,LON\n"
            "--sats 9            satellites in the fix\n"
            "--config FILE       the same as key = value lines\n\n"
            "Each device also reads simdata\\<name>\\sim.ini at start\n"
            "(Device > Save this setup writes it).");
    });
}

void MainWindow::setSdSize(uint64_t bytes) {
    { std::lock_guard<std::mutex> l(sim::hw().mu); sim::hw().sdBytes = bytes; }
    sd_->setText("SD card inserted (" + gbText(bytes) + ")");
    status_->setText("SD card size " + gbText(bytes) + ": the firmware sees it at its next mount (or Reset)");
}

void MainWindow::saveSetup() {
    QString text;
    {
        std::lock_guard<std::mutex> l(sim::hw().mu);
        const auto& h = sim::hw();
        const uint64_t gb = h.sdBytes >> 30;
        text = QString("# Hardware setup of simulated device \"%1\" (Device > Save this setup).\n"
                       "# Read at every start; command-line options override it.\n"
                       "sd = %2\nbattery = %3\nusb = %4\ncharged = %5\ngps = %6\nsats = %7\nfast = %8\n")
                   .arg(opt_.name)
                   .arg(!h.sdPresent ? QString("none") : (h.sdBytes == gb << 30 ? QString("%1G").arg(gb)
                                                                                  : QString("%1M").arg(h.sdBytes >> 20)))
                   .arg(h.batterySoc)
                   .arg(h.usb ? "on" : "off")
                   .arg(h.chargeDone ? "on" : "off")
                   .arg(h.gpsFix ? QString("%1,%2").arg(h.lat, 0, 'f', 5).arg(h.lon, 0, 'f', 5) : QString("nofix"))
                   .arg(h.sats)
                   .arg(h.realisticPanel ? "off" : "on");
        if (h.flashBytes != 0x7E0000) text += QString("flashkb = %1\n").arg(h.flashBytes / 1024);
    }
    const QString path = QDir(opt_.dataDir).filePath("sim.ini");
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        QMessageBox::warning(this, "Save setup", "Cannot write " + path);
        return;
    }
    f.write(text.toUtf8());
    status_->setText("saved " + path);
}

void MainWindow::resetDevice() {
    // Like the RESET button: the device restarts from flash; whatever the firmware had not
    // written yet is lost. The simulator restarts itself with the same options.
    QStringList args = QCoreApplication::arguments().mid(1);
    for (int i = 0; i < args.size(); i++) {      // a script is not run again after a reset
        if (args[i] == "--quit") { args.removeAt(i--); continue; }
        if (args[i] == "--script") { args.removeAt(i); if (i < args.size()) args.removeAt(i); i--; continue; }
        if (args[i].startsWith("--script=")) args.removeAt(i--);
    }
    if (!QProcess::startDetached(QCoreApplication::applicationFilePath(), args, QDir::currentPath())) {
        QMessageBox::warning(this, "Reset", "Could not restart the simulator.");
        return;
    }
    fprintf(stdout, "[SIM] RESET button: restarting\n");
    fflush(stdout);
    _exit(0);
}

void MainWindow::applyBattery() {
    const int soc = soc_->value();
    socLabel_->setText(QString("%1%").arg(soc));
    std::lock_guard<std::mutex> l(sim::hw().mu);
    auto& h = sim::hw();
    h.batterySoc = soc;
    h.usb = usb_->isChecked();
    h.chargeDone = h.usb && full_->isChecked();
    h.charging = h.usb && !h.chargeDone;
    h.batteryMv = socToMv(soc) + (h.charging ? 60 : 0);
    h.batteryMa = h.charging ? 420 : h.chargeDone ? 15 : -85;
}

void MainWindow::drainSerial() {
    if (powerState_) {
        std::string info;
        { std::lock_guard<std::mutex> l(sim::hw().mu); info = sim::hw().powerInfo; }
        if (!info.empty()) {
            const QString text = QString::fromStdString(info);
            if (powerState_->text() != text) powerState_->setText(text);
        }
    }
    QStringList lines;
    { std::lock_guard<std::mutex> g(g_logMu); lines.swap(g_logQueue); }
    lines.erase(std::remove_if(lines.begin(), lines.end(), isMirrorLine), lines.end());
    if (lines.isEmpty()) return;
    for (const auto& s : lines) log_->appendPlainText(s);
    lastLines_ += lines.join('\n') + '\n';
    if (lastLines_.size() > 200000) lastLines_ = lastLines_.right(100000);
}

void MainWindow::saveShot(const QString& file, bool wholeDevice, bool window) {
    const QString dir = opt_.shotDir.isEmpty() ? QDir(opt_.dataDir).filePath("shots") : opt_.shotDir;
    QDir().mkpath(dir);
    const QString path = QDir(dir).filePath(file);
    if (window) drainSerial();   // fresh side-panel state
    const QImage img = window ? grab().toImage() : wholeDevice ? device_->deviceImage() : device_->screenImage(2);
    const bool ok = img.save(path);
    status_->setText(ok ? "saved " + path : "FAILED to save " + path);
    fprintf(stdout, "[SIM] screenshot %s %s\n", ok ? "saved" : "FAILED", path.toLocal8Bit().constData());
    fflush(stdout);
}

// ---- Script runner -------------------------------------------------------------------
// One command per line (see sim/README.md): wait, key, down, up, type, touch, shot,
// devshot, winshot, battery, usb, full, gps, serial, expect, quit. '#' starts a comment.
void MainWindow::loadScript(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        fprintf(stderr, "[SCRIPT] cannot open %s\n", path.toLocal8Bit().constData());
        if (opt_.quitAfterScript) QTimer::singleShot(0, [] { QApplication::exit(2); });
        return;
    }
    QTextStream in(&f);
    while (!in.atEnd()) script_ << in.readLine();
    scriptTimer_ = new QTimer(this);
    scriptTimer_->setSingleShot(true);
    connect(scriptTimer_, &QTimer::timeout, this, &MainWindow::stepScript);
    scriptTimer_->start(500);
}

void MainWindow::stepScript() {
    static uint32_t expectDeadline = 0;
    static QString expectText;
    auto next = [this](int ms) { scriptTimer_->start(ms); };
    auto fail = [this](const QString& why) {
        fprintf(stdout, "[SCRIPT] FAIL line %d: %s\n", scriptPos_, why.toLocal8Bit().constData());
        fflush(stdout);
        if (opt_.quitAfterScript) QApplication::exit(1);
    };

    if (!expectText.isEmpty()) {                  // waiting on serial output
        drainSerial();
        const int at = lastLines_.indexOf(expectText);
        if (at >= 0) {
            lastLines_ = lastLines_.mid(at + expectText.size());
            expectText.clear();
            next(20);
        } else if ((int32_t)(sim::nowMs() - expectDeadline) >= 0) {
            fail("expected serial text not seen: " + expectText);
            expectText.clear();
        } else {
            next(50);
        }
        return;
    }
    while (scriptPos_ < script_.size()) {
        QString line = script_[scriptPos_++].trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;
        const QString cmd = line.section(' ', 0, 0).toLower();
        const QString arg = line.section(' ', 1).trimmed();
        fprintf(stdout, "[SCRIPT] %s\n", line.toLocal8Bit().constData());
        fflush(stdout);
        if (cmd == "wait") { next(arg.toInt()); return; }
        if (cmd == "key") { device_->tapKey(arg); next(80); return; }
        if (cmd == "down") { device_->pressKey(arg, true); next(30); return; }
        if (cmd == "up") { device_->pressKey(arg, false); next(30); return; }
        if (cmd == "type") {
            // Keyboard boots in caps lock: upper-case letters are plain taps, lower-case
            // letters are sent as ALT+letter (ALT flips case), digits/symbols via SYM.
            for (QChar c : arg) {
                if (c == ' ') device_->tapKey("SPACE");
                else if (c.isUpper()) device_->tapKey(QString(c));
                else if (c.isLower()) device_->tapKey("ALT+" + QString(c.toUpper()));
                else device_->tapKey(QString(c));
            }
            next(80);
            return;
        }
        if (cmd == "touch") { device_->touch(arg.section(' ', 0, 0).toInt(), arg.section(' ', 1, 1).toInt()); next(200); return; }
        if (cmd == "shot") { saveShot(arg, false); next(20); return; }
        if (cmd == "devshot") { saveShot(arg, true); next(20); return; }
        if (cmd == "winshot") { saveShot(arg, false, true); next(20); return; }
        if (cmd == "battery") { soc_->setValue(arg.toInt()); next(20); return; }
        if (cmd == "usb") { usb_->setChecked(arg == "on"); next(20); return; }
        if (cmd == "full") { full_->setChecked(arg == "on"); next(20); return; }
        if (cmd == "gps") { gps_->setChecked(arg == "on"); next(20); return; }
        if (cmd == "menu") {  // "menu SD card > Card size > 32 GB": run a menu item (prefix match, no &)
            const QStringList path = arg.split('>');
            QList<QAction*> items = menuBar()->actions();
            QAction* hit = nullptr;
            for (int i = 0; i < path.size(); i++) {
                const QString want = path[i].trimmed().toLower();
                hit = nullptr;
                for (QAction* a : items)
                    if (QString(a->text()).remove('&').toLower().startsWith(want)) { hit = a; break; }
                if (!hit) break;
                if (i + 1 < path.size()) {
                    if (!hit->menu()) { hit = nullptr; break; }
                    items = hit->menu()->actions();
                }
            }
            if (!hit || hit->menu()) { fail("no menu item \"" + arg + "\""); return; }
            fprintf(stdout, "[SIM] menu %s\n", arg.toLocal8Bit().constData());
            fflush(stdout);
            hit->trigger();
            next(50);
            return;
        }
        if (cmd == "sd") {   // "sd in" / "sd out": the firmware sees it at its next mount
            sd_->setChecked(arg == "in");
            next(20); return;
        }
        if (cmd == "serial") {
            const std::string s = arg.toStdString() + "\n";
            std::lock_guard<std::mutex> l(sim::hw().mu);
            for (char c : s) sim::hw().serialIn.push_back(c);
            next(20);
            return;
        }
        if (cmd == "expect") {
            // expect <ms> <text>
            expectDeadline = sim::nowMs() + (uint32_t)arg.section(' ', 0, 0).toInt();
            expectText = arg.section(' ', 1);
            next(20);
            return;
        }
        if (cmd == "quit") {
            fprintf(stdout, "[SCRIPT] PASS\n");
            fflush(stdout);
            QApplication::exit(0);
            return;
        }
        fail("unknown command: " + cmd);
        return;
    }
    fprintf(stdout, "[SCRIPT] PASS (end of script)\n");
    fflush(stdout);
    if (opt_.quitAfterScript) QApplication::exit(0);
}
