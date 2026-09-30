#include <QCanBus>
#include <QCanBusDevice>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>
#include <QVariant>

#include "serialcanbackend.h"

#include <cstdio>

namespace {

int passed = 0;
int failed = 0;

void report(bool ok, const QString &what, const QString &detail = QString())
{
    if (ok) {
        ++passed;
        std::printf("PASS  %s\n", qPrintable(what));
        return;
    }
    ++failed;
    std::printf("FAIL  %s%s\n", qPrintable(what), detail.isEmpty() ? "" : qPrintable(QStringLiteral(" - ") + detail));
}

void pump(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

struct Device
{
    QCanBusDevice *dev = nullptr;
    QList<QCanBusFrame> received;
    qint64 written = 0;
    QList<QString> errors;
    int lastErrorCode = -1;

    ~Device() { close(); }

    bool create(const QString &port)
    {
        QString error;
        dev = QCanBus::instance()->createDevice(QStringLiteral("serialcan-weact"), port, &error);
        if (!dev) {
            errors.append(error);
            return false;
        }
        QObject::connect(dev, &QCanBusDevice::errorOccurred, [this](QCanBusDevice::CanBusError code) {
            lastErrorCode = int(code);
            errors.append(dev->errorString());
        });
        return true;
    }

    void setParam(QCanBusDevice::ConfigurationKey key, const QVariant &value)
    {
        lastErrorCode = -1;
        dev->setConfigurationParameter(key, value);
    }

    bool open(const QString &port, quint32 bitrate, bool canFd, quint32 dataBitrate = 0)
    {
        QString error;
        dev = QCanBus::instance()->createDevice(QStringLiteral("serialcan-weact"), port, &error);
        if (!dev) {
            errors.append(error);
            return false;
        }
        dev->setConfigurationParameter(QCanBusDevice::BitRateKey, QVariant::fromValue(bitrate));
        dev->setConfigurationParameter(QCanBusDevice::CanFdKey, QVariant(canFd));
        if (dataBitrate)
            dev->setConfigurationParameter(QCanBusDevice::DataBitRateKey, QVariant::fromValue(dataBitrate));

        QObject::connect(dev, &QCanBusDevice::framesReceived, [this] {
            while (dev->framesAvailable())
                received.append(dev->readFrame());
        });
        QObject::connect(dev, &QCanBusDevice::framesWritten, [this](qint64 n) { written += n; });
        QObject::connect(dev, &QCanBusDevice::errorOccurred, [this](QCanBusDevice::CanBusError code) {
            lastErrorCode = int(code);
            errors.append(dev->errorString());
        });
        if (!dev->connectDevice()) {
            errors.append(dev->errorString());
            close();
            return false;
        }
        return true;
    }

    bool write(const QCanBusFrame &frame)
    {
        lastErrorCode = -1;
        return dev->writeFrame(frame);
    }

    void close()
    {
        if (!dev)
            return;
        if (dev->state() != QCanBusDevice::UnconnectedState)
            dev->disconnectDevice();
        delete dev;
        dev = nullptr;
    }

    bool waitWritten(qint64 count, int ms)
    {
        QElapsedTimer t;
        t.start();
        while (written < count && t.elapsed() < ms)
            pump(5);
        return written >= count;
    }

    void listen(int ms)
    {
        received.clear();
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < ms)
            pump(100);
    }

    void checkShapes() const
    {
        bool classicOnly = true;
        bool idsFit = true;
        for (const QCanBusFrame &frame : received) {
            if (frame.hasFlexibleDataRateFormat() || frame.payload().size() > 8)
                classicOnly = false;
            if (frame.frameId() > 0x7ff && !frame.hasExtendedFrameFormat())
                idsFit = false;
        }
        report(classicOnly, QStringLiteral("received frames parsed as classic CAN (no FD, <= 8 bytes)"));
        report(idsFit, QStringLiteral("received identifiers match their reported format"));
    }
};

QCanBusFrame classic(quint32 id, const QByteArray &payload, bool extended = false)
{
    QCanBusFrame frame{QCanBusFrame::FrameId(id), payload};
    frame.setExtendedFrameFormat(extended);
    return frame;
}

// Kept separate from the bus session: the only bus access is the failing connect attempt at the end.
void testOfflineConfiguration(const QString &port)
{
    Device off;
    if (!off.create(port)) {
        report(false, QStringLiteral("create device for offline configuration checks"), off.errors.value(0));
        return;
    }

    off.setParam(static_cast<QCanBusDevice::ConfigurationKey>(SerialCanBackend::ModeKey), QVariant(QStringLiteral("bogus")));
    report(off.lastErrorCode == int(QCanBusDevice::ConfigurationError), QStringLiteral("invalid mode value rejected"));

    off.setParam(static_cast<QCanBusDevice::ConfigurationKey>(SerialCanBackend::ModeKey), QVariant(QStringLiteral("silent")));
    report(off.lastErrorCode == -1, QStringLiteral("silent mode accepted"));

    off.setParam(static_cast<QCanBusDevice::ConfigurationKey>(SerialCanBackend::NominalBitrateCodeKey), QVariant(QStringLiteral("6")));
    report(off.lastErrorCode == int(QCanBusDevice::ConfigurationError),
        QStringLiteral("malformed raw bitrate code rejected"));

    off.setParam(static_cast<QCanBusDevice::ConfigurationKey>(SerialCanBackend::NominalBitrateCodeKey), QVariant(QStringLiteral("S0502")));
    report(off.lastErrorCode == -1, QStringLiteral("raw bitrate code accepted"));

    off.setParam(QCanBusDevice::RawFilterKey, QVariant::fromValue(QList<QCanBusDevice::Filter>{}));
    report(off.lastErrorCode == -1, QStringLiteral("empty filter list accepted"));

    QCanBusDevice::Filter a;
    a.frameId = 0x100;
    a.frameIdMask = 0x7ff;
    a.format = QCanBusDevice::Filter::MatchBaseFormat;
    a.type = QCanBusFrame::DataFrame;
    QCanBusDevice::Filter b = a;
    b.frameId = 0x200;
    off.setParam(QCanBusDevice::RawFilterKey, QVariant::fromValue(QList<QCanBusDevice::Filter>{ a, b }));
    report(off.lastErrorCode == -1, QStringLiteral("two filters accepted while disconnected"));

    off.setParam(QCanBusDevice::BitRateKey, QVariant::fromValue(quint32(500000)));
    std::printf("      (this connect attempt takes the adapter off the bus for a moment)\n");
    report(!off.dev->connectDevice(), QStringLiteral("two standard filters make the connection fail"),
        QStringLiteral("the device holds a single filter per identifier format"));
}

// Everything that needs the adapter to stay on the bus runs inside this single connection.
void testBusSession(const QString &port, int listenMs)
{
    Device d;
    report(d.open(port, 500000, false), QStringLiteral("open at 500 kbit/s"), d.errors.value(0));
    if (!d.dev)
        return;

    d.listen(listenMs);
    report(!d.received.isEmpty(), QStringLiteral("frames received from the other node"),
        QStringLiteral("got %1 frames in %2 ms").arg(d.received.size()).arg(listenMs));
    d.checkShapes();

    QCanBusFrame fd{QCanBusFrame::FrameId(0x123), QByteArray(8, 'x')};
    fd.setFlexibleDataRateFormat(true);
    report(!d.write(fd) && d.lastErrorCode == int(QCanBusDevice::WriteError),
        QStringLiteral("CAN FD frame rejected when CanFdKey is off"));

    report(!d.write(classic(0x123, QByteArray(9, 'x'))) && d.lastErrorCode == int(QCanBusDevice::WriteError),
        QStringLiteral("9 byte payload rejected on classic CAN"));

    report(!d.write(classic(0x800, QByteArray(1, 'x'))) && d.lastErrorCode == int(QCanBusDevice::WriteError),
        QStringLiteral("0x800 id rejected without extended format"));

    d.setParam(QCanBusDevice::BitRateKey, QVariant::fromValue(quint32(250000)));
    report(d.lastErrorCode == int(QCanBusDevice::ConfigurationError),
        QStringLiteral("bitrate change on a connected device rejected"));

    d.setParam(QCanBusDevice::LoopbackKey, QVariant(true));
    report(d.lastErrorCode == int(QCanBusDevice::ConfigurationError), QStringLiteral("LoopbackKey rejected"));

    d.setParam(QCanBusDevice::ReceiveOwnKey, QVariant(true));
    report(d.lastErrorCode == int(QCanBusDevice::ConfigurationError), QStringLiteral("ReceiveOwnKey rejected"));

    d.setParam(QCanBusDevice::ProtocolKey, QVariant(1));
    report(d.lastErrorCode == int(QCanBusDevice::ConfigurationError), QStringLiteral("ProtocolKey rejected"));

    d.setParam(QCanBusDevice::ErrorFilterKey, QVariant(1));
    report(d.lastErrorCode == int(QCanBusDevice::ConfigurationError), QStringLiteral("ErrorFilterKey rejected"));

    d.setParam(static_cast<QCanBusDevice::ConfigurationKey>(SerialCanBackend::DataBitrateCodeKey + 5),
        QVariant(QStringLiteral("x")));
    report(d.lastErrorCode == int(QCanBusDevice::ConfigurationError),
        QStringLiteral("unknown configuration key rejected"));

    d.written = 0;
    d.errors.clear();
    report(d.write(classic(0x123, QByteArray::fromHex("DEADBEEF"))), QStringLiteral("write standard frame"));
    report(d.write(classic(0x1F334455, QByteArray::fromHex("1122334455667788"), true)),
        QStringLiteral("write extended frame"));
    report(d.write(classic(0x124, QByteArray())), QStringLiteral("write zero length frame"));

    QCanBusFrame remote{QCanBusFrame::FrameId(0x125), QByteArray(8, '\0')};
    remote.setFrameType(QCanBusFrame::RemoteRequestFrame);
    report(d.write(remote), QStringLiteral("write remote request frame"));

    report(d.waitWritten(4, 2000), QStringLiteral("every written frame was acknowledged by the bus"),
        QStringLiteral("acknowledged %1 of 4, last error: %2").arg(d.written).arg(d.errors.value(0)));
    report(d.errors.isEmpty(), QStringLiteral("no write errors on a live bus"), d.errors.value(0));

    QCanBusFrame brs{QCanBusFrame::FrameId(0x126), QByteArray::fromHex("CAFE")};
    brs.setFlexibleDataRateFormat(true);
    report(!d.write(brs) && d.lastErrorCode == int(QCanBusDevice::WriteError),
        QStringLiteral("FD frame still rejected (CanFdKey off)"));

    d.errors.clear();
    d.dev->resetController();
    pump(200);
    report(d.errors.isEmpty(), QStringLiteral("resetController() reports no error"), d.errors.value(0));
    report(d.write(classic(0x127, QByteArray::fromHex("01"))), QStringLiteral("write after controller reset"));
    report(d.waitWritten(5, 1500), QStringLiteral("frame after controller reset acknowledged"), d.errors.value(0));

    d.written = 0;
    d.errors.clear();
    QElapsedTimer t;
    t.start();
    const int count = 200;
    for (int i = 0; i < count; ++i)
        d.write(classic(0x200, QByteArray(1, char(i))));
    const bool allAcked = d.waitWritten(count, 15000);
    const qint64 elapsed = t.elapsed();
    report(allAcked, QStringLiteral("200 back to back frames acknowledged"),
        QStringLiteral("%1 of %2 in %3 ms").arg(d.written).arg(count).arg(elapsed));
    report(d.errors.isEmpty(), QStringLiteral("no errors during the burst"), d.errors.value(0));
    std::printf("      throughput: %lld frames in %lld ms (%.1f frames/s)\n", d.written, elapsed,
        elapsed ? 1000.0 * double(d.written) / double(elapsed) : 0.0);
}

void testFilterSuppression(const QString &port, int listenMs)
{
    Device f;
    if (!f.create(port)) {
        report(false, QStringLiteral("open with a restrictive filter"), f.errors.value(0));
        return;
    }
    QCanBusDevice::Filter onlyZero;
    onlyZero.frameId = 0x000;
    onlyZero.frameIdMask = 0x7ff;
    onlyZero.format = QCanBusDevice::Filter::MatchBaseFormat;
    onlyZero.type = QCanBusFrame::DataFrame;
    f.dev->setConfigurationParameter(QCanBusDevice::RawFilterKey,
        QVariant::fromValue(QList<QCanBusDevice::Filter>{ onlyZero }));
    f.dev->setConfigurationParameter(QCanBusDevice::BitRateKey, QVariant::fromValue(quint32(500000)));
    f.dev->setConfigurationParameter(QCanBusDevice::CanFdKey, QVariant(false));
    QObject::connect(f.dev, &QCanBusDevice::framesReceived, [&f] {
        while (f.dev->framesAvailable())
            f.received.append(f.dev->readFrame());
    });
    std::printf("      (this connection takes the adapter off the bus for a moment)\n");
    report(f.dev->connectDevice(), QStringLiteral("open with a restrictive filter"), f.dev->errorString());
    if (f.dev->state() != QCanBusDevice::ConnectedState)
        return;

    f.received.clear();
    pump(listenMs);
    report(f.received.isEmpty(), QStringLiteral("standard identifier filter suppresses other ids"),
        QStringLiteral("still received %1 frames").arg(f.received.size()));
}

void testReconnect(const QString &port)
{
    Device d;
    std::printf("      (close and reopen takes the adapter off the bus for a moment)\n");
    report(d.open(port, 500000, false), QStringLiteral("open before close/reopen"), d.errors.value(0));
    if (!d.dev)
        return;
    d.close();
    report(d.dev == nullptr, QStringLiteral("device closed"));
    report(d.open(port, 500000, false), QStringLiteral("reopen after close"), d.errors.value(0));
    if (!d.dev)
        return;
    d.written = 0;
    report(d.write(classic(0x128, QByteArray::fromHex("02"))), QStringLiteral("write after reopen"));
    report(d.waitWritten(1, 1500), QStringLiteral("frame acknowledged after reopen"), d.errors.value(0));
}

void testListenOnly(const QString &port, int listenMs)
{
    Device d;
    report(d.open(port, 500000, false), QStringLiteral("open for passive listen"), d.errors.value(0));
    if (!d.dev)
        return;
    d.listen(listenMs);
    report(!d.received.isEmpty(), QStringLiteral("passive listen received frames"),
        QStringLiteral("got %1 frames in %2 ms").arg(d.received.size()).arg(listenMs));
    d.checkShapes();
}

void testBitrateCoding(const QString &port)
{
    struct Case { const char *name; quint32 bitrate; bool accepted; };
    const Case cases[] = {
        { "10 kbit/s (S0)", 10000, true },
        { "20 kbit/s (S1)", 20000, true },
        { "50 kbit/s (S2)", 50000, true },
        { "100 kbit/s (S3)", 100000, true },
        { "125 kbit/s (S4)", 125000, true },
        { "250 kbit/s (S5)", 250000, true },
        { "800 kbit/s (S7)", 800000, true },
        { "83.3 kbit/s (S9)", 83333, true },
        { "62.5 kbit/s (SB)", 62500, true },
        { "5 kbit/s (SD)", 5000, true },
        { "400 kbit/s (custom Sxxyy)", 400000, true },
        { "123.456 kbit/s (not representable)", 123456, false },
    };

    for (const Case &c : cases) {
        Device d;
        const bool opened = d.open(port, c.bitrate, false);
        report(opened == c.accepted,
            QStringLiteral("bitrate %1 %2").arg(QString::fromLatin1(c.name),
                c.accepted ? QStringLiteral("accepted") : QStringLiteral("rejected")),
            d.errors.value(0));
    }

    Device fd;
    report(fd.open(port, 500000, true, 2000000), QStringLiteral("open with CAN FD enabled and 2 Mbit/s data phase"),
        fd.errors.value(0));
    if (!fd.dev)
        return;
    QCanBusFrame fdFrame{QCanBusFrame::FrameId(0x129), QByteArray(8, '\xAA')};
    fdFrame.setFlexibleDataRateFormat(true);
    report(fd.write(fdFrame), QStringLiteral("FD frame queued while CAN FD is enabled"));
    report(!fd.waitWritten(1, 1500), QStringLiteral("FD frame is not acknowledged on a classic bus"));
    report(fd.lastErrorCode == int(QCanBusDevice::WriteError),
        QStringLiteral("FD attempt on a classic bus reported as a write error"), fd.errors.value(0));
}

}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2) {
        std::fprintf(stderr,
            "usage: %s <serial-port> [test] [listen-ms]\n"
            "  the bus is only needed for the session/listen/filter/reconnect tests\n"
            "  test: session | listen | filter | reconnect | offline | bitrates\n"
            "        default: offline + session + filter + reconnect\n"
            "  bitrates: opens at every bitrate and sends an FD frame on a classic bus,\n"
            "            only run it with no other node attached\n", argv[0]);
        return 2;
    }
    const QString port = QString::fromLocal8Bit(argv[1]);
    const QString only = argc > 2 ? QString::fromLocal8Bit(argv[2]) : QString();
    const int listenMs = argc > 3 ? QByteArray(argv[3]).toInt() : 10000;

    std::printf("== classic CAN matrix on %s ==\n", qPrintable(port));
    if (only == QLatin1String("bitrates")) {
        std::fprintf(stderr,
            "WARNING: 'bitrates' opens the adapter at every bitrate, including wrong ones for the "
            "bus, and sends a CAN FD frame on a classic bus. Only run it with no other node attached.\n");
        testBitrateCoding(port);
    } else if (only == QLatin1String("offline")) {
        testOfflineConfiguration(port);
    } else if (only == QLatin1String("session")) {
        testBusSession(port, listenMs);
    } else if (only == QLatin1String("listen")) {
        testListenOnly(port, listenMs);
    } else if (only == QLatin1String("filter")) {
        testFilterSuppression(port, listenMs);
    } else if (only == QLatin1String("reconnect")) {
        testReconnect(port);
    } else {
        testBusSession(port, listenMs);
        testFilterSuppression(port, listenMs);
        testOfflineConfiguration(port);
        testReconnect(port);
    }

    std::printf("== %d passed, %d failed ==\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
