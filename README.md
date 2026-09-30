# qtserialcan-weact

An out-of-tree [QCanBus](https://doc.qt.io/qt-6/qtcanbus-backends.html) plugin that drives a
**WeAct USB2CANFDV2** stick running the stock **WeAct SLCAN firmware** (USB CDC serial),
including **CAN FD**.

```
QCanBus::createDevice("serialcan", "/dev/ttyACM1")
```

## Why this exists

Qt ships no serial-CAN plugin: the CAN plugins are `socketcan`, `peakcan`, `tinycan`,
`vectorcan`, `systeccan`, `virtualcan` and `passthrucan`. Routing this stick through
SocketCAN means using the kernel `slcan` line discipline, which is **classic CAN only** — the
WeAct firmware's CAN FD support (`Y*`, `d`/`D`/`b`/`B` frames) is a vendor extension of the
LAWICEL/SLCAN ASCII protocol that nothing in the kernel implements. This plugin speaks that
dialect directly over the serial port, so CAN FD works out of the box, together with the
bit rates, filters and modes the firmware offers.

## Build

Requires Qt 6 (`Core`, `SerialPort`, `SerialBus`) and CMake ≥ 3.21.

```bash
cmake -B build -G Ninja -D CMAKE_BUILD_TYPE=Release
cmake --build build -j $(nproc)
ctest --test-dir build --output-on-failure     # protocol codec self-test
```

The plugin lands in `build/canbus/serialcan.so` (a `canbus/` subdirectory is what Qt's plugin
loader scans).

### Use it from a build tree

```bash
QT_PLUGIN_PATH=$PWD/build your-app            # or export QT_PLUGIN_PATH
```

### Install

```bash
sudo cmake --install build                     # -> Qt's own plugin directory + /canbus
```

The destination defaults to `${QT6_INSTALL_PLUGINS}/canbus` (the Qt installation's plugin
directory, e.g. `/usr/lib/x86_64-linux-gnu/qt6/plugins/canbus`); `-D SERIALCAN_PLUGIN_DIR=<dir>`
overrides it. Configure with the same `CMAKE_INSTALL_PREFIX` you later install with, so Qt
reports the matching plugin directory.

## Use

```cpp
#include <QCanBus>
#include <QCanBusDevice>

QString error;
QCanBusDevice *device = QCanBus::instance()->createDevice("serialcan", "/dev/ttyACM1", &error);

device->setConfigurationParameter(QCanBusDevice::BitRateKey, 500000);
device->setConfigurationParameter(QCanBusDevice::CanFdKey, true);
device->setConfigurationParameter(QCanBusDevice::DataBitRateKey, 5000000);

QObject::connect(device, &QCanBusDevice::framesReceived, [device] {
    while (device->framesAvailable()) {
        const QCanBusFrame frame = device->readFrame();
        qDebug() << frame.frameId() << frame.payload().toHex();
    }
});

device->connectDevice();

QCanBusFrame frame(0x123, QByteArray::fromHex("DEADBEEF"));
device->writeFrame(frame);

QCanBusFrame fd(0x123, QByteArray(64, '\xAA'));
fd.setFlexibleDataRateFormat(true);
fd.setBitrateSwitch(true);
device->writeFrame(fd);
```

Discovery: `QCanBus::availableDevices("serialcan")` lists serial ports that look like this
device (USB vendor/product `0483:5740`, or a description mentioning USB2CAN/SLCAN/CANable/WeAct).
`createDevice()` also accepts any existing serial port path (e.g. a symlink), so a device that
is not auto-detected can still be used explicitly.

## Configuration

| Key | Support |
|---|---|
| `BitRateKey` | Standard S-codes (10k, 20k, 50k, 100k, 125k, 250k, 500k, 800k, 1M plus 83.3k, 75k, 62.5k, 33.3k, 5k); any other rate that is exact on the 60 MHz nominal clock becomes a custom `Sxxyy` command, otherwise `open()` fails |
| `DataBitRateKey` | `Y1`..`Y5` for 1..5 Mbit/s, otherwise custom `Yxxyy`; only applied when `CanFdKey` is enabled |
| `CanFdKey` | Enables CAN FD: gates FD writes and decides whether FD frames are delivered (default off, like the other Qt plugins) |
| `RawFilterKey` | One standard (`fIIImmm`) and one extended (`FIIIIIIIIMMMMMMMM`) identifier filter, which is what the firmware holds; filters are matched by identifier only, so `type` must be a data frame |
| `LoopbackKey`, `ReceiveOwnKey` | Rejected when set to true — the firmware cannot loop back |
| `ErrorFilterKey`, `ProtocolKey` | Rejected when set — the firmware reports no error frames, and this is not a socket |
| `ModeKey` (`UserKey + 1`) | `"normal"` (default) or `"silent"` (`M0`/`M1`) |
| `AutoRetransmitKey` (`UserKey + 2`) | `bool` (`A0`/`A1`). The vendor notes that enabling it "may crash"; default off |
| `NominalBitrateCodeKey` (`UserKey + 3`) | Raw nominal code, e.g. `"S6"` or `"S0502"` — escape hatch for exact `Sxxyy`/`Sddxxyy` control |
| `DataBitrateCodeKey` (`UserKey + 4`) | Raw data phase code, e.g. `"Y5"` or `"Y0902"` |

All of these except `CanFdKey` must be set **before** `connectDevice()`; changing them on a
connected device is rejected.

`hasBusStatus()` / `busStatus()` are implemented through the firmware's `E` command (CAN error
register). `resetController()` restarts the CAN channel with `C` + `O`.

