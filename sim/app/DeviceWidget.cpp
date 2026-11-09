#include "app/DeviceWidget.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>

#include "hw/SimHw.h"

namespace {
// Device drawing units: 1 unit = 1 panel pixel.
constexpr double kDevW = 280, kDevH = 560;
const QRectF kScreen(20, 40, 240, 320);
const QRectF kBezel(10, 28, 260, 344);
constexpr double kKbY = 390, kKeyW = 23, kKeyH = 30, kGap = 3, kPitch = 36;

const QColor kBody(0x2a, 0x2c, 0x30), kBodyEdge(0x45, 0x48, 0x4e);
const QColor kBezelC(0xf3, 0xf2, 0xee), kBezelEdge(0xc9, 0xc8, 0xc2);   // white-ish, like the device
const QColor kPaper(0xe9, 0xe7, 0xdf), kInk(0x1c, 0x1c, 0x1c);
const QColor kCap(0x3b, 0x3e, 0x44), kCapDown(0x6a, 0x6f, 0x78), kLegend(0xf2, 0xf2, 0xf2);
const QColor kSym(0xf0, 0xa0, 0x40), kGlow(0x9f, 0xd4, 0xff);

struct KeyDef { const char* label; const char* sym; };
// Left-to-right as printed on the device; matrix column = 9 - position (firmware kMap).
const KeyDef kRow0[10] = {{"Q", "#"}, {"W", "1"}, {"E", "2"}, {"R", "3"}, {"T", "("},
                          {"Y", ")"}, {"U", "-"}, {"I", "_"}, {"O", "+"}, {"P", "@"}};
const KeyDef kRow1[10] = {{"A", "*"}, {"S", "4"}, {"D", "5"}, {"F", "6"}, {"G", "/"},
                          {"H", ":"}, {"J", ";"}, {"K", "'"}, {"L", "\""}, {"⌫", ""}};
const KeyDef kRow2[10] = {{"ALT", ""}, {"Z", "7"}, {"X", "8"}, {"C", "9"}, {"V", "?"},
                          {"B", "!"}, {"N", "'"}, {"M", "`"}, {"$", "$"}, {"↵", ""}};

// PC characters typed via the SYM layer: char -> device letter.
QString symLetterFor(QChar c) {
    static const char* map[][2] = {{"#", "Q"}, {"1", "W"}, {"2", "E"}, {"3", "R"}, {"(", "T"}, {")", "Y"},
                                   {"-", "U"}, {"_", "I"}, {"+", "O"}, {"@", "P"}, {"*", "A"}, {"4", "S"},
                                   {"5", "D"}, {"6", "F"}, {"/", "G"}, {":", "H"}, {";", "J"}, {"'", "K"},
                                   {"\"", "L"}, {"7", "Z"}, {"8", "X"}, {"9", "C"}, {"?", "V"}, {"!", "B"},
                                   {"`", "M"}, {"0", "MIC"}};
    for (auto& m : map) if (c == QChar(m[0][0])) return QString(m[1]);
    return QString();
}
}  // namespace

DeviceWidget::DeviceWidget(QWidget* parent) : QWidget(parent) {
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(280, 560);
    auto addRow = [&](const KeyDef* defs, int row, double y) {
        const double x0 = (kDevW - (10 * kKeyW + 9 * kGap)) / 2;
        for (int i = 0; i < 10; i++)
            keys_.push_back({(uint8_t)row, (uint8_t)(9 - i), QString::fromUtf8(defs[i].label),
                             QString::fromUtf8(defs[i].sym), QRectF(x0 + i * (kKeyW + kGap), y, kKeyW, kKeyH)});
    };
    addRow(kRow0, 0, kKbY);
    addRow(kRow1, 1, kKbY + kPitch);
    addRow(kRow2, 2, kKbY + 2 * kPitch);
    // Bottom row: LSHIFT(col4) MIC(col3) SPACE(col2) SYM(col1) RSHIFT(col0)
    const double y3 = kKbY + 3 * kPitch, x0 = (kDevW - (10 * kKeyW + 9 * kGap)) / 2;
    const double mod = 36, space = (10 * kKeyW + 9 * kGap) - 4 * mod - 4 * kGap;
    double x = x0;
    auto add3 = [&](uint8_t col, const char* label, double w) {
        keys_.push_back({3, col, QString::fromUtf8(label), QString(), QRectF(x, y3, w, kKeyH)});
        x += w + kGap;
    };
    add3(4, "⇧", mod);
    add3(3, "MIC", mod);
    add3(2, "SPACE", space);
    add3(1, "SYM", mod);
    add3(0, "⇧", mod);
    panel_ = QImage(240, 320, QImage::Format_RGB32);
    panel_.fill(kPaper);
    startTimer(30);
}

