#include "serialcanbackend.h"

#include "slcan.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QLoggingCategory>
#include <QSerialPortInfo>
#include <QStringList>
#include <QTimer>
#include <QVariant>

#include <optional>

Q_LOGGING_CATEGORY(serialcanLog, "qt.canbus.plugins.serialcan-weact")

namespace {

constexpr int CommandTimeoutMs = 500;
// The unterminated error register answer counts as complete once no byte arrived for this long.
constexpr int AnswerSettleMs = 150;
constexpr int MaxErrorRegisterDigits = 8;
constexpr quint16 WeActVendorId = 0x0483;
constexpr quint16 WeActProductId = 0x5740;
constexpr quint32 StandardIdMask = 0x7ff;
constexpr quint32 ExtendedIdMask = 0x1fffffff;

const QByteArray ErrorRegisterPrefix = QByteArrayLiteral("CANable Error Register: ");

bool isValidRawCode(const QByteArray &code, char prefix)
{
    if (code.size() < 2 || code.size() > 7 || code.at(0) != prefix)
        return false;
    for (int i = 1; i < code.size(); ++i) {
        if (!weact::slcan::isHexDigit(code.at(i)))
            return false;
    }
    return true;
}

// The answer to `E` has no terminator, so the reader and the drain path have to locate
// "CANable Error Register: <hex>" inside the receive buffer the same way.
int findErrorRegister(const QByteArray &buffer, int &digits)
{
    const int start = buffer.indexOf(ErrorRegisterPrefix);
    if (start < 0)
        return -1;
    const int valueStart = start + ErrorRegisterPrefix.size();
    int count = 0;
    while (count < MaxErrorRegisterDigits && valueStart + count < buffer.size()
        && weact::slcan::isHexDigit(buffer.at(valueStart + count))) {
        ++count;
    }
    digits = count;
    return valueStart;
}

QString describePort(const QSerialPortInfo &info)
{
    QStringList parts;
    if (!info.description().isEmpty())
        parts.append(info.description());
    if (!info.manufacturer().isEmpty())
        parts.append(info.manufacturer());
    if (parts.isEmpty())
        parts.append(QStringLiteral("serial port"));
    parts.append(QStringLiteral("(SLCAN, CAN FD capable)"));
    return parts.join(QStringLiteral(" - "));
}

}

SerialCanBackend::SerialCanBackend(const QString &interfaceName, QObject *parent)
    : QCanBusDevice(parent)
    , m_name(interfaceName)
{
    m_port.setPortName(interfaceName);
    m_port.setBaudRate(QSerialPort::Baud115200);
    m_port.setDataBits(QSerialPort::Data8);
    m_port.setParity(QSerialPort::NoParity);
    m_port.setStopBits(QSerialPort::OneStop);
    m_port.setFlowControl(QSerialPort::NoFlowControl);

    for (const QSerialPortInfo &info : QSerialPortInfo::availablePorts()) {
        if (info.systemLocation() == interfaceName || info.portName() == interfaceName) {
            m_description = describePort(info);
            m_serialNumber = info.serialNumber();
            break;
        }
    }

    connect(&m_port, &QSerialPort::readyRead, this, &SerialCanBackend::onReadyRead);
    connect(&m_port, &QSerialPort::errorOccurred, this, &SerialCanBackend::onSerialError);
}

bool SerialCanBackend::looksLikeSlcanDevice(const QSerialPortInfo &info)
{
    if (info.hasVendorIdentifier() && info.hasProductIdentifier()
        && info.vendorIdentifier() == WeActVendorId && info.productIdentifier() == WeActProductId) {
        return true;
    }
    const QString text = info.description() + QLatin1Char(' ') + info.manufacturer();
    const QString lowered = text.toLower();
    return lowered.contains(QLatin1String("usb2can")) || lowered.contains(QLatin1String("slcan"))
        || lowered.contains(QLatin1String("canable")) || lowered.contains(QLatin1String("weact"));
}

QList<QCanBusDeviceInfo> SerialCanBackend::interfaces()
{
    QList<QCanBusDeviceInfo> result;
    const auto ports = QSerialPortInfo::availablePorts();
    for (const QSerialPortInfo &info : ports) {
        if (!looksLikeSlcanDevice(info))
            continue;
        result.append(createDeviceInfo(QStringLiteral("serialcan-weact"), info.systemLocation(),
            info.serialNumber(), describePort(info), info.portName(), 0, false, true));
    }
    return result;
}

