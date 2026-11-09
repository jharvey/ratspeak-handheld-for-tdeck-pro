#include "app/DeviceWidget.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>

#include "hw/SimHw.h"

namespace {
// Device drawing units: 1 unit = 1 panel pixel.
constexpr double kDevW = 280, kDevH = 600;
const QRectF kScreen(20, 40, 240, 320);
const QRectF kBezel(10, 28, 260, 344);
constexpr double kKbY = 390, kKeyW = 23, kKeyH = 30, kGap = 3, kPitch = 36;

// Colours from the device photo: frosted light-grey case, light screen surround,
// black keys with white legends.
const QColor kBody(0xd3, 0xd6, 0xd9), kBodyEdge(0xa9, 0xad, 0xb2), kBodyShine(0xec, 0xee, 0xf0);
const QColor kBezelC(0xe4, 0xe6, 0xe8), kBezelEdge(0xb4, 0xb8, 0xbc), kGlassEdge(0x9a, 0x9e, 0xa3);
const QColor kPlate(0xbf, 0xc3, 0xc7), kTrace(0x9c, 0xa1, 0xa7);
const QColor kPaper(0xe9, 0xe7, 0xdf), kInk(0x1c, 0x1c, 0x1c);
const QColor kCap(0x17, 0x18, 0x1a), kCapDown(0x50, 0x53, 0x58), kLegend(0xf4, 0xf4, 0xf4);
const QColor kSym(0xc8, 0xcb, 0xcf), kGlow(0x9f, 0xd4, 0xff);

struct KeyDef { const char* label; const char* sym; };
// Left-to-right as printed on the device; matrix column = 9 - position (firmware kMap).
const KeyDef kRow0[10] = {{"Q", "#"}, {"W", "1"}, {"E", "2"}, {"R", "3"}, {"T", "("},
                          {"Y", ")"}, {"U", "-"}, {"I", "_"}, {"O", "+"}, {"P", "@"}};
const KeyDef kRow1[10] = {{"A", "*"}, {"S", "4"}, {"D", "5"}, {"F", "6"}, {"G", "/"},
                          {"H", ":"}, {"J", ";"}, {"K", "'"}, {"L", "\""}, {"⌫", ""}};
const KeyDef kRow2[10] = {{"alt", ""}, {"Z", "7"}, {"X", "8"}, {"C", "9"}, {"V", "?"},
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
    setMinimumSize(280, 600);
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
    auto add3 = [&](uint8_t col, const char* label, double w, const char* sym = "") {
        keys_.push_back({3, col, QString::fromUtf8(label), QString::fromUtf8(sym), QRectF(x, y3, w, kKeyH)});
        x += w + kGap;
    };
    add3(4, "⇧", mod);
    add3(3, "MIC", mod, "0");
    add3(2, "SPACE", space);
    add3(1, "SYM", mod);
    add3(0, "⇧", mod);
    panel_ = QImage(240, 320, QImage::Format_RGB32);
    panel_.fill(kPaper);
    startTimer(30);
}

QSize DeviceWidget::sizeHint() const { return QSize(448, 960); }

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

    // Body: frosted light-grey case with a soft top highlight
    QLinearGradient bodyGrad(0, 0, 0, kDevH);
    bodyGrad.setColorAt(0, kBodyShine);
    bodyGrad.setColorAt(0.08, kBody);
    bodyGrad.setColorAt(1, kBody.darker(104));
    p.setPen(QPen(kBodyEdge, 1.6));
    p.setBrush(bodyGrad);
    p.drawRoundedRect(QRectF(2, 2, kDevW - 4, kDevH - 4), 24, 24);
    // Screen module: a lighter raised surround
    p.setPen(QPen(kBezelEdge, 0.8));
    p.setBrush(kBezelC);
    p.drawRoundedRect(QRectF(8, 10, kDevW - 16, 368), 18, 18);
    // Beep indicator
    p.setPen(Qt::NoPen);
    if ((int32_t)(beepFlashUntil_ - sim::nowMs()) > 0) {
        p.setBrush(QColor(0x40, 0xb0, 0x60));
        p.drawEllipse(QPointF(kDevW / 2, 24), 3, 3);
    }
    // E-ink glass + panel
    p.setPen(QPen(kGlassEdge, 1.0));
    p.setBrush(kPaper);
    p.drawRect(kScreen.adjusted(-1.5, -1.5, 1.5, 1.5));
    p.drawImage(kScreen, panel_);

