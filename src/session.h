#pragma once
#include "protocol.h"
#include <QObject>
#include <QTimer>
#include <memory>

class LampSession : public QObject {
    Q_OBJECT
public:
    explicit LampSession(QObject *parent = nullptr);
    void start(int lampType, const QString &address);
    void stop();
    void receive(const QByteArray &bytes);
    bool isReady() const { return m_active && m_ready; }
    void control(const QByteArray &command, int lampType);
    void queryState(int lampType);
signals:
    void outgoing(const QByteArray &frame);
    void status(const QString &text, bool ready);
    void feedback(const QByteArray &command);
    void failed(const QString &reason);
private:
    void queryVersion();
    void handle(const ilight::Frame &frame);
    void later(int ms, const std::function<void()> &task);
    void sendInner(const QByteArray &command);
    ilight::Decoder m_decoder;
    QTimer m_timeout, m_queryFallback, m_refreshTimer;
    std::unique_ptr<ilight::Handshake> m_handshake;
    QString m_address, m_version;
    int m_type=2, m_generation=0, m_refreshType=2;
    bool m_active=false, m_ready=false, m_querying=false;
};