QSize DeviceWidget::sizeHint() const { return QSize(448, 896); }

QTransform DeviceWidget::devToWidget() const {
    const double s = std::min(width() / kDevW, height() / kDevH);
    QTransform t;
    t.translate((width() - kDevW * s) / 2, (height() - kDevH * s) / 2);
    t.scale(s, s);
    return t;
}

void DeviceWidget::pullPanel() {
    auto& h = sim::hw();
    std::vector<uint8_t> fb;
    {
        std::lock_guard<std::mutex> l(h.mu);
        if (h.panelVersion == panelVersion_) return;
        panelVersion_ = h.panelVersion;
        fb = h.panel;
        lastRefreshMs_ = h.lastRefreshMs;
        lastFull_ = h.lastRefreshFull;
    }
    for (int y = 0; y < 320; y++) {
        auto* line = reinterpret_cast<QRgb*>(panel_.scanLine(y));
        for (int x = 0; x < 240; x++) {
            const bool black = fb[(y * 240 + x) / 8] & (0x80 >> (x % 8));
            line[x] = black ? kInk.rgb() : kPaper.rgb();
        }
    }
    update();
}

void DeviceWidget::timerEvent(QTimerEvent*) {
    pullPanel();
    bool bl, vib;
    uint32_t beeps;
    {
        auto& h = sim::hw();
        std::lock_guard<std::mutex> l(h.mu);
        bl = h.kbBacklight;
        vib = (int32_t)(h.vibrateUntil - sim::nowMs()) > 0;
        beeps = h.beepCount;
    }
    if (beeps != beepSeen_) { beepSeen_ = beeps; beepFlashUntil_ = sim::nowMs() + 400; update(); }
    const bool flash = (int32_t)(beepFlashUntil_ - sim::nowMs()) > 0;
    static bool lastFlash = false;
    if (bl != backlight_ || vib != vibrating_ || flash != lastFlash) {
        backlight_ = bl; vibrating_ = vib; lastFlash = flash;
        update();
    }
}

void DeviceWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    QTransform t = devToWidget();
    if (vibrating_) t.translate((sim::nowMs() / 16 % 2) ? 1.2 : -1.2, 0);
    p.setTransform(t);

    // Body
    p.setPen(QPen(kBodyEdge, 2));
    p.setBrush(kBody);
    p.drawRoundedRect(QRectF(2, 2, kDevW - 4, kDevH - 4), 22, 22);
    // Speaker slot + status LEDs
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x18, 0x19, 0x1b));
    p.drawRoundedRect(QRectF(kDevW / 2 - 28, 12, 56, 6), 3, 3);
    if ((int32_t)(beepFlashUntil_ - sim::nowMs()) > 0) {
        p.setBrush(QColor(0x60, 0xd0, 0x80));
        p.drawEllipse(QPointF(kDevW / 2 + 40, 15), 3, 3);
    }
    // Bezel + screen
    p.setBrush(kBezelC);
    p.drawRoundedRect(kBezel, 8, 8);
    p.drawImage(kScreen, panel_);
    p.setPen(QPen(kBezelEdge, 0.8));
    p.setBrush(Qt::NoBrush);
    p.drawRect(kScreen.adjusted(-0.5, -0.5, 0.5, 0.5));   // panel edge against the light bezel

    // Keyboard
    QFont f = font();
    for (const auto& k : keys_) {
        p.setPen(Qt::NoPen);
        p.setBrush(k.held ? kCapDown : kCap);
        p.drawRoundedRect(k.rect, 5, 5);
        const bool wide = k.label.size() > 2;
        f.setPixelSize(wide ? 8 : 12);
        f.setBold(!wide);
        p.setFont(f);
        p.setPen(backlight_ ? kGlow : kLegend);
        p.drawText(k.rect.adjusted(0, k.sym.isEmpty() ? 0 : 4, 0, 0), Qt::AlignCenter, k.label);
        if (!k.sym.isEmpty()) {
            f.setPixelSize(7); f.setBold(false); p.setFont(f);
            p.setPen(kSym);
            p.drawText(k.rect.adjusted(2, 1, -2, 0), Qt::AlignRight | Qt::AlignTop, k.sym);
        }
    }
    f.setPixelSize(7); f.setBold(false); p.setFont(f);
    p.setPen(QColor(0x80, 0x84, 0x8a));
    p.drawText(QRectF(0, kDevH - 18, kDevW, 10), Qt::AlignCenter,
               !footer_.isEmpty() ? footer_
                                  : QString("panel %1 %2 ms").arg(lastFull_ ? "full" : "partial").arg(lastRefreshMs_));
}