bool SerialCanBackend::open()
{
    if (!m_port.open(QIODevice::ReadWrite)) {
        setState(QCanBusDevice::UnconnectedState);
        return fail(tr("Cannot open %1: %2").arg(m_name, m_port.errorString()));
    }

    m_buffer.clear();
    m_pendingCommands.clear();
    m_commandInFlight = false;
    m_fdDropWarned = false;
    m_port.clear(QSerialPort::AllDirections);

    if (!configureDevice()) {
        m_port.close();
        setState(QCanBusDevice::UnconnectedState);
        return false;
    }

    setState(QCanBusDevice::ConnectedState);
    return true;
}

void SerialCanBackend::close()
{
    if (m_port.isOpen()) {
        if (state() == QCanBusDevice::ConnectedState)
            command(QByteArrayLiteral("C\r"), 200);
        m_port.close();
    }
    while (hasOutgoingFrames())
        dequeueOutgoingFrame();
    m_pendingCommands.clear();
    m_commandInFlight = false;
    m_buffer.clear();
    setState(QCanBusDevice::UnconnectedState);
}

bool SerialCanBackend::writeFrame(const QCanBusFrame &newFrame)
{
    if (state() != QCanBusDevice::ConnectedState)
        return fail(tr("Cannot write a frame: %1 is not connected").arg(m_name), QCanBusDevice::WriteError);

    if (newFrame.hasFlexibleDataRateFormat() && !m_fdEnabled) {
        return fail(tr("Cannot write a CAN FD frame: enable QCanBusDevice::CanFdKey first"),
            QCanBusDevice::WriteError);
    }

    QString error;
    const QByteArray payload = weact::slcan::encode(newFrame, error);
    if (payload.isEmpty())
        return fail(error, QCanBusDevice::WriteError);

    enqueueOutgoingFrame(newFrame);
    m_pendingCommands.append(payload);
    return transmitNext();
}

// The firmware answers one command at a time and drops input while it is busy with a
// transmission, so frames are handed over strictly one by one, each after its reply.
bool SerialCanBackend::transmitNext()
{
    if (m_commandInFlight || m_pendingCommands.isEmpty())
        return true;

    const QByteArray command = m_pendingCommands.first();
    if (m_port.write(command) != command.size())
        return fail(tr("Cannot write to %1: %2").arg(m_name, m_port.errorString()), QCanBusDevice::WriteError);

    m_commandInFlight = true;
    return true;
}

QString SerialCanBackend::interpretErrorFrame(const QCanBusFrame &errorFrame)
{
    return tr("CAN error frame with id 0x%1").arg(errorFrame.frameId(), 0, 16);
}

QCanBusDeviceInfo SerialCanBackend::deviceInfo() const
{
    return createDeviceInfo(QStringLiteral("serialcan-weact"), m_name, m_serialNumber,
        m_description.isEmpty() ? describePort(QSerialPortInfo(m_name)) : m_description,
        QSerialPortInfo(m_name).portName(), 0, false, true);
}

bool SerialCanBackend::hasBusStatus() const
{
    return true;
}

QCanBusDevice::CanBusStatus SerialCanBackend::busStatus()
{
    if (state() != QCanBusDevice::ConnectedState)
        return QCanBusDevice::CanBusStatus::Unknown;

    quint32 value = 0;
    if (!readErrorRegister(value))
        return QCanBusDevice::CanBusStatus::Unknown;

    return value == 0 ? QCanBusDevice::CanBusStatus::Good : QCanBusDevice::CanBusStatus::Error;
}

void SerialCanBackend::resetController()
{
    if (state() != QCanBusDevice::ConnectedState)
        return;
    if (!command(QByteArrayLiteral("C\r")) || !command(QByteArrayLiteral("O\r")))
        fail(tr("Could not restart the CAN controller of %1").arg(m_name));
}

