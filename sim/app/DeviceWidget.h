#pragma once
// Draws the T-Deck Pro (body, bezel, e-ink screen, keyboard) and turns mouse /
// PC-keyboard input into touch points and TCA8418 key-matrix events.
#include <QImage>
#include <QRectF>
#include <QWidget>
#include <vector>

struct SimKey {
    uint8_t row, col;       // TCA8418 matrix position (firmware kMap)
    QString label;          // cap legend
    QString sym;            // SYM-layer legend
    QRectF rect;            // in device units
    bool held = false;
};

class DeviceWidget : public QWidget {
    Q_OBJECT
public:
    explicit DeviceWidget(QWidget* parent = nullptr);

    QImage screenImage(int scale = 2) const;    // panel only, crisp integer scale
    QImage deviceImage();                       // whole device as drawn
    void tapKey(const QString& name);            // "Q", "ENTER", "SYM+W", "SHIFT"...
    void pressKey(const QString& name, bool down);
    void touch(int x, int y);                    // tap at panel coordinates
    QSize sizeHint() const override;
    void setFooter(const QString& text) { footer_ = text; update(); }   // replaces the panel timing line

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void keyReleaseEvent(QKeyEvent*) override;
    void timerEvent(QTimerEvent*) override;

private:
    SimKey* keyAt(const QPointF& dev);
    SimKey* keyByName(const QString& name);
    void sendKey(SimKey* k, bool down);
    QTransform devToWidget() const;
    void pullPanel();

    std::vector<SimKey> keys_;
    SimKey* mouseKey_ = nullptr;
    bool mouseTouch_ = false;
    QImage panel_;                      // 240x320, current panel contents
    uint32_t panelVersion_ = ~0u;
    uint32_t lastRefreshMs_ = 0;
    bool lastFull_ = false;
    QString footer_;
    bool backlight_ = false;
    bool vibrating_ = false;
    uint32_t beepSeen_ = 0, beepFlashUntil_ = 0;
};
