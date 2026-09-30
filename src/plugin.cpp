#include "serialcanbackend.h"

#include <QCanBusFactory>
#include <QFileInfo>
#include <QSerialPortInfo>
#include <QStringList>

#include <utility>

class SerialCanPlugin : public QObject, public QCanBusFactory
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.qt-project.Qt.QCanBusFactory" FILE "plugin.json")
    Q_INTERFACES(QCanBusFactory)

public:
    QCanBusDevice *createDevice(const QString &interfaceName, QString *errorMessage) const override
    {
        if (interfaceName.isEmpty()) {
            if (errorMessage)
                *errorMessage = tr("No serial port name was given");
            return nullptr;
        }

        if (!QFileInfo::exists(interfaceName) && !isEnumerated(interfaceName)) {
            if (errorMessage) {
                *errorMessage = tr("'%1' is not an available serial port. Available SLCAN devices: [%2]")
                    .arg(interfaceName, availableNames().join(QStringLiteral(", ")));
            }
            return nullptr;
        }

        return new SerialCanBackend(interfaceName);
    }

    QList<QCanBusDeviceInfo> availableDevices(QString *errorMessage) const override
    {
        Q_UNUSED(errorMessage);
        return SerialCanBackend::interfaces();
    }

private:
    static bool isEnumerated(const QString &interfaceName)
    {
        const auto ports = QSerialPortInfo::availablePorts();
        for (const QSerialPortInfo &info : std::as_const(ports)) {
            if (info.systemLocation() == interfaceName || info.portName() == interfaceName)
                return true;
        }
        return false;
    }

    static QStringList availableNames()
    {
        QStringList names;
        const auto ports = QSerialPortInfo::availablePorts();
        for (const QSerialPortInfo &info : std::as_const(ports)) {
            if (SerialCanBackend::looksLikeSlcanDevice(info))
                names.append(info.systemLocation());
        }
        return names;
    }
};

#include "plugin.moc"