void SerialCanBackend::setConfigurationParameter(ConfigurationKey key, const QVariant &value)
{
    switch (int(key)) {
    case QCanBusDevice::BitRateKey:
    case QCanBusDevice::DataBitRateKey:
    case QCanBusDevice::RawFilterKey:
    case ModeKey:
    case AutoRetransmitKey:
    case NominalBitrateCodeKey:
    case DataBitrateCodeKey:
        if (state() != QCanBusDevice::UnconnectedState) {
            rejectConfiguration(tr("Configuration parameter 0x%1 can only be changed while disconnected")
                .arg(int(key), 0, 16));
            return;
        }
        break;
    default:
        break;
    }

    switch (int(key)) {
    case QCanBusDevice::BitRateKey:
    case QCanBusDevice::DataBitRateKey:
    case QCanBusDevice::CanFdKey:
    case QCanBusDevice::RawFilterKey:
        break;
    case QCanBusDevice::LoopbackKey:
    case QCanBusDevice::ReceiveOwnKey:
        if (value.toBool()) {
            rejectConfiguration(tr("The WeAct SLCAN firmware cannot loop back or receive its own frames"));
            return;
        }
        break;
    case QCanBusDevice::ErrorFilterKey:
    case QCanBusDevice::ProtocolKey:
        if (value.isValid()) {
            rejectConfiguration(tr("Configuration parameter 0x%1 is not supported by the serial SLCAN backend")
                .arg(int(key), 0, 16));
            return;
        }
        break;
    case ModeKey: {
        const QString mode = value.toString().trimmed().toLower();
        if (mode != QLatin1String("normal") && mode != QLatin1String("silent")) {
            rejectConfiguration(tr("Mode must be \"normal\" or \"silent\", got \"%1\"").arg(value.toString()));
            return;
        }
        break;
    }
    case AutoRetransmitKey:
        break;
    case NominalBitrateCodeKey:
        if (!isValidRawCode(value.toString().toLatin1(), 'S')) {
            rejectConfiguration(tr("Nominal bitrate code must look like \"S6\" or \"S0502\", got \"%1\"")
                .arg(value.toString()));
            return;
        }
        break;
    case DataBitrateCodeKey:
        if (!isValidRawCode(value.toString().toLatin1(), 'Y')) {
            rejectConfiguration(tr("Data bitrate code must look like \"Y5\" or \"Y0902\", got \"%1\"")
                .arg(value.toString()));
            return;
        }
        break;
    default:
        rejectConfiguration(tr("Configuration parameter 0x%1 is not supported by the serial SLCAN backend")
            .arg(int(key), 0, 16));
        return;
    }

    if (key == QCanBusDevice::CanFdKey)
        m_fdEnabled = value.toBool();

    QCanBusDevice::setConfigurationParameter(key, value);
}

void SerialCanBackend::onReadyRead()
{
    m_buffer += m_port.readAll();

    for (;;) {
        while (!m_buffer.isEmpty() && quint8(m_buffer.at(0)) == 0x07) {
            m_buffer.remove(0, 1);
            handleCommandReply(false);
        }
        const int cr = m_buffer.indexOf('\r');
        if (cr < 0)
            return;
        const QByteArray line = m_buffer.left(cr);
        m_buffer.remove(0, cr + 1);
        if (line.isEmpty())
            handleCommandReply(true);
        else
            handleLine(line);
    }
}

void SerialCanBackend::handleLine(const QByteArray &line)
{
    const auto frame = weact::slcan::parse(line);
    if (!frame) {
        if (line.startsWith(ErrorRegisterPrefix))
            return;
        qCWarning(serialcanLog) << "Ignoring unparsable reply from" << m_name << line;
        return;
    }

    if (frame->hasFlexibleDataRateFormat() && !m_fdEnabled) {
        if (!m_fdDropWarned) {
            m_fdDropWarned = true;
            qCWarning(serialcanLog) << "Dropping CAN FD frames received from" << m_name
                                    << "- enable QCanBusDevice::CanFdKey";
        }
        return;
    }

    enqueueReceivedFrames({ *frame });
}

void SerialCanBackend::handleCommandReply(bool ok)
{
    if (m_waitingReply) {
        m_replyOk = ok;
        emit commandFinished();
        return;
    }

    if (!m_commandInFlight || m_pendingCommands.isEmpty()) {
        qCWarning(serialcanLog) << "Unexpected" << (ok ? "acknowledgement" : "failure reply") << "from" << m_name;
        return;
    }

    m_commandInFlight = false;
    m_pendingCommands.removeFirst();
    dequeueOutgoingFrame();

    if (ok)
        emit framesWritten(1);
    else
        fail(tr("The frame was not transmitted: no acknowledgement on the CAN bus"), QCanBusDevice::WriteError);

    transmitNext();
}

void SerialCanBackend::onSerialError(QSerialPort::SerialPortError error)
{
    if (error == QSerialPort::NoError)
        return;

    const bool fatal = error == QSerialPort::ResourceError || error == QSerialPort::DeviceNotFoundError
        || error == QSerialPort::PermissionError || error == QSerialPort::OpenError;

    if (fatal && state() != QCanBusDevice::UnconnectedState) {
        const QString text = m_port.errorString();
        m_port.close();
        while (hasOutgoingFrames())
            dequeueOutgoingFrame();
        m_pendingCommands.clear();
        m_commandInFlight = false;
        setState(QCanBusDevice::UnconnectedState);
        setError(tr("Lost connection to %1: %2").arg(m_name, text), QCanBusDevice::ConnectionError);
        return;
    }

    if (!fatal) {
        setError(tr("%1: %2").arg(m_name, m_port.errorString()), QCanBusDevice::ReadError);
    }
}

