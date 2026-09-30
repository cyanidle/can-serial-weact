#include "slcan.h"

#include <QList>

#include <cmath>

namespace weact::slcan {
namespace {

struct CodeTable
{
    const char *codes;
    quint32 bitrates[16];
    int count;
};

const CodeTable nominalCodes = {
    "0123456789ABCD",
    {10000, 20000, 50000, 100000, 125000, 250000, 500000, 800000,
        1000000, 83333, 75000, 62500, 33333, 5000},
    14,
};

const CodeTable dataCodes = {
    "12345",
    {1000000, 2000000, 3000000, 4000000, 5000000},
    5,
};

bool sameBitrate(quint32 requested, quint32 candidate)
{
    const quint32 tolerance = qMax<quint32>(1, candidate / 500);
    return requested >= candidate - tolerance && requested <= candidate + tolerance;
}

bool isHexDigit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

int hexValue(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

std::optional<quint32> hexValue(const QByteArray &text)
{
    if (text.isEmpty())
        return std::nullopt;
    quint32 value = 0;
    for (char c : text) {
        const int digit = hexValue(c);
        if (digit < 0)
            return std::nullopt;
        value = (value << 4) | quint32(digit);
    }
    return value;
}

QByteArray hex(quint32 value, int digits)
{
    return QByteArray::number(value, 16).rightJustified(digits, '0').toUpper();
}

char dlcChar(quint8 dlc)
{
    return dlc < 10 ? char('0' + dlc) : char('A' + dlc - 10);
}

bool frameIsFd(const QCanBusFrame &frame)
{
    return frame.hasFlexibleDataRateFormat();
}

std::optional<QString> customTiming(quint32 bitrate, int seg1Min, int seg1Max, int seg2Min, int seg2Max, char prefix)
{
    if (bitrate == 0)
        return std::nullopt;
    const quint32 clock = ClockHz / FixedDiv;
    const double total = double(clock) / double(bitrate);
    const quint32 totalTq = quint32(std::llround(total));
    if (totalTq < 3 || !qFuzzyCompare(double(totalTq), total))
        return std::nullopt;
    const int segments = int(totalTq) - 1;
    int seg2 = int(std::lround(totalTq * 0.25));
    seg2 = qBound(seg2Min, seg2, seg2Max);
    int seg1 = segments - seg2;
    if (seg1 < seg1Min || seg1 > seg1Max) {
        seg1 = qBound(seg1Min, seg1, seg1Max);
        seg2 = segments - seg1;
        if (seg2 < seg2Min || seg2 > seg2Max)
            return std::nullopt;
    }
    const QByteArray timings = QByteArray::number(seg1, 16).rightJustified(2, '0')
        + QByteArray::number(seg2, 16).rightJustified(2, '0');
    return QString::fromLatin1(QByteArray(1, prefix) + timings.toUpper());
}

std::optional<QChar> codeFor(const CodeTable &table, quint32 bitrate)
{
    for (int i = 0; i < table.count; ++i) {
        if (sameBitrate(bitrate, table.bitrates[i]))
            return QChar::fromLatin1(table.codes[i]);
    }
    return std::nullopt;
}

}

quint8 dlcToLength(quint8 dlc)
{
    static const quint8 lengths[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64};
    return dlc < 16 ? lengths[dlc] : 0;
}

std::optional<quint8> lengthToDlc(int length)
{
    for (quint8 dlc = 0; dlc < 16; ++dlc) {
        if (dlcToLength(dlc) == length)
            return dlc;
    }
    return std::nullopt;
}

QByteArray encode(const QCanBusFrame &frame, QString &error)
{
    if (frame.frameType() == QCanBusFrame::InvalidFrame) {
        error = QStringLiteral("Cannot write an invalid frame");
        return {};
    }

    const bool extended = frame.hasExtendedFrameFormat();
    const bool fd = frameIsFd(frame);
    const bool remote = frame.frameType() == QCanBusFrame::RemoteRequestFrame;

    if (frame.frameType() != QCanBusFrame::DataFrame && !remote) {
        error = QStringLiteral("Only data and remote request frames can be written, got frame type 0x%1")
            .arg(int(frame.frameType()), 0, 16);
        return {};
    }
    if (frame.frameId() > (extended ? 0x1fffffffU : 0x7ffU)) {
        error = QStringLiteral("Frame id 0x%1 does not fit the %2 identifier format")
            .arg(frame.frameId(), 0, 16).arg(extended ? QStringLiteral("extended") : QStringLiteral("standard"));
        return {};
    }

    const QByteArray payload = frame.payload();
    if (remote && payload.size() > MaxClassicPayload) {
        error = QStringLiteral("Remote request frames carry at most %1 bytes, got %2")
            .arg(MaxClassicPayload).arg(payload.size());
        return {};
    }
    if (!remote && payload.size() > (fd ? MaxFdPayload : MaxClassicPayload)) {
        error = QStringLiteral("%1 frames carry at most %2 bytes, got %3")
            .arg(fd ? QStringLiteral("CAN FD") : QStringLiteral("Classic CAN"))
            .arg(fd ? MaxFdPayload : MaxClassicPayload).arg(payload.size());
        return {};
    }
    if (fd && remote) {
        error = QStringLiteral("CAN FD remote request frames do not exist");
        return {};
    }

    char type = 't';
    if (remote)
        type = 'r';
    else if (fd)
        type = frame.hasBitrateSwitch() ? 'b' : 'd';
    if (extended)
        type = char(type - 'a' + 'A');

    QByteArray command;
    command.reserve(1 + 8 + 1 + 2 * 64 + 1);
    command.append(type);
    command.append(hex(frame.frameId(), extended ? 8 : 3));

    if (remote) {
        const auto dlc = lengthToDlc(payload.size());
        if (!dlc) {
            error = QStringLiteral("Invalid remote request length %1").arg(payload.size());
            return {};
        }
        command.append(dlcChar(*dlc));
    } else {
        const auto dlc = lengthToDlc(payload.size());
        if (!dlc) {
            error = QStringLiteral("Invalid CAN FD payload length %1 (use 0-8, 12, 16, 20, 24, 32, 48 or 64)")
                .arg(payload.size());
            return {};
        }
        command.append(dlcChar(*dlc));
        command.append(payload.toHex().toUpper());
    }
    command.append('\r');
    return command;
}

std::optional<QCanBusFrame> parse(const QByteArray &line)
{
    if (line.size() < 5)
        return std::nullopt;

    const char type = line.at(0);
    const bool fd = type == 'd' || type == 'D' || type == 'b' || type == 'B';
    const bool remote = type == 'r' || type == 'R';
    const bool extended = type == 'T' || type == 'R' || type == 'D' || type == 'B';
    if (!fd && !remote && type != 't' && type != 'T')
        return std::nullopt;

    const int idDigits = extended ? 8 : 3;
    if (line.size() < 1 + idDigits + 1)
        return std::nullopt;

    const auto id = hexValue(line.mid(1, idDigits));
    if (!id || *id > (extended ? 0x1fffffffU : 0x7ffU))
        return std::nullopt;

    const int dlcDigit = hexValue(line.at(1 + idDigits));
    if (dlcDigit < 0)
        return std::nullopt;
    const quint8 dlc = quint8(dlcDigit);
    const int length = dlcToLength(dlc);
    if (!fd && length > MaxClassicPayload)
        return std::nullopt;

    QCanBusFrame frame;
    frame.setFrameId(*id);
    frame.setExtendedFrameFormat(extended);
    frame.setFrameType(remote ? QCanBusFrame::RemoteRequestFrame : QCanBusFrame::DataFrame);
    frame.setFlexibleDataRateFormat(fd);
    frame.setBitrateSwitch(type == 'b' || type == 'B');

    const QByteArray data = line.mid(2 + idDigits);
    if (remote) {
        if (!data.isEmpty())
            return std::nullopt;
        frame.setPayload(QByteArray(length, '\0'));
    } else {
        if (data.size() != length * 2)
            return std::nullopt;
        frame.setPayload(QByteArray::fromHex(data));
    }
    return frame;
}

std::optional<QChar> nominalCode(quint32 bitrate)
{
    return codeFor(nominalCodes, bitrate);
}

std::optional<QChar> dataCode(quint32 bitrate)
{
    return codeFor(dataCodes, bitrate);
}

std::optional<QString> customNominal(quint32 bitrate)
{
    return customTiming(bitrate, NominalSeg1Min, NominalSeg1Max, NominalSeg2Min, NominalSeg2Max, 'S');
}

std::optional<QString> customData(quint32 bitrate)
{
    return customTiming(bitrate, DataSeg1Min, DataSeg1Max, DataSeg2Min, DataSeg2Max, 'Y');
}

}