## Protocol notes

The plugin drives the device in the ASCII SLCAN dialect the firmware documents (and which the
bundled Cangaroo uses). Reverse-engineering the firmware image confirmed the receive direction,
which the vendor's README does not spell out:

* frames are `<t|T|r|R|d|D|b|B><id hex><dlc char><data hex><CR>`, uppercase hex, terminated by
  `CR`. Lowercase letters mean standard identifiers, uppercase letters extended ones; `d`/`D` are
  CAN FD without BRS, `b`/`B` are CAN FD with BRS;
* the character after the identifier is the **DLC code**, not a byte count: `0`-`8` and then
  `9`,`A`..`F` for 12, 16, 20, 24, 32, 48, 64 bytes;
* a command is acknowledged with a bare `CR`; a single `0x07` byte means failure (for a frame,
  "no acknowledgement on the bus");
* the `E` response (`CANable Error Register: <hex>`) is **not** CR-terminated;
* the firmware handles one command at a time and drops input while it is busy, so the plugin
  sends frames strictly one by one, each after the previous reply, and reports
  `framesWritten()` per acknowledged frame;
* the binary "enhance mode" (`H1`, `0x80 | <type>` framing) exists in the firmware but is not
  implemented here — the plugin always selects `H0`.

Bit rates: the firmware derives timings from a 120 MHz clock divided by 2 for the two-byte
custom forms (`Sxxyy`, `Yxxyy`). This plugin synthesises those with a ~75% sample point when no
standard code matches; use the raw code keys if you need an exact sample point.

## Tests

* `ctest` runs `slcan_codec_test`, which covers frame encoding/decoding, the DLC table and bit
  rate code selection.
* `serialcan_probe` is a small manual tool: with no arguments it lists the CAN plugins and
  devices Qt can see, with arguments it opens a device and exercises it:

  ```bash
  QT_PLUGIN_PATH=$PWD/build ./build/serialcan_probe                    # list devices
  QT_PLUGIN_PATH=$PWD/build ./build/serialcan_probe /dev/ttyACM1 500000 5000000 [id:mask]
  ```

  On a bus with no other node the writes fail with "no acknowledgement on the bus"; that is the
  device answering `0x07` and the plugin reporting it as a write error.

To exercise the plugin without hardware, pair two pseudo terminals and play a fake device:

```bash
socat pty,raw,echo=0,link=/tmp/slcan_dev pty,raw,echo=0,link=/tmp/slcan_host &
# answer every command with CR and emit frame lines on /tmp/slcan_dev, then:
QT_PLUGIN_PATH=$PWD/build ./build/serialcan_probe /tmp/slcan_host 1000000 5000000
```

## Limitations

* One channel (the stick has a single FDCAN port), classic CAN and CAN FD only.
* No hardware timestamps: `QCanBusFrame::TimeStamp` is left as the device delivered it.
* The firmware has no loopback, no error-frame reporting and no hardware acceptance filtering
  beyond the single standard/extended ID filter pair.
* Binary enhance mode, the firmware update command (`X`) and the `Sddxxyy`/`Yddxxyy` divisor
  forms are not exposed (the raw code keys can still send the two-byte custom forms).
