#pragma once
// Live-device mode and serial-over-TCP bridge.
//
// LiveLink: instead of running the firmware here, connect to a real T-Deck Pro on a
// USB serial port (or to another simulator's --serial-tcp port), ask it to mirror its
// screen (protocol in src/boards/tdeck_Pro/remote/ScreenMirror.h), show the frames in
// the device drawing and send the keys / touches / console lines back to it. It uses
// the same sim::hw() fields as the simulated hardware, so DeviceWidget, the serial
// console and scripts work unchanged.
//
// SerialTcpBridge: exposes this simulator's own firmware USB serial on a localhost TCP
// port, so a second simulator can attach to it with --live tcp:127.0.0.1:<port>.
#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <mutex>

class QIODevice;
class QTcpServer;
class QTcpSocket;

class LiveLink : public QObject {
    Q_OBJECT
public:
    // target: "COM8" (any serial port name) or "tcp:host:port".
    explicit LiveLink(const QString& target, QObject* parent = nullptr);
    QString target() const { return target_; }

signals:
    void stateChanged(const QString& text);     // connection / device status for the UI

private:
    void open();
    void onReadyRead();
    void handleLine(const QByteArray& line);
    void handleFrameLine(const QByteArray& line);
    void pump();                 // keys, touch, console input -> device
    void send(const QByteArray& line);
    void lost(const QString& why);

    QString target_;
    QIODevice* io_ = nullptr;
    QTimer pumpTimer_, beatTimer_, retryTimer_;
    QByteArray rx_;
    bool connected_ = false;
    bool touchWasDown_ = false;
    int width_ = 240, height_ = 320;
    int frames_ = 0, badLines_ = 0;
    qint64 lastFrameMs_ = 0;
    QString status_;
};

class SerialTcpBridge : public QObject {
    Q_OBJECT
public:
    // Wraps sim::hw().serialOut (call after MainWindow has installed its own).
    SerialTcpBridge(quint16 port, QObject* parent = nullptr);
    bool listening() const;

private:
    void flush();

    QTcpServer* server_ = nullptr;
    QTcpSocket* client_ = nullptr;
    QTimer timer_;
    std::mutex mu_;
    QByteArray pending_;
};
