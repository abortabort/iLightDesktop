#pragma once
#include <QThread>
#include <QString>

class NativeAudio : public QThread {
    Q_OBJECT
public:
    NativeAudio(const QString &address, quintptr window, QObject *parent);
signals:
    void status(const QString &text);
    void diagnostic(const QString &text);
protected:
    void run() override;
private:
    QString m_address;
    quintptr m_window;
};