bool SerialCanBackend::command(const QByteArray &bytes, int timeoutMs)
{
    if (!m_port.isOpen())
        return false;

    m_replyOk = false;
    m_waitingReply = true;
    if (m_port.write(bytes) != bytes.size()) {
        m_waitingReply = false;
        return false;
    }
    m_port.flush();

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    connect(this, &SerialCanBackend::commandFinished, &loop, &QEventLoop::quit);
    timer.start(timeoutMs);
    loop.exec();

    m_waitingReply = false;
    return m_replyOk;
}

bool SerialCanBackend::readErrorRegister(quint32 &value, int timeoutMs)
{
    // The firmware drops input while it is busy with a transmission, and its answer to `E`
    // has no terminator, so the request is only sent from an idle command stream.
    if (m_commandInFlight || m_waitingReply)
        return false;

    if (m_port.write(QByteArrayLiteral("E\r")) < 0)
        return false;

    bool found = false;
    QElapsedTimer quiet;
    quiet.start();
    int lastSize = m_buffer.size();
    QEventLoop loop;
    QTimer timeout;
    QTimer poll;
    timeout.setSingleShot(true);
    poll.setInterval(10);
    connect(&poll, &QTimer::timeout, this, [&] {
        if (m_buffer.size() != lastSize) {
            lastSize = m_buffer.size();
            quiet.restart();
            return;
        }
        int digits = 0;
        const int valueStart = findErrorRegister(m_buffer, digits);
        if (valueStart < 0 || digits == 0)
            return;
        if (quiet.elapsed() < AnswerSettleMs)
            return;
        bool ok = false;
        const quint32 parsed = m_buffer.mid(valueStart, digits).toUInt(&ok, 16);
        if (!ok)
            return;
        m_buffer.remove(0, valueStart + digits);
        value = parsed;
        found = true;
        loop.quit();
    });
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    poll.start();
    timeout.start(timeoutMs);
    loop.exec();

    if (!found) {
        int digits = 0;
        const int valueStart = findErrorRegister(m_buffer, digits);
        if (valueStart >= 0)
            m_buffer.remove(0, valueStart + digits);
    }
    return found;
}

bool SerialCanBackend::configureDevice()
{
    m_fdEnabled = configurationParameter(QCanBusDevice::CanFdKey).toBool();

    if (!command(QByteArrayLiteral("C\r"))) {
        return fail(tr("%1 does not answer the SLCAN close command - is the WeAct SLCAN firmware running?")
            .arg(m_name));
    }

    if (!command(QByteArrayLiteral("H0\r")))
        return fail(tr("%1 rejected the ASCII framing command").arg(m_name));

    if (!configureMode() || !configureBitrate() || !configureFilters())
        return false;

    if (!command(QByteArrayLiteral("O\r")))
        return fail(tr("%1 rejected the open command - check the bitrate settings").arg(m_name));

    return true;
}

bool SerialCanBackend::configureMode()
{
    const QString mode = configurationParameter(static_cast<QCanBusDevice::ConfigurationKey>(ModeKey)).toString().trimmed().toLower();
    if (!mode.isEmpty())
        m_mode = mode;

    const bool silent = m_mode == QLatin1String("silent");
    if (!command(silent ? QByteArrayLiteral("M1\r") : QByteArrayLiteral("M0\r")))
        return fail(tr("%1 rejected the %2 mode command").arg(m_name, m_mode));

    m_autoRetransmit = configurationParameter(static_cast<QCanBusDevice::ConfigurationKey>(AutoRetransmitKey)).toBool();
    if (!command(m_autoRetransmit ? QByteArrayLiteral("A1\r") : QByteArrayLiteral("A0\r")))
        return fail(tr("%1 rejected the auto retransmission command").arg(m_name));

    return true;
}

