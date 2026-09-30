#include <QCanBus>
#include <QCanBusDevice>
#include <QCanBusDeviceInfo>
#include <QCoreApplication>
#include <QList>
#include <QTimer>
#include <QVariant>

#include <cstdio>

static void printDevices()
{
    const QStringList plugins = QCanBus::instance()->plugins();
    std::printf("plugins: %s\n", qPrintable(plugins.join(QStringLiteral(", "))));
    for (const QString &plugin : plugins) {
        QString error;
        const auto devices = QCanBus::instance()->availableDevices(plugin, &error);
        for (const QCanBusDeviceInfo &info : devices) {
            std::printf("  %s/%s: %s [fd=%d virtual=%d]\n", qPrintable(info.plugin()), qPrintable(info.name()),
                qPrintable(info.description()), info.hasFlexibleDataRate(), info.isVirtual());
        }
        if (!error.isEmpty())
            std::printf("  %s: %s\n", qPrintable(plugin), qPrintable(error));
    }
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    if (argc < 2) {
        printDevices();
        return 0;
    }

    const QString port = QString::fromLocal8Bit(argv[1]);
    const quint32 nominal = argc > 2 ? quint32(QByteArray(argv[2]).toUInt()) : 500000U;
    const quint32 data = argc > 3 ? quint32(QByteArray(argv[3]).toUInt()) : 0U;

    QString error;
    QCanBusDevice *device = QCanBus::instance()->createDevice(QStringLiteral("serialcan-weact"), port, &error);
    if (!device) {
        std::fprintf(stderr, "createDevice failed: %s\n", qPrintable(error));
        return 1;
    }

    device->setConfigurationParameter(QCanBusDevice::BitRateKey, QVariant::fromValue(nominal));
    device->setConfigurationParameter(QCanBusDevice::CanFdKey, QVariant(data > 0));
    if (data)
        device->setConfigurationParameter(QCanBusDevice::DataBitRateKey, QVariant::fromValue(data));

    if (argc > 4) {
        const QList<QByteArray> parts = QByteArray(argv[4]).split(':');
        if (parts.size() == 2) {
            QCanBusDevice::Filter filter;
            filter.frameId = parts.at(0).toUInt(nullptr, 16);
            filter.frameIdMask = parts.at(1).toUInt(nullptr, 16);
            filter.format = QCanBusDevice::Filter::MatchBaseFormat;
            filter.type = QCanBusFrame::DataFrame;
            device->setConfigurationParameter(QCanBusDevice::RawFilterKey,
                QVariant::fromValue(QList<QCanBusDevice::Filter>{ filter }));
        }
    }

    QObject::connect(device, &QCanBusDevice::errorOccurred, [device](QCanBusDevice::CanBusError code) {
        std::printf("error signal: %d (%s)\n", int(code), qPrintable(device->errorString()));
    });
    QObject::connect(device, &QCanBusDevice::framesWritten, [](qint64 count) {
        std::printf("framesWritten: %lld\n", count);
    });
    QObject::connect(device, &QCanBusDevice::framesReceived, [device] {
        while (device->framesAvailable()) {
            const QCanBusFrame frame = device->readFrame();
            std::printf("RX id=0x%X ext=%d fd=%d brs=%d dlc=%lld payload=%s\n", frame.frameId(),
                frame.hasExtendedFrameFormat(), frame.hasFlexibleDataRateFormat(), frame.hasBitrateSwitch(),
                frame.payload().size(), frame.payload().toHex().toUpper().constData());
        }
    });

    if (!device->connectDevice()) {
        std::fprintf(stderr, "connectDevice failed: %s (error %d)\n", qPrintable(device->errorString()),
            int(device->error()));
        return 1;
    }
    std::printf("connected: %s\n", qPrintable(device->deviceInfo().description()));
    std::printf("bus status: %d\n", int(device->busStatus()));

    if (argc > 5 && QByteArray(argv[5]) == "fd-toggle") {
        device->setConfigurationParameter(QCanBusDevice::CanFdKey, QVariant(true));
        std::printf("enabled CanFdKey after connecting\n");
    }

    QCanBusFrame frame;
    frame.setFrameId(0x123);
    frame.setPayload(QByteArray::fromHex("DEADBEEF"));
    std::printf("writeFrame: %d, framesToWrite=%lld\n", device->writeFrame(frame), device->framesToWrite());

    QCanBusFrame fdFrame;
    fdFrame.setFrameId(0x123);
    fdFrame.setPayload(QByteArray(64, '\xAA'));
    fdFrame.setFlexibleDataRateFormat(true);
    fdFrame.setBitrateSwitch(true);
    if (device->deviceInfo().hasFlexibleDataRate())
        std::printf("writeFrame(fd): %d, framesToWrite=%lld\n", device->writeFrame(fdFrame), device->framesToWrite());

    QTimer::singleShot(1000, &app, [device] {
        std::printf("after 1s: framesToWrite=%lld bus status=%d\n", device->framesToWrite(), int(device->busStatus()));
        device->disconnectDevice();
        std::printf("disconnected: state=%d\n", int(device->state()));
        QCoreApplication::quit();
    });

    return app.exec();
}
