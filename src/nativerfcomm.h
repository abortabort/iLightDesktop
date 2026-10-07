#ifndef NATIVERFCOMM_H
#define NATIVERFCOMM_H

#include <QThread>
#include <QByteArray>
#include <QMutex>
#include <QQueue>
#include <atomic>

// Socket operations run off the GUI thread. No Windows pairing is requested here.
class NativeRfcomm : public QThread {
    Q_OBJECT
public:
    explicit NativeRfcomm(const QString &address);
    ~NativeRfcomm() override;
    bool enqueue(const QByteArray &bytes);
    void requestStop();
signals:
    void connected();
    void received(const QByteArray &bytes);
    void failed(const QString &message);
    void diagnostic(const QString &message);
protected:
    void run() override;
private:
    void closeSocket();
    QString m_address;
    std::atomic<bool> m_stopping {false};
    std::atomic<quintptr> m_socket {~quintptr(0)};
    QMutex m_queueMutex;
    QQueue<QByteArray> m_queue;
    int m_queuedBytes = 0;
};

#endif
