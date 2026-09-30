#pragma once

#include <QCanBusDevice>
#include <QSerialPort>

class QEventLoop;

class SerialCanBackend : public QCanBusDevice
{
    Q_OBJECT

public:
    static constexpr int ModeKey = int(QCanBusDevice::UserKey) + 1;
    static constexpr int AutoRetransmitKey = int(QCanBusDevice::UserKey) + 2;
    static constexpr int NominalBitrateCodeKey = int(QCanBusDevice::UserKey) + 3;
    static constexpr int DataBitrateCodeKey = int(QCanBusDevice::UserKey) + 4;

    explicit SerialCanBackend(const QString &interfaceName, QObject *parent = nullptr);

    static QList<QCanBusDeviceInfo> interfaces();
    static bool looksLikeSlcanDevice(const QSerialPortInfo &info);

    bool open() override;
    void close() override;
    bool writeFrame(const QCanBusFrame &newFrame) override;
    QString interpretErrorFrame(const QCanBusFrame &errorFrame) override;
    QCanBusDeviceInfo deviceInfo() const override;
    bool hasBusStatus() const override;
    QCanBusDevice::CanBusStatus busStatus() override;
    void resetController() override;
    void setConfigurationParameter(ConfigurationKey key, const QVariant &value) override;

signals:
    void commandFinished();

private:
    void onReadyRead();
    void onSerialError(QSerialPort::SerialPortError error);
    void processBuffer();
    void handleLine(const QByteArray &line);
    void handleCommandReply(bool ok);

    bool command(const QByteArray &bytes, int timeoutMs = 500);
    bool configureDevice();
    bool configureMode();
    bool configureBitrate();
    bool configureFilters();
    bool readErrorRegister(quint32 &value, int timeoutMs = 500);
    bool transmitNext();

    bool rejectConfiguration(const QString &reason);
    bool fail(const QString &message, QCanBusDevice::CanBusError error = QCanBusDevice::ConnectionError);

    QSerialPort m_port;
    QString m_name;
    QString m_description;
    QString m_serialNumber;

    QByteArray m_buffer;
    bool m_waitingReply = false;
    bool m_replyOk = false;

    bool m_fdEnabled = false;
    QString m_mode = QStringLiteral("normal");
    bool m_autoRetransmit = false;
    QString m_nominalCode;
    QString m_dataCode;

    QList<QByteArray> m_pendingCommands;
    bool m_commandInFlight = false;
    bool m_fdDropWarned = false;
};
