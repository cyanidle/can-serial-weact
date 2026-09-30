#include "slcan.h"

#include <QCanBusFrame>
#include <QCoreApplication>

#include <cstdio>

using namespace weact::slcan;

static int failures = 0;

static void check(bool condition, const char *what)
{
    if (condition)
        return;
    ++failures;
    std::fprintf(stderr, "FAIL: %s\n", what);
}

static void checkEqual(const QByteArray &actual, const QByteArray &expected, const char *what)
{
    if (actual == expected)
        return;
    ++failures;
    std::fprintf(stderr, "FAIL: %s (got '%s', expected '%s')\n", what, actual.constData(), expected.constData());
}

static QCanBusFrame classicFrame(quint32 id, const QByteArray &payload, bool extended = false)
{
    QCanBusFrame frame{QCanBusFrame::FrameId(id), payload};
    frame.setExtendedFrameFormat(extended);
    return frame;
}

static void testDlcTable()
{
    check(dlcToLength(8) == 8, "dlc 8 -> 8 bytes");
    check(dlcToLength(9) == 12, "dlc 9 -> 12 bytes");
    check(dlcToLength(15) == 64, "dlc F -> 64 bytes");
    check(lengthToDlc(64) == 15, "64 bytes -> dlc F");
    check(!lengthToDlc(9).has_value(), "9 bytes is not a CAN FD length");
}

static void testEncode()
{
    QString error;

    checkEqual(encode(classicFrame(0x123, QByteArray::fromHex("DEADBEEF")), error),
        QByteArrayLiteral("t1234DEADBEEF\r"), "classic standard frame");
    checkEqual(encode(classicFrame(0x123, QByteArray::fromHex("DEADBEEF"), true), error),
        QByteArrayLiteral("T000001234DEADBEEF\r"), "classic extended frame");

    QCanBusFrame fd(classicFrame(0x123, QByteArray(64, '\xAA')));
    fd.setFlexibleDataRateFormat(true);
    fd.setBitrateSwitch(true);
    checkEqual(encode(fd, error), QByteArrayLiteral("b123F") + QByteArray(128, 'A') + QByteArrayLiteral("\r"),
        "CAN FD frame with BRS");

    fd.setBitrateSwitch(false);
    checkEqual(encode(fd, error), QByteArrayLiteral("d123F") + QByteArray(128, 'A') + QByteArrayLiteral("\r"),
        "CAN FD frame without BRS");

    fd.setExtendedFrameFormat(true);
    checkEqual(encode(fd, error), QByteArrayLiteral("D00000123F") + QByteArray(128, 'A') + QByteArrayLiteral("\r"),
        "CAN FD extended frame");

    QCanBusFrame remote(classicFrame(0x123, QByteArray(8, '\0')));
    remote.setFrameType(QCanBusFrame::RemoteRequestFrame);
    checkEqual(encode(remote, error), QByteArrayLiteral("r1238\r"), "remote request frame");

    check(encode(classicFrame(0x123, QByteArray(9, '\0')), error).isEmpty(), "9 bytes do not fit classic CAN");
    check(encode(classicFrame(0x800, QByteArray(1, '\0')), error).isEmpty(), "0x800 does not fit a standard id");
    check(encode(classicFrame(0x123, QByteArray(10, '\0')), error).isEmpty(), "10 bytes is not a CAN FD length");

    QCanBusFrame fdShort(classicFrame(0x7ff, QByteArray(12, '\x11')));
    fdShort.setFlexibleDataRateFormat(true);
    checkEqual(encode(fdShort, error), QByteArrayLiteral("d7FF9") + QByteArray(24, '1') + QByteArrayLiteral("\r"),
        "CAN FD 12 byte payload uses dlc 9");
}

static void testParse()
{
    const auto classic = parse(QByteArrayLiteral("t1234DEADBEEF"));
    check(classic.has_value(), "parse classic frame");
    if (classic) {
        check(classic->frameId() == 0x123, "parsed classic id");
        check(classic->payload() == QByteArray::fromHex("DEADBEEF"), "parsed classic payload");
        check(!classic->hasExtendedFrameFormat(), "parsed classic format");
        check(!classic->hasFlexibleDataRateFormat(), "parsed classic is not FD");
    }

    const auto ext = parse(QByteArrayLiteral("T1F33445581122334455667788"));
    check(ext && ext->frameId() == 0x1f334455 && ext->hasExtendedFrameFormat(), "parse extended frame");
    check(ext && ext->payload().size() == 8, "parsed extended payload length");

    check(!parse(QByteArrayLiteral("T1F3344558112233445566778")).has_value(), "odd payload digit count is rejected");

    const auto fd = parse(QByteArrayLiteral("b123F") + QByteArray(128, 'A'));
    check(fd && fd->hasFlexibleDataRateFormat() && fd->hasBitrateSwitch(), "parse CAN FD frame with BRS");
    check(fd && fd->payload().size() == 64, "parsed CAN FD payload length");
    check(fd && fd->payload() == QByteArray(64, '\xaa'), "parsed CAN FD payload bytes");

    const auto fdNoBrs = parse(QByteArrayLiteral("D00000123A") + QByteArray(32, '5'));
    check(fdNoBrs && !fdNoBrs->hasBitrateSwitch() && fdNoBrs->payload().size() == 16,
        "parse CAN FD extended frame without BRS");

    const auto remote = parse(QByteArrayLiteral("r1238"));
    check(remote && remote->frameType() == QCanBusFrame::RemoteRequestFrame && remote->payload().size() == 8,
        "parse remote request frame");

    check(!parse(QByteArrayLiteral("t1239DEADBEEF")).has_value(), "classic frame with dlc 9 is rejected");
    check(!parse(QByteArrayLiteral("t1234DEADBE")).has_value(), "truncated payload is rejected");
    check(!parse(QByteArrayLiteral("WeAct Studio V1.0.0.0")).has_value(), "text is not a frame");
    check(!parse(QByteArrayLiteral("t12Z4DEADBEEF")).has_value(), "non hex digits are rejected");
}

static void testBitrates()
{
    check(nominalCode(500000) == QChar('6'), "500 kbit/s is S6");
    check(nominalCode(1000000) == QChar('8'), "1 Mbit/s is S8");
    check(nominalCode(10000) == QChar('0'), "10 kbit/s is S0");
    check(nominalCode(83300) == QChar('9'), "83.3 kbit/s is S9");
    check(!nominalCode(400000).has_value(), "400 kbit/s has no standard code");

    check(dataCode(5000000) == QChar('5'), "5 Mbit/s is Y5");
    check(dataCode(2000000) == QChar('2'), "2 Mbit/s is Y2");
    check(!dataCode(6000000).has_value(), "6 Mbit/s has no standard code");

    check(customNominal(1000000) == QStringLiteral("S2C0F"), "1 Mbit/s custom timing uses a 75% sample point");
    check(customData(5000000) == QStringLiteral("Y0803"), "5 Mbit/s data phase custom timing");
    check(!customNominal(123456).has_value(), "123.456 kbit/s is not exact on the 60 MHz clock");

    check(customNominal(1000000)->toLatin1().size() == 5, "custom nominal code keeps the Sxxyy shape");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    testDlcTable();
    testEncode();
    testParse();
    testBitrates();

    if (failures == 0) {
        std::printf("all slcan codec checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "%d checks failed\n", failures);
    return 1;
}
