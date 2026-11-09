#include "app/LiveLink.h"

#include <QDateTime>
#include <QSerialPort>
#include <QTcpServer>
#include <QTcpSocket>

#include <cstdio>

#include "hw/SimHw.h"

namespace {
uint16_t crc16(const uint8_t* d, size_t n) {   // CRC-16/CCITT-FALSE, as in ScreenMirror.cpp
    uint16_t c = 0xFFFF;
    while (n--) {
        c ^= (uint16_t)(*d++) << 8;
        for (int i = 0; i < 8; i++) c = (c & 0x8000) ? (uint16_t)(c << 1 ^ 0x1021) : (uint16_t)(c << 1);
    }
    return c;
}

void toConsole(const QString& line) {
    std::function<void(const std::string&)> out;
    { std::lock_guard<std::mutex> l(sim::hw().mu); out = sim::hw().serialOut; }
    if (out) out(line.toStdString());
}
}  // namespace

// ---- LiveLink ---------------------------------------------------------------------------

LiveLink::LiveLink(const QString& target, QObject* parent) : QObject(parent), target_(target) {
    connect(&pumpTimer_, &QTimer::timeout, this, &LiveLink::pump);
    pumpTimer_.start(15);
    connect(&beatTimer_, &QTimer::timeout, this, [this] { if (connected_) send("@M on"); });
    beatTimer_.start(2000);
    retryTimer_.setSingleShot(true);
    connect(&retryTimer_, &QTimer::timeout, this, &LiveLink::open);
    QTimer::singleShot(0, this, &LiveLink::open);
}

void LiveLink::open() {
    if (io_) { io_->deleteLater(); io_ = nullptr; }
    connected_ = false;
    rx_.clear();
    if (target_.startsWith("tcp:", Qt::CaseInsensitive)) {
        const QString hp = target_.mid(4);
        const int colon = hp.lastIndexOf(':');
        auto* s = new QTcpSocket(this);
        io_ = s;
        connect(s, &QTcpSocket::connected, this, [this] {
            connected_ = true;
            emit stateChanged("LIVE " + target_ + ": connected, waiting for screen");
            send("@M on");
        });
        connect(s, &QTcpSocket::errorOccurred, this, [this, s] { lost(s->errorString()); });
        connect(s, &QTcpSocket::disconnected, this, [this] { lost("disconnected"); });
        connect(s, &QTcpSocket::readyRead, this, &LiveLink::onReadyRead);
        s->connectToHost(hp.left(colon), hp.mid(colon + 1).toUShort());
        emit stateChanged("LIVE " + target_ + ": connecting");
        return;
    }
    auto* p = new QSerialPort(target_, this);
    io_ = p;
    p->setBaudRate(115200);   // ignored by USB CDC, set for UART bridges
    connect(p, &QSerialPort::readyRead, this, &LiveLink::onReadyRead);
    connect(p, &QSerialPort::errorOccurred, this, [this, p](QSerialPort::SerialPortError e) {
        if (e != QSerialPort::NoError && e != QSerialPort::TimeoutError) lost(p->errorString());
    });
    if (!p->open(QIODevice::ReadWrite)) {
        lost(p->errorString());
        return;
    }
    // ESP32-S3 USB serial: DTR on with RTS off never resets the chip.
    p->setDataTerminalReady(true);
    p->setRequestToSend(false);
    connected_ = true;
    emit stateChanged("LIVE " + target_ + ": open, waiting for screen");
    send("@M on");
}

void LiveLink::lost(const QString& why) {
    if (!io_) return;
    const bool was = connected_;
    connected_ = false;
    if (was || !retryTimer_.isActive()) {
        emit stateChanged(QString("LIVE %1: %2 - retrying").arg(target_, why));
        if (was) toConsole("[LIVE] " + target_ + ": " + why);
    }
    if (!retryTimer_.isActive()) retryTimer_.start(2000);
}

void LiveLink::send(const QByteArray& line) {
    if (!io_ || !connected_) return;
    io_->write(line + "\n");
}

void LiveLink::onReadyRead() {
    rx_ += io_->readAll();
    int nl;
    while ((nl = rx_.indexOf('\n')) >= 0) {
        QByteArray line = rx_.left(nl);
        rx_.remove(0, nl + 1);
        if (line.endsWith('\r')) line.chop(1);
        handleLine(line);
    }
    if (rx_.size() > 64 * 1024) rx_.clear();   // no newline in 64 KB: junk
}

