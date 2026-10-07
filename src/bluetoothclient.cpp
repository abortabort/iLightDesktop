#include "bluetoothclient.h"
#include <QBluetoothLocalDevice>
#include <QBluetoothUuid>
#include <QMetaEnum>
#ifdef Q_OS_WIN
#include "nativerfcomm.h"
#include "nativeble.h"
#include "nativeblescanner.h"
#endif

BluetoothClient::BluetoothClient(QObject *parent) : QObject(parent), m_discovery(this), m_session(this) {
    m_connectTimeout.setSingleShot(true);
    connect(&m_connectTimeout,&QTimer::timeout,this,[this] { fail(QStringLiteral("连接超时，请检查灯具电源、Windows 配对状态与蓝牙适配器")); });
    connect(&m_discovery,&QBluetoothDeviceDiscoveryAgent::deviceDiscovered,this,[this](const QBluetoothDeviceInfo &info) {
        emit diagnostic(QStringLiteral("DISCOVER name=%1 address=%2 core=%3 rssi=%4 services=%5")
            .arg(info.name(),info.address().toString()).arg(int(info.coreConfigurations())).arg(info.rssi())
            .arg(info.serviceUuids().size()));
        if (info.coreConfigurations() & (QBluetoothDeviceInfo::BaseRateCoreConfiguration | QBluetoothDeviceInfo::LowEnergyCoreConfiguration))
            emit deviceFound(info.name(),info.address().toString());
    });
    connect(&m_discovery,&QBluetoothDeviceDiscoveryAgent::finished,this,[this] {
#ifdef Q_OS_WIN
        if (m_scanner) return;
#endif
        emit searchState(false,QStringLiteral("搜索完成"));
    });
    connect(&m_discovery,&QBluetoothDeviceDiscoveryAgent::canceled,this,[this] {
#ifdef Q_OS_WIN
        if (m_scanner) return;
#endif
        emit searchState(false,QStringLiteral("已停止搜索"));
    });
    connect(&m_discovery,QOverload<QBluetoothDeviceDiscoveryAgent::Error>::of(&QBluetoothDeviceDiscoveryAgent::error),this,[this](auto) {
#ifdef Q_OS_WIN
        if (m_scanner) { emit diagnostic(QStringLiteral("Qt discovery: %1; native BLE discovery continues").arg(m_discovery.errorString())); return; }
#endif
        emit searchState(false,QStringLiteral("搜索失败：%1").arg(m_discovery.errorString()));
    });
    connect(&m_session,&LampSession::outgoing,this,[this](const QByteArray &bytes) {
#ifdef Q_OS_WIN
        if (m_ble) {
            if (!m_ble->enqueue(bytes)) { fail(QStringLiteral("BLE 发送队列不可用，请重新连接")); return; }
            emit trace("TX", bytes); return;
        }
        if (m_native) {
            if (!m_native->enqueue(bytes)) { fail(QStringLiteral("RFCOMM 发送队列不可用，请重新连接")); return; }
            emit trace("TX",bytes); return;
        }
#endif
        if (!m_socket || m_socket->state()!=QBluetoothSocket::ConnectedState) { fail(QStringLiteral("蓝牙连接已中断")); return; }
        if (m_socket->bytesToWrite()>16384) { fail(QStringLiteral("灯具发送队列阻塞，请重新连接")); return; }
        if (m_socket->write(bytes)!=bytes.size()) { fail(QStringLiteral("蓝牙写入失败：%1").arg(m_socket->errorString())); return; }
        emit trace("TX",bytes);
    });
    connect(&m_session,&LampSession::status,this,&BluetoothClient::status);
    connect(&m_session,&LampSession::feedback,this,&BluetoothClient::feedback);
    connect(&m_session,&LampSession::failed,this,&BluetoothClient::fail);
}
BluetoothClient::~BluetoothClient() { stopSearch(); disposeSocket(); }
void BluetoothClient::search() {
#ifdef Q_OS_WIN
    if (m_scanner) { stopSearch(); return; }
    m_scanner = new NativeBleScanner;
    auto scanner = m_scanner;
    connect(scanner, &NativeBleScanner::deviceFound, this, [this, scanner](const QString &name, const QString &address) {
        if (m_scanner == scanner) emit deviceFound(name, address);
    });
    connect(scanner, &NativeBleScanner::diagnostic, this, [this, scanner](const QString &message) {
        if (m_scanner == scanner) emit diagnostic(message);
    });
    connect(scanner, &QThread::finished, this, [this, scanner] {
        if (m_scanner != scanner) return;
        if (m_discovery.isActive()) m_discovery.stop();
        m_scanner = nullptr; scanner->deleteLater();
        emit searchState(false, QStringLiteral("搜索完成"));
    });
    emit searchState(true, QStringLiteral("正在搜索附近灯具…"));
    scanner->start();
    m_discovery.start(QBluetoothDeviceDiscoveryAgent::ClassicMethod);
    return;
#endif
    if (m_discovery.isActive()) { m_discovery.stop(); return; }
    QBluetoothLocalDevice local;
    if (!local.isValid()) { emit searchState(false,QStringLiteral("未检测到蓝牙适配器，请检查 Windows 蓝牙设置")); return; }
    if (local.hostMode()==QBluetoothLocalDevice::HostPoweredOff) { emit searchState(false,QStringLiteral("蓝牙已关闭，请在 Windows 设置中开启")); return; }
    emit searchState(true,QStringLiteral("正在搜索附近灯具…"));
    m_discovery.setLowEnergyDiscoveryTimeout(12000);
    m_discovery.start(QBluetoothDeviceDiscoveryAgent::supportedDiscoveryMethods());
}
void BluetoothClient::stopSearch() {
#ifdef Q_OS_WIN
    if (m_scanner) {
        auto old = m_scanner; m_scanner = nullptr;
        old->disconnect(this); old->requestStop(); old->wait(); old->deleteLater();
        emit searchState(false, QStringLiteral("已停止搜索"));
    }
#endif
    if (m_discovery.isActive()) m_discovery.stop();
}
void BluetoothClient::connectDevice(const QString &address,int type) {
    disconnectDevice(); stopSearch();
    const QBluetoothAddress mac(address);
    if (mac.isNull()) { emit status(QStringLiteral("设备地址无效，请输入完整蓝牙地址"),false); return; }
#ifdef Q_OS_WIN
    connectRfcomm(address, type); return;
#endif
    m_socket=new QBluetoothSocket(QBluetoothServiceInfo::RfcommProtocol,this);
    auto current=m_socket;
    QBluetoothLocalDevice local;
    emit diagnostic(QStringLiteral("CONNECT address=%1 type=%2 pairing=%3 uuid=00001101-0000-1000-8000-00805f9b34fb")
                        .arg(address).arg(type).arg(int(local.pairingStatus(mac))));
    m_socket->setPreferredSecurityFlags(QBluetooth::NoSecurity);
    connect(m_socket,&QBluetoothSocket::stateChanged,this,[this,current](QBluetoothSocket::SocketState state) {
        if (m_socket!=current) return;
        const auto name=QMetaEnum::fromType<QBluetoothSocket::SocketState>().valueToKey(int(state));
        emit diagnostic(QStringLiteral("SOCKET state=%1 (%2)").arg(QString::fromLatin1(name ? name : "unknown")).arg(int(state)));
    });
    connect(m_socket,&QBluetoothSocket::connected,this,[this,current,type,address] {
        if (m_socket!=current) return;
        m_connectTimeout.stop(); m_session.start(type,address);
    });
    connect(m_socket,&QBluetoothSocket::readyRead,this,[this,current] {
        if (m_socket!=current) return;
        const auto bytes=m_socket->readAll(); emit trace("RX",bytes); m_session.receive(bytes);
    });
    connect(m_socket,&QBluetoothSocket::disconnected,this,[this,current] {
        if (m_socket==current) fail(QStringLiteral("灯具连接已断开，可重新连接"));
    });
    connect(m_socket,QOverload<QBluetoothSocket::SocketError>::of(&QBluetoothSocket::error),this,[this,current](auto error) {
        if (m_socket==current) {
            emit diagnostic(QStringLiteral("SOCKET error=%1 state=%2 message=%3").arg(int(error)).arg(int(m_socket->state())).arg(m_socket->errorString()));
            fail(QStringLiteral("灯控连接失败：%1。请先断开手机与灯具的连接，再查看通信日志。").arg(m_socket->errorString()));
        }
    });
    emit status(QStringLiteral("正在连接灯具…"),false); m_connectTimeout.start(12000);
    m_socket->connectToService(mac,QBluetoothUuid(QStringLiteral("00001101-0000-1000-8000-00805f9b34fb")));
}
void BluetoothClient::disposeSocket() {
    m_connectTimeout.stop(); m_session.stop();
#ifdef Q_OS_WIN
    if (m_ble) {
        auto old = m_ble; m_ble = nullptr;
        old->disconnect(this); old->requestStop(); old->wait(); old->deleteLater();
    }
    if (m_native) {
        auto old = m_native; m_native = nullptr;
        old->disconnect(this); old->requestStop(); old->wait(); old->deleteLater();
    }
#endif
    if (m_socket) {
        auto old=m_socket; m_socket=nullptr;
        old->disconnect(this); old->abort(); old->deleteLater();
    }
}
void BluetoothClient::disconnectDevice() { disposeSocket(); emit status(QStringLiteral("未连接 · 选择灯具开始"),false); }
void BluetoothClient::fail(const QString &reason) { disposeSocket(); emit status(reason,false); }
void BluetoothClient::control(const QByteArray &command,int type) {
    if (!m_session.isReady()) return;
    m_session.control(command,type);
    if (m_session.isReady()) emit commandSent();
}
void BluetoothClient::refresh(int type) { m_session.queryState(type); }

