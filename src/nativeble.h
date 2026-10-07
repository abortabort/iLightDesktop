#pragma once
#include <QThread>
#include <QMutex>
#include <QQueue>
#include <atomic>

class NativeBle : public QThread {
    Q_OBJECT
public:
    explicit NativeBle(const QString &address) : m_address(address) {}
    ~NativeBle() override { requestStop(); wait(); }
    void requestStop() { m_stopping = true; }
    bool enqueue(const QByteArray &bytes);
    bool hasDevice() const { return m_hasDevice; }
signals:
    void connected();
    void received(const QByteArray &bytes);
    void failed(const QString &message);
    void diagnostic(const QString &message);
protected:
    void run() override;
private:
    QString m_address;
    std::atomic<bool> m_stopping {false};
    std::atomic<bool> m_hasDevice {false};
    QMutex m_mutex;
    QQueue<QByteArray> m_queue;
    int m_queuedBytes = 0;
};
