#include "maindialog.h"
#include <QApplication>
#include <QFont>
#include <QStyleFactory>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QLoggingCategory>
#include <QMutex>
#include <QMutexLocker>
#include <QTextStream>
#include <QCommandLineParser>
#include <QRegularExpression>
#include <QTimer>

namespace {
QString logPath;
QMutex logMutex;
void writeLog(QtMsgType type, const QMessageLogContext &, const QString &message) {
    QMutexLocker locker(&logMutex);
    QFile file(logPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) return;
    QTextStream stream(&file); stream.setCodec("UTF-8");
    stream << QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))
           << " [" << int(type) << "] [pid=" << QCoreApplication::applicationPid() << "] " << message << '\n';
}
}

int main(int argc, char *argv[]) {
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("iLight Desktop"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QCoreApplication::setOrganizationName(QStringLiteral("iLight"));
    logPath = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("ilight.log"));
    QFile log(logPath);
    if (log.size() > 2 * 1024 * 1024) {
        QFile::remove(logPath + QStringLiteral(".old"));
        log.rename(logPath + QStringLiteral(".old"));
    }
    qInstallMessageHandler(writeLog);
    QLoggingCategory::setFilterRules(QStringLiteral("qt.bluetooth.*=true"));
    qInfo("iLight Desktop startup; Qt %s", qVersion());
    QCommandLineParser parser;
    parser.addHelpOption();
    QCommandLineOption connectOption(QStringLiteral("connect"), QStringLiteral("Connect to a Bluetooth MAC address"), QStringLiteral("address"));
    parser.addOption(connectOption);
    QCommandLineOption scanOption(QStringLiteral("scan"), QStringLiteral("Discover nearby Bluetooth devices"));
    parser.addOption(scanOption);
    parser.process(app);
    const QString address = parser.value(connectOption).trimmed().toUpper();
    if (parser.isSet(connectOption) && !QRegularExpression(QStringLiteral("^[0-9A-F]{2}(:[0-9A-F]{2}){5}$")).match(address).hasMatch()) {
        qWarning("Invalid Bluetooth MAC address");
        return 2;
    }
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    app.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 9));
    MainDialog dialog;
    dialog.show();
    if (parser.isSet(connectOption)) QTimer::singleShot(0, &dialog, [&dialog, address] { dialog.connectToAddress(address); });
    else if (parser.isSet(scanOption)) QTimer::singleShot(0, &dialog, &MainDialog::searchDevices);
    return app.exec();
}