#ifdef Q_OS_WIN
void BluetoothClient::connectBle(const QString &address, int type) {
    m_ble = new NativeBle(address);
    auto ble = m_ble;
    connect(ble, &NativeBle::diagnostic, this, [this, ble](const QString &text) { if (m_ble == ble) emit diagnostic(text); });
    connect(ble, &NativeBle::received, this, [this, ble](const QByteArray &bytes) {
        if (m_ble != ble) return;
        emit trace("RX", bytes); m_session.receive(bytes);
    });
    connect(ble, &NativeBle::connected, this, [this, ble, address, type] {
        if (m_ble != ble) return;
        m_connectTimeout.stop(); m_session.start(type, address);
    });
    connect(ble, &NativeBle::failed, this, [this, ble](const QString &reason) {
        if (m_ble != ble) return;
        emit diagnostic(reason);
        fail(reason);
    });
    emit status(QStringLiteral("正在通过 Windows BLE 连接灯具…"), false);
    m_connectTimeout.start(100000); ble->start();
}
void BluetoothClient::connectRfcomm(const QString &address, int type) {
    m_rfcommConnected = false;
    m_native = new NativeRfcomm(address);
    auto native = m_native;
    connect(native, &NativeRfcomm::diagnostic, this, [this, native](const QString &text) { if (m_native == native) emit diagnostic(text); });
    connect(native, &NativeRfcomm::connected, this, [this, native, type, address] {
        if (m_native != native) return;
        m_rfcommConnected = true;
        m_connectTimeout.stop(); m_session.start(type, address);
    });
    connect(native, &NativeRfcomm::received, this, [this, native](const QByteArray &bytes) {
        if (m_native != native) return;
        emit trace("RX", bytes); m_session.receive(bytes);
    });
    connect(native, &NativeRfcomm::failed, this, [this, native, address, type](const QString &reason) {
        if (m_native != native) return;
        if (!m_rfcommConnected) {
            emit diagnostic(reason); disposeSocket(); connectBle(address, type); return;
        }
        fail(QStringLiteral("原生 RFCOMM 连接失败：%1").arg(reason));
    });
    emit status(QStringLiteral("正在通过 Windows RFCOMM 连接灯具…"), false);
    m_connectTimeout.start(25000); native->start();
}
#endif