QImage DeviceWidget::screenImage(int scale) const {
    return panel_.scaled(240 * scale, 320 * scale, Qt::IgnoreAspectRatio, Qt::FastTransformation);
}

QImage DeviceWidget::deviceImage() {
    QImage img(size() * devicePixelRatioF(), QImage::Format_ARGB32_Premultiplied);
    img.setDevicePixelRatio(devicePixelRatioF());
    img.fill(Qt::transparent);
    render(&img, QPoint(), QRegion(), QWidget::DrawChildren);
    return img;
}

SimKey* DeviceWidget::keyAt(const QPointF& dev) {
    for (auto& k : keys_) if (k.rect.contains(dev)) return &k;
    return nullptr;
}

SimKey* DeviceWidget::keyByName(const QString& name) {
    const QString n = name.toUpper();
    auto byPos = [&](int row, int col) -> SimKey* {
        for (auto& k : keys_) if (k.row == row && k.col == col) return &k;
        return nullptr;
    };
    if (n == "ENTER" || n == "RETURN") return byPos(2, 0);
    if (n == "BKSP" || n == "BACKSPACE" || n == "DEL") return byPos(1, 0);
    if (n == "ALT") return byPos(2, 9);
    if (n == "SPACE") return byPos(3, 2);
    if (n == "MIC") return byPos(3, 3);
    if (n == "SYM") return byPos(3, 1);
    if (n == "SHIFT" || n == "LSHIFT" || n == "BACK" || n == "^") return byPos(3, 4);
    if (n == "RSHIFT") return byPos(3, 0);
    if (n == "$") return byPos(2, 1);
    for (auto& k : keys_) if (k.row < 3 && k.label == n) return &k;
    return nullptr;
}

void DeviceWidget::sendKey(SimKey* k, bool down) {
    if (!k || k->held == down) return;
    k->held = down;
    {
        std::lock_guard<std::mutex> l(sim::hw().mu);
        sim::hw().keyEvents.push_back({k->row, k->col, down});
    }
    update();
}

void DeviceWidget::pressKey(const QString& name, bool down) { sendKey(keyByName(name), down); }

void DeviceWidget::tapKey(const QString& specIn) {
    // "SYM+W", "ALT+Q", "Q"; a lone digit/symbol ("2", "@") is sent as its SYM combo.
    QString spec = specIn;
    if (spec.size() == 1) {
        const QString letter = symLetterFor(spec[0]);
        if (!letter.isEmpty()) spec = "SYM+" + letter;
    }
    const QStringList parts = spec.split('+', Qt::SkipEmptyParts);
    if (parts.isEmpty()) return;
    std::vector<SimKey*> mods;
    for (int i = 0; i + 1 < parts.size(); i++) if (auto* m = keyByName(parts[i])) mods.push_back(m);
    SimKey* k = keyByName(parts.last());
    if (!k) return;
    std::lock_guard<std::mutex> l(sim::hw().mu);
    auto& q = sim::hw().keyEvents;
    for (auto* m : mods) q.push_back({m->row, m->col, true});
    q.push_back({k->row, k->col, true});
    q.push_back({k->row, k->col, false});
    for (auto it = mods.rbegin(); it != mods.rend(); ++it) q.push_back({(*it)->row, (*it)->col, false});
}

