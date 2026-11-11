#pragma once
#include <QMainWindow>
#include <QStringList>

class DeviceWidget;
class QPlainTextEdit;
class QLineEdit;
class QLabel;
class QSlider;
class QCheckBox;
class QTimer;
class QGroupBox;
class LiveLink;

struct SimOptions {
    QString name = "device1";
    QString dataDir;            // default: <exe dir>/simdata/<name>
    QString script;             // run this script (see sim/README.md)
    QString shotDir;            // where "shot" writes PNGs (default: <data>/shots)
    bool fast = false;          // no e-ink refresh delay
    bool quitAfterScript = false;
    QString live;               // live-device mode: "COM8" or "tcp:host:port" (no firmware here)
    quint16 serialTcp = 0;      // expose this firmware's USB serial on 127.0.0.1:<port>
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(const SimOptions& opt, QWidget* parent = nullptr);

private:
    void buildPanel(QWidget* panel);
    void drainSerial();
    void applyBattery();
    void saveShot(const QString& file, bool wholeDevice, bool window = false);
    // Script runner
    void loadScript(const QString& path);
    void stepScript();

    SimOptions opt_;
    DeviceWidget* device_ = nullptr;
    QPlainTextEdit* log_ = nullptr;
    QLineEdit* serialIn_ = nullptr;
    QSlider* soc_ = nullptr;
    QLabel* socLabel_ = nullptr;
    QLabel* powerState_ = nullptr;
    QCheckBox* usb_ = nullptr;
    QCheckBox* full_ = nullptr;
    QCheckBox* gps_ = nullptr;
    QCheckBox* realistic_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* liveInfo_ = nullptr;
    QGroupBox* powerBox_ = nullptr;
    QGroupBox* envBox_ = nullptr;
    LiveLink* live_ = nullptr;

    QStringList script_;
    int scriptPos_ = 0;
    QTimer* scriptTimer_ = nullptr;
    QStringList waitPattern_;   // "expect" serial text
    QString lastLines_;
};