void LiveLink::handleLine(const QByteArray& line) {
    // A frame line can arrive glued behind log text written by another task.
    const int at = line.indexOf("@FB ");
    if (at >= 0) {
        if (at > 0) toConsole(QString::fromUtf8(line.left(at)));
        handleFrameLine(line.mid(at));
        return;
    }
    if (line.startsWith("@HELLO ")) {
        const QList<QByteArray> f = line.split(' ');
        if (f.size() >= 3) { width_ = f[1].toInt(); height_ = f[2].toInt(); }
        if (width_ != 240 || height_ != 320)
            toConsole(QString("[LIVE] unexpected screen size %1x%2").arg(width_).arg(height_));
        return;
    }
    if (line.startsWith("@ST ")) {
        status_ = QString::fromUtf8(line.mid(4));
        emit stateChanged(QString("LIVE %1  |  %2  |  %3 frames").arg(target_, status_).arg(frames_));
        return;
    }
    toConsole(QString::fromUtf8(line));
}

void LiveLink::handleFrameLine(const QByteArray& line) {
    // @FB <y> <rows> <crc16> <base64>
    const QList<QByteArray> f = line.split(' ');
    bool ok = f.size() == 5;
    const int y = ok ? f[1].toInt(&ok) : 0;
    const int rows = ok ? f[2].toInt(&ok) : 0;
    const uint16_t crc = ok ? (uint16_t)f[3].toUInt(&ok, 16) : 0;
    const int stride = width_ / 8;
    QByteArray data;
    if (ok) {
        data = QByteArray::fromBase64(f[4]);
        ok = rows > 0 && y >= 0 && y + rows <= height_ && data.size() == rows * stride &&
             crc16(reinterpret_cast<const uint8_t*>(data.constData()), data.size()) == crc;
    }
    if (!ok) {
        badLines_++;
        send("@M sync");
        return;
    }
    {
        std::lock_guard<std::mutex> l(sim::hw().mu);
        auto& h = sim::hw();
        if ((int)h.panel.size() >= (y + rows) * stride) {
            memcpy(h.panel.data() + y * stride, data.constData(), data.size());
            h.panelVersion++;
            h.lastRefreshMs = 0;
        }
    }
    frames_++;
    lastFrameMs_ = QDateTime::currentMSecsSinceEpoch();
}

void LiveLink::pump() {
    // Everything the UI and scripts put into the simulated hardware goes to the device.
    std::deque<sim::KeyMatrixEvent> keys;
    std::string console;
    bool down;
    int tx, ty;
    {
        std::lock_guard<std::mutex> l(sim::hw().mu);
        auto& h = sim::hw();
        keys.swap(h.keyEvents);
        console.assign(h.serialIn.begin(), h.serialIn.end());
        h.serialIn.clear();
        down = h.touchDown;
        tx = h.touchX;
        ty = h.touchY;
    }
    for (const auto& k : keys) {
        const int raw = (k.row * 10 + k.col + 1) | (k.pressed ? 0x80 : 0);
        send("@K " + QByteArray::number(raw));
    }
    if (down && !touchWasDown_) send(QString("@T %1 %2").arg(tx).arg(ty).toUtf8());
    touchWasDown_ = down;
    if (!console.empty() && io_ && connected_) io_->write(console.data(), (qint64)console.size());
}

// ---- SerialTcpBridge --------------------------------------------------------------------

SerialTcpBridge::SerialTcpBridge(quint16 port, QObject* parent) : QObject(parent) {
    server_ = new QTcpServer(this);
    if (!server_->listen(QHostAddress::LocalHost, port)) {
        fprintf(stderr, "[SIM] serial bridge: cannot listen on 127.0.0.1:%u (%s)\n", port,
                server_->errorString().toLocal8Bit().constData());
        return;
    }
    fprintf(stdout, "[SIM] firmware USB serial on tcp 127.0.0.1:%u\n", port);
    fflush(stdout);
    connect(server_, &QTcpServer::newConnection, this, [this] {
        while (QTcpSocket* s = server_->nextPendingConnection()) {
            if (client_) client_->deleteLater();   // one client at a time: newest wins
            client_ = s;
            connect(s, &QTcpSocket::readyRead, this, [this, s] {
                const QByteArray in = s->readAll();
                std::lock_guard<std::mutex> l(sim::hw().mu);
                for (char c : in) sim::hw().serialIn.push_back(c);
            });
            connect(s, &QTcpSocket::disconnected, this, [this, s] {
                if (client_ == s) client_ = nullptr;
                s->deleteLater();
            });
        }
    });
    {
        std::lock_guard<std::mutex> l(sim::hw().mu);
        auto prev = sim::hw().serialOut;
        sim::hw().serialOut = [this, prev](const std::string& line) {
            if (prev) prev(line);
            std::lock_guard<std::mutex> g(mu_);
            if (pending_.size() < 4 * 1024 * 1024) pending_.append(line.data(), (qsizetype)line.size()).append('\n');
        };
    }
    connect(&timer_, &QTimer::timeout, this, &SerialTcpBridge::flush);
    timer_.start(10);
}

bool SerialTcpBridge::listening() const { return server_ && server_->isListening(); }

void SerialTcpBridge::flush() {
    QByteArray out;
    { std::lock_guard<std::mutex> g(mu_); out.swap(pending_); }
    if (!out.isEmpty() && client_) client_->write(out);
}
