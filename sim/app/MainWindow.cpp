#include "app/MainWindow.h"

#include <QApplication>
#include <QCheckBox>
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
#include <mutex>

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
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    setCentralWidget(split);
    buildPanel(panel);
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
    gps_->setChecked(true);
    connect(gps_, &QCheckBox::toggled, this, [](bool on) {
        std::lock_guard<std::mutex> l(sim::hw().mu); sim::hw().gpsFix = on;
    });
    realistic_ = new QCheckBox("Real e-ink timing (~650 ms partial refresh)");
    realistic_->setChecked(!opt_.fast);
    connect(realistic_, &QCheckBox::toggled, this, [](bool on) {
        std::lock_guard<std::mutex> l(sim::hw().mu); sim::hw().realisticPanel = on;
    });
    ef->addWidget(gps_);
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
        if (cmd == "sd") {   // "sd in" / "sd out": the firmware sees it at its next mount
            std::lock_guard<std::mutex> l(sim::hw().mu); sim::hw().sdPresent = arg == "in";
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
