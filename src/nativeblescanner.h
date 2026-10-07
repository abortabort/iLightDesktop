#pragma once
#include <QThread>
#include <atomic>

class NativeBleScanner : public QThread {
    Q_OBJECT
public:
    ~NativeBleScanner() override { requestStop(); wait(); }
    void requestStop() { m_stopping = true; }
signals:
    void deviceFound(const QString &name, const QString &address);
    void diagnostic(const QString &message);
protected:
    void run() override;
private:
    std::atomic<bool> m_stopping{false};
};
