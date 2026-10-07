#pragma once
#include "session.h"
#include <QBluetoothDeviceDiscoveryAgent>
#include <QBluetoothSocket>
#include <QObject>
#include <QTimer>
class NativeRfcomm;
class NativeBle;
class NativeBleScanner;

class BluetoothClient : public QObject {
    Q_OBJECT
public:
    explicit BluetoothClient(QObject *parent=nullptr);
    ~BluetoothClient() override;
    void search();
    void stopSearch();
    void connectDevice(const QString &address, int type);
    void disconnectDevice();
    void control(const QByteArray &command, int type);
    void refresh(int type);
    bool isReady() const { return m_session.isReady(); }
signals:
    void deviceFound(const QString &name, const QString &address);
    void searchState(bool active, const QString &message);
    void status(const QString &message, bool ready);
    void feedback(const QByteArray &command);
    void trace(const QString &direction, const QByteArray &bytes);
    void commandSent();
    void diagnostic(const QString &message);
private:
    void disposeSocket();
    void fail(const QString &reason);
    QBluetoothDeviceDiscoveryAgent m_discovery;
    QBluetoothSocket *m_socket=nullptr;
    LampSession m_session;
    QTimer m_connectTimeout;
#ifdef Q_OS_WIN
    NativeRfcomm *m_native = nullptr;
    NativeBle *m_ble = nullptr;
    NativeBleScanner *m_scanner = nullptr;
    bool m_rfcommConnected = false;
    void connectRfcomm(const QString &address, int type);
    void connectBle(const QString &address, int type);
#endif
};