void DeviceWidget::touch(int x, int y) {
    {
        std::lock_guard<std::mutex> l(sim::hw().mu);
        sim::hw().touchX = x; sim::hw().touchY = y; sim::hw().touchDown = true;
    }
    QTimer::singleShot(150, this, [] { std::lock_guard<std::mutex> l(sim::hw().mu); sim::hw().touchDown = false; });
}

void DeviceWidget::mousePressEvent(QMouseEvent* e) {
    setFocus();
    const QPointF dev = devToWidget().inverted().map(e->position());
    if (kScreen.contains(dev)) {
        std::lock_guard<std::mutex> l(sim::hw().mu);
        sim::hw().touchX = (int)(dev.x() - kScreen.x());
        sim::hw().touchY = (int)(dev.y() - kScreen.y());
        sim::hw().touchDown = true;
        mouseTouch_ = true;
        return;
    }
    SimKey* k = keyAt(dev);
    if (!k) return;
    if (e->button() == Qt::RightButton) { sendKey(k, !k->held); return; }   // latch (e.g. hold SYM)
    mouseKey_ = k;
    sendKey(k, true);
}

void DeviceWidget::mouseReleaseEvent(QMouseEvent* e) {
    if (mouseTouch_) {
        mouseTouch_ = false;
        std::lock_guard<std::mutex> l(sim::hw().mu);
        sim::hw().touchDown = false;
        return;
    }
    if (e->button() == Qt::LeftButton && mouseKey_) { sendKey(mouseKey_, false); mouseKey_ = nullptr; }
}

void DeviceWidget::keyPressEvent(QKeyEvent* e) {
    if (e->isAutoRepeat()) return;
    const int k = e->key();
    if (k >= Qt::Key_A && k <= Qt::Key_Z) { pressKey(QString(QChar('A' + (k - Qt::Key_A))), true); return; }
    switch (k) {
        case Qt::Key_Return: case Qt::Key_Enter: pressKey("ENTER", true); return;
        case Qt::Key_Backspace: case Qt::Key_Delete: pressKey("BKSP", true); return;
        case Qt::Key_Space: pressKey("SPACE", true); return;
        case Qt::Key_Shift: pressKey("SHIFT", true); return;
        case Qt::Key_Alt: pressKey("ALT", true); return;
        case Qt::Key_Control: pressKey("SYM", true); return;
        case Qt::Key_Tab: case Qt::Key_F5: pressKey("MIC", true); return;
        case Qt::Key_Escape: tapKey("SHIFT"); return;                 // lone Shift = Back
        case Qt::Key_Dollar: tapKey("$"); return;
        default: break;
    }
    const QString t = e->text();
    if (t.size() == 1) {
        const QString letter = symLetterFor(t[0]);
        if (!letter.isEmpty()) tapKey("SYM+" + letter);
    }
}

void DeviceWidget::keyReleaseEvent(QKeyEvent* e) {
    if (e->isAutoRepeat()) return;
    const int k = e->key();
    if (k >= Qt::Key_A && k <= Qt::Key_Z) { pressKey(QString(QChar('A' + (k - Qt::Key_A))), false); return; }
    switch (k) {
        case Qt::Key_Return: case Qt::Key_Enter: pressKey("ENTER", false); return;
        case Qt::Key_Backspace: case Qt::Key_Delete: pressKey("BKSP", false); return;
        case Qt::Key_Space: pressKey("SPACE", false); return;
        case Qt::Key_Shift: pressKey("SHIFT", false); return;
        case Qt::Key_Alt: pressKey("ALT", false); return;
        case Qt::Key_Control: pressKey("SYM", false); return;
        case Qt::Key_Tab: case Qt::Key_F5: pressKey("MIC", false); return;
        default: break;
    }
}