bool SerialCanBackend::configureBitrate()
{
    QByteArray nominal;
    const QString rawNominal = configurationParameter(static_cast<QCanBusDevice::ConfigurationKey>(NominalBitrateCodeKey)).toString();
    const QVariant bitrate = configurationParameter(QCanBusDevice::BitRateKey);
    if (!rawNominal.isEmpty()) {
        nominal = rawNominal.toLatin1();
    } else if (bitrate.isValid()) {
        const quint32 requested = bitrate.toUInt();
        if (const auto code = weact::slcan::nominalCode(requested)) {
            nominal = QByteArrayLiteral("S") + char(code->toLatin1());
        } else if (const auto custom = weact::slcan::customNominal(requested)) {
            nominal = custom->toLatin1();
        } else {
            return fail(tr("%1 bit/s is not representable on the 60 MHz nominal clock").arg(requested));
        }
    }
    if (!nominal.isEmpty() && !command(nominal + '\r'))
        return fail(tr("%1 rejected the nominal bitrate command %2").arg(m_name, QString::fromLatin1(nominal)));

    QByteArray data;
    const QString rawData = configurationParameter(static_cast<QCanBusDevice::ConfigurationKey>(DataBitrateCodeKey)).toString();
    const QVariant dataBitrate = configurationParameter(QCanBusDevice::DataBitRateKey);
    if (!rawData.isEmpty()) {
        data = rawData.toLatin1();
    } else if (m_fdEnabled && dataBitrate.isValid()) {
        const quint32 requested = dataBitrate.toUInt();
        if (const auto code = weact::slcan::dataCode(requested)) {
            data = QByteArrayLiteral("Y") + char(code->toLatin1());
        } else if (const auto custom = weact::slcan::customData(requested)) {
            data = custom->toLatin1();
        } else {
            return fail(tr("%1 bit/s is not representable in the CAN FD data phase on the 60 MHz clock")
                .arg(requested));
        }
    }
    if (!data.isEmpty() && !command(data + '\r'))
        return fail(tr("%1 rejected the CAN FD data bitrate command %2").arg(m_name, QString::fromLatin1(data)));

    return true;
}

bool SerialCanBackend::configureFilters()
{
    const QVariant value = configurationParameter(QCanBusDevice::RawFilterKey);
    if (!value.isValid())
        return true;

    const auto filters = value.value<QList<QCanBusDevice::Filter>>();
    if (filters.isEmpty())
        return true;

    std::optional<QCanBusDevice::Filter> standard;
    std::optional<QCanBusDevice::Filter> extended;

    for (const QCanBusDevice::Filter &filter : filters) {
        const char *kind = nullptr;
        if (filter.type != QCanBusFrame::DataFrame) {
            return fail(tr("The device filters identifiers only, frame type 0x%1 cannot be filtered")
                .arg(int(filter.type), 0, 16));
        }
        if (filter.format & QCanBusDevice::Filter::MatchBaseFormat) {
            if (filter.frameId > StandardIdMask || filter.frameIdMask > StandardIdMask)
                return fail(tr("Filter 0x%1/0x%2 does not fit the standard identifier format")
                    .arg(filter.frameId, 0, 16).arg(filter.frameIdMask, 0, 16));
            if (standard && (standard->frameId != filter.frameId || standard->frameIdMask != filter.frameIdMask)) {
                return fail(tr("The device holds a single standard identifier filter, %1 were given")
                    .arg(filters.size()));
            }
            standard = filter;
            kind = "standard";
        }
        if (filter.format & QCanBusDevice::Filter::MatchExtendedFormat) {
            if (filter.frameId > ExtendedIdMask || filter.frameIdMask > ExtendedIdMask)
                return fail(tr("Filter 0x%1/0x%2 does not fit the extended identifier format")
                    .arg(filter.frameId, 0, 16).arg(filter.frameIdMask, 0, 16));
            if (extended && (extended->frameId != filter.frameId || extended->frameIdMask != filter.frameIdMask)) {
                return fail(tr("The device holds a single extended identifier filter, %1 were given")
                    .arg(filters.size()));
            }
            extended = filter;
            kind = "extended";
        }
        if (!kind)
            return fail(tr("Unsupported filter format 0x%1").arg(int(filter.format), 0, 16));
    }

    const QByteArray standardCommand = QByteArrayLiteral("f")
        + weact::slcan::hexText(standard ? standard->frameId : 0, 3)
        + weact::slcan::hexText(standard ? standard->frameIdMask : 0, 3);
    const QByteArray extendedCommand = QByteArrayLiteral("F")
        + weact::slcan::hexText(extended ? extended->frameId : 0, 8)
        + weact::slcan::hexText(extended ? extended->frameIdMask : 0, 8);

    if (!command(standardCommand + '\r') || !command(extendedCommand + '\r'))
        return fail(tr("%1 rejected the identifier filters").arg(m_name));

    return true;
}

bool SerialCanBackend::rejectConfiguration(const QString &reason)
{
    setError(reason, QCanBusDevice::ConfigurationError);
    return false;
}

bool SerialCanBackend::fail(const QString &message, QCanBusDevice::CanBusError error)
{
    setError(message, error);
    return false;
}
