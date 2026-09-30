#pragma once

#include <QCanBusFrame>
#include <QByteArray>
#include <QChar>
#include <QString>

#include <optional>

namespace weact::slcan {

inline constexpr quint32 ClockHz = 120'000'000;
inline constexpr quint32 FixedDiv = 2;

inline constexpr int NominalSeg1Min = 0x02;
inline constexpr int NominalSeg1Max = 0xff;
inline constexpr int NominalSeg2Min = 0x02;
inline constexpr int NominalSeg2Max = 0x80;

inline constexpr int DataSeg1Min = 0x01;
inline constexpr int DataSeg1Max = 0x20;
inline constexpr int DataSeg2Min = 0x01;
inline constexpr int DataSeg2Max = 0x10;

inline constexpr int MaxClassicPayload = 8;
inline constexpr int MaxFdPayload = 64;

bool isHexDigit(char c);

QByteArray hexText(quint32 value, int digits);

quint8 dlcToLength(quint8 dlc);
std::optional<quint8> lengthToDlc(int length);

QByteArray encode(const QCanBusFrame &frame, QString &error);
std::optional<QCanBusFrame> parse(const QByteArray &line);

std::optional<QChar> nominalCode(quint32 bitrate);
std::optional<QChar> dataCode(quint32 bitrate);
std::optional<QString> customNominal(quint32 bitrate);
std::optional<QString> customData(quint32 bitrate);

}