    // Keyboard plate (frosted, darker) and the circuit-trace decoration below it
    p.setPen(Qt::NoPen);
    p.setBrush(kPlate);
    p.drawRoundedRect(QRectF(8, kKbY - 8, kDevW - 16, 4 * kPitch + 6), 10, 10);
    {
        const double yb = kKbY + 4 * kPitch + 14;      // band under the keys
        QPen tp(kTrace, 0.9);
        p.setPen(tp);
        p.setBrush(Qt::NoBrush);
        QPainterPath tr;
        tr.moveTo(18, yb + 2); tr.lineTo(18, yb + 26); tr.lineTo(30, yb + 34); tr.lineTo(kDevW - 30, yb + 34);
        tr.lineTo(kDevW - 18, yb + 26); tr.lineTo(kDevW - 18, yb + 2);
        p.drawPath(tr);
        p.setBrush(kTrace);
        for (double x : {18.0, kDevW - 18.0}) p.drawEllipse(QPointF(x, yb + 2), 1.6, 1.6);
        p.setPen(QPen(kTrace, 2.2));
        for (int i = 0; i < 4; i++) {                     // chevron stripes, left and right
            p.drawLine(QPointF(30 + i * 7, yb + 24), QPointF(35 + i * 7, yb + 18));
            p.drawLine(QPointF(kDevW - 30 - i * 7, yb + 24), QPointF(kDevW - 35 - i * 7, yb + 18));
        }
    }

    // Keyboard
    QFont f = font();
    for (const auto& k : keys_) {
        p.setPen(QPen(QColor(0x05, 0x05, 0x06), 0.6));
        p.setBrush(k.held ? kCapDown : kCap);
        p.drawRoundedRect(k.rect, 6, 6);
        p.setPen(QPen(QColor(0x3a, 0x3c, 0x40), 0.6));   // top-edge sheen of a glossy cap
        p.drawLine(k.rect.topLeft() + QPointF(5, 1), k.rect.topRight() + QPointF(-5, 1));
        const bool wide = k.label.size() > 2;
        f.setPixelSize(wide ? 7 : 12);
        f.setBold(!wide);
        p.setFont(f);
        p.setPen(backlight_ ? kGlow : kLegend);
        p.drawText(k.rect.adjusted(0, k.sym.isEmpty() ? 0 : 6, 0, 0), Qt::AlignCenter, k.label.toLower() == "space" ? QString() : (wide ? k.label.toLower() : k.label));
        if (k.label.toLower() == "space") {                 // space bar: a bar glyph like the cap
            p.drawLine(QPointF(k.rect.center().x() - 12, k.rect.center().y() + 2),
                       QPointF(k.rect.center().x() + 12, k.rect.center().y() + 2));
        }
        if (!k.sym.isEmpty()) {
            f.setPixelSize(7); f.setBold(false); p.setFont(f);
            p.setPen(backlight_ ? kGlow : kSym);
            p.drawText(k.rect.adjusted(3, 1, -2, 0), Qt::AlignLeft | Qt::AlignTop, k.sym);
        }
    }
    f.setPixelSize(7); f.setBold(false); p.setFont(f);
    p.setPen(QColor(0x5c, 0x61, 0x67));
    p.drawText(QRectF(0, kKbY + 4 * kPitch + 30, kDevW, 10), Qt::AlignCenter,
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
