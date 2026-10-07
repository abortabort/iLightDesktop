#include "maindialog.h"
#include "bluetoothclient.h"
#include "protocol.h"
#ifdef Q_OS_WIN
#include "nativeaudio.h"
#endif

#include <QApplication>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDebug>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QUrl>
#include <QVBoxLayout>

class LampPreview : public QWidget {
public:
    explicit LampPreview(QWidget *parent = nullptr) : QWidget(parent) {
        setMinimumSize(180, 180);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }
    void setLight(const QColor &color, int brightness, bool power) {
        m_color = color; m_brightness = brightness; m_power = power; update();
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QPointF center(width() / 2.0, height() / 2.0);
        const qreal radius = qMin(width(), height()) * 0.34;
        QColor color = m_power ? m_color : QColor(162, 165, 167);
        QRadialGradient glow(center, radius * 1.45);
        QColor halo = color; halo.setAlpha(m_power ? 90 + m_brightness * 7 : 18);
        glow.setColorAt(0, halo); glow.setColorAt(1, Qt::transparent);
        painter.setPen(Qt::NoPen); painter.setBrush(glow);
        painter.drawEllipse(center, radius * 1.45, radius * 1.45);
        QLinearGradient fill(center.x(), center.y() - radius, center.x(), center.y() + radius);
        fill.setColorAt(0, color.lighter(120)); fill.setColorAt(1, color.darker(115));
        painter.setBrush(fill); painter.drawEllipse(center, radius, radius);
        painter.setPen(QPen(QColor(255, 255, 255, 130), 2)); painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(center, radius - 6, radius - 6);
        QFont font = painter.font(); font.setPointSize(13); painter.setFont(font);
        painter.setPen(QColor(37, 42, 45));
        painter.drawText(QRectF(center.x() - radius, center.y() - 15, radius * 2, 30), Qt::AlignCenter,
                         m_power ? QStringLiteral("灯光预览") : QStringLiteral("已关灯"));
    }
private:
    QColor m_color = QColor(255, 160, 64);
    int m_brightness = 8;
    bool m_power = true;
};

namespace {
QSlider *createSlider(QWidget *parent, int maximum, int value) {
    auto slider = new QSlider(Qt::Horizontal, parent);
    slider->setRange(0, maximum); slider->setValue(value);
    slider->setMinimumWidth(180); return slider;
}
QWidget *createPanel(QVBoxLayout **layout, QWidget *parent) {
    auto panel = new QWidget(parent);
    *layout = new QVBoxLayout(panel); (*layout)->setContentsMargins(0, 0, 0, 0);
    return panel;
}
}

MainDialog::MainDialog(QWidget *parent)
    : QDialog(parent),
      m_settings(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("ilight.ini")), QSettings::IniFormat) {
    m_client = new BluetoothClient(this);
    createUi();
    m_adjustTimer.setSingleShot(true); m_adjustTimer.setInterval(140);
    connect(&m_adjustTimer, &QTimer::timeout, this, [this] {
        const bool includeWarm = m_pendingWarm; m_pendingWarm = false; applyLight(includeWarm);
    });
    connect(m_client, &BluetoothClient::deviceFound, this, [this](const QString &name, const QString &address) {
        for (int i = 0; i < m_devices->count(); ++i) {
            if (m_devices->item(i)->data(Qt::UserRole).toString() == address) {
                if (!name.isEmpty()) {
                    m_devices->item(i)->setText(QStringLiteral("%1\n%2").arg(name, address));
                    m_devices->item(i)->setData(Qt::UserRole + 1, name);
                }
                return;
            }
        }
        auto item = new QListWidgetItem(QStringLiteral("%1\n%2").arg(name.isEmpty() ? tr("未命名蓝牙设备") : name, address), m_devices);
        item->setData(Qt::UserRole, address); item->setData(Qt::UserRole + 1, name);
    });
    connect(m_client, &BluetoothClient::searchState, this, [this](bool active, const QString &message) {
        m_search->setText(active ? tr("停止搜索") : tr("搜索灯具"));
        m_searchStatus->setText(message); appendLog(message);
    });
    connect(m_client, &BluetoothClient::status, this, [this](const QString &text, bool ready) {
        m_ready = ready;
        m_connecting = !ready && (text.contains(QStringLiteral("正在")) || text.contains(QStringLiteral("读取灯具")));
        m_status->setText(text);
        m_confirmation->setText(ready ? tr("正在读取灯具状态…") : tr("连接并读取灯具状态后可调节灯光"));
        if (!ready) m_adjustTimer.stop();
        appendLog(text); updateControls();
    });
    connect(m_client, &BluetoothClient::feedback, this, &MainDialog::receiveFeedback);
    connect(m_client, &BluetoothClient::diagnostic, this, &MainDialog::appendLog);
    connect(m_client, &BluetoothClient::commandSent, this, [this] {
        m_confirmation->setText(tr("命令已发送 · 等待灯具反馈"));
    });
    connect(m_client, &BluetoothClient::trace, this, [this](const QString &direction, const QByteArray &bytes) {
        appendLog(QStringLiteral("%1  %2").arg(direction, QString::fromLatin1(bytes.left(256).toHex(' '))));
    });
    updateControls(); updatePreview();
}

MainDialog::~MainDialog() {
#ifdef Q_OS_WIN
    if (m_audio) m_audio->wait();
#endif
    m_settings.setValue(QStringLiteral("geometry"), saveGeometry());
    m_client->disconnectDevice();
}

void MainDialog::createUi() {
    setWindowTitle(tr("iLight Desktop")); resize(1000, 720); setMinimumSize(800, 620);
    auto rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(16, 16, 16, 16);
    auto bodyLayout = new QHBoxLayout;
    rootLayout->addLayout(bodyLayout, 1);
    auto devicePanel = new QGroupBox(tr("灯具连接"), this);
    devicePanel->setMaximumWidth(290);
    auto deviceLayout = new QVBoxLayout(devicePanel);
    deviceLayout->setContentsMargins(16, 16, 16, 16); deviceLayout->setSpacing(10);
    auto intro = new QLabel(tr("先断开手机与灯具的连接，再选择设备连接。\n系统配对失败时，也可尝试直接连接灯控。"), devicePanel);
    intro->setWordWrap(true); intro->setObjectName(QStringLiteral("hint")); deviceLayout->addWidget(intro);
    m_search = new QPushButton(tr("搜索灯具"), devicePanel); deviceLayout->addWidget(m_search);
    connect(m_search, &QPushButton::clicked, m_client, &BluetoothClient::search);
    m_searchStatus = new QLabel(tr("尚未搜索"), devicePanel);
    m_searchStatus->setWordWrap(true); deviceLayout->addWidget(m_searchStatus);
    m_devices = new QListWidget(devicePanel); m_devices->setMinimumWidth(240); deviceLayout->addWidget(m_devices, 1);
    connect(m_devices, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *item) {
        if (item) m_address->setText(item->data(Qt::UserRole).toString());
    });
    connect(m_devices, &QListWidget::itemDoubleClicked, this, [this] { connectSelected(); });
    deviceLayout->addWidget(new QLabel(tr("设备地址"), devicePanel));
    m_address = new QLineEdit(devicePanel); m_address->setPlaceholderText(QStringLiteral("C9:77:01:9C:8F:0D"));
    const QString savedAddress = m_settings.value(QStringLiteral("address")).toString().trimmed();
    m_address->setText(savedAddress.isEmpty() ? QStringLiteral("C9:77:01:9C:8F:0D") : savedAddress);
    deviceLayout->addWidget(m_address); connect(m_address, &QLineEdit::returnPressed, this, &MainDialog::connectSelected);
    auto connectionRow = new QHBoxLayout;
    m_connect = new QPushButton(tr("连接灯具"), devicePanel); m_disconnect = new QPushButton(tr("断开"), devicePanel);
    connectionRow->addWidget(m_connect); connectionRow->addWidget(m_disconnect); deviceLayout->addLayout(connectionRow);
    connect(m_connect, &QPushButton::clicked, this, &MainDialog::connectSelected);
    connect(m_disconnect, &QPushButton::clicked, this, &MainDialog::disconnectDevice);
    auto bluetoothSettings = new QPushButton(tr("打开 Windows 蓝牙设置"), devicePanel); deviceLayout->addWidget(bluetoothSettings);
    connect(bluetoothSettings, &QPushButton::clicked, this, [] { QDesktopServices::openUrl(QUrl(QStringLiteral("ms-settings:bluetooth"))); });
#ifdef Q_OS_WIN
    auto audioButton = new QPushButton(tr("连接音箱"), devicePanel); deviceLayout->addWidget(audioButton);
    auto audioStatus = new QLabel(tr("音箱状态尚未检查"), devicePanel); audioStatus->setWordWrap(true); deviceLayout->addWidget(audioStatus);
    connect(audioButton, &QPushButton::clicked, this, [this, audioButton, audioStatus] {
        if (m_audio) return;
        const QString address = m_address->text().trimmed().toUpper();
        if (!QRegularExpression(QStringLiteral("^[0-9A-F]{2}(:[0-9A-F]{2}){5}$")).match(address).hasMatch()) {
            audioStatus->setText(tr("音箱：请输入完整蓝牙地址")); return;
        }
        m_audio = new NativeAudio(address, quintptr(winId()), this);
        audioButton->setEnabled(false);
        connect(m_audio, &NativeAudio::status, this, [this, audioStatus](const QString &text) { audioStatus->setText(text); appendLog(text); });
        connect(m_audio, &NativeAudio::diagnostic, this, &MainDialog::appendLog);
        connect(m_audio, &QThread::finished, this, [this, audioButton] {
            m_audio->deleteLater(); m_audio = nullptr; audioButton->setEnabled(true);
        });
        m_audio->start();
    });
    auto soundSettings = new QPushButton(tr("打开声音设置"), devicePanel); deviceLayout->addWidget(soundSettings);
    connect(soundSettings, &QPushButton::clicked, this, [] { QDesktopServices::openUrl(QUrl(QStringLiteral("ms-settings:sound"))); });
#endif
    m_demoButton = new QPushButton(tr("界面演示"), devicePanel); m_demoButton->setCheckable(true);
    deviceLayout->addWidget(m_demoButton);
    connect(m_demoButton, &QPushButton::toggled, this, &MainDialog::setDemo);
    bodyLayout->addWidget(devicePanel);

    auto central = new QWidget(this); auto layout = new QVBoxLayout(central); layout->setContentsMargins(24, 18, 24, 20); layout->setSpacing(12);
    m_deviceName = new QLabel(tr("iLight 蓝牙灯控"), central); m_deviceName->setObjectName(QStringLiteral("heading")); layout->addWidget(m_deviceName);
    m_status = new QLabel(tr("未连接 · 选择灯具开始"), central); m_status->setWordWrap(true); layout->addWidget(m_status);
    m_controls = new QGroupBox(tr("灯光控制"), central); auto lightLayout = new QVBoxLayout(m_controls);
    auto topRow = new QHBoxLayout;
    m_preview = new LampPreview(m_controls); topRow->addWidget(m_preview, 1);
    auto powerLayout = new QVBoxLayout;
    auto on = new QPushButton(tr("开灯"), m_controls); auto off = new QPushButton(tr("关灯"), m_controls);
    on->setMinimumWidth(100); off->setMinimumWidth(100); powerLayout->addStretch(); powerLayout->addWidget(on); powerLayout->addWidget(off); powerLayout->addStretch(); topRow->addLayout(powerLayout);
    connect(on, &QPushButton::clicked, this, [this] { m_adjustTimer.stop(); m_pendingWarm = false; if (m_demo) { m_power = true; updatePreview(); } if (lampType() == 2 && isWhiteColor()) applyLight(false); else sendCommand(ilight::command(activeLampType(), 3, 1, {1})); });
    connect(off, &QPushButton::clicked, this, [this] { m_adjustTimer.stop(); m_pendingWarm = false; if (m_demo) { m_power = false; updatePreview(); } sendCommand(ilight::command(activeLampType(), 3, 1, {2})); });
    lightLayout->addLayout(topRow, 1);
    auto brightnessRow = new QHBoxLayout;
    brightnessRow->addWidget(new QLabel(tr("亮度"), m_controls));
    m_brightness = createSlider(m_controls, 16, 8); m_brightness->setMinimum(1);
    m_brightnessValue = new QSpinBox(m_controls); m_brightnessValue->setRange(1, 16); m_brightnessValue->setValue(8); m_brightnessValue->setSuffix(QStringLiteral(" / 16"));
    brightnessRow->addWidget(m_brightness, 1); brightnessRow->addWidget(m_brightnessValue); lightLayout->addLayout(brightnessRow);
    connect(m_brightnessValue, QOverload<int>::of(&QSpinBox::valueChanged), m_brightness, &QSlider::setValue);
    connect(m_brightness, &QSlider::valueChanged, this, [this](int value) {
        QSignalBlocker blocker(m_brightnessValue); m_brightnessValue->setValue(value);
        if (lampType() == 2) {
            const QColor current(m_red->value(), m_green->value(), m_blue->value());
            const QColor adjusted = QColor::fromHsvF(qMax(0.0, current.hsvHueF()), current.hsvSaturationF(), value / 16.0);
            const QSignalBlocker r(m_red), g(m_green), b(m_blue);
            m_red->setValue(adjusted.red()); m_green->setValue(adjusted.green()); m_blue->setValue(adjusted.blue());
        }
        updatePreview();
        if (m_ready || m_demo) m_adjustTimer.start();
    });

    QVBoxLayout *whiteLayout; m_whitePanel = createPanel(&whiteLayout, m_controls);
    m_warmValue = new QLabel(tr("冷暖白"), m_whitePanel); whiteLayout->addWidget(m_warmValue);
    auto warmRow = new QHBoxLayout;
    warmRow->addWidget(new QLabel(tr("暖白"), m_whitePanel)); m_warm = createSlider(m_whitePanel, 255, 128); warmRow->addWidget(m_warm, 1); warmRow->addWidget(new QLabel(tr("冷白"), m_whitePanel)); whiteLayout->addLayout(warmRow);
    connect(m_warm, &QSlider::valueChanged, this, [this] {
        updatePreview(); if (m_ready || m_demo) { m_pendingWarm = true; m_adjustTimer.start(); }
    });
    lightLayout->addWidget(m_whitePanel);

    QVBoxLayout *colorLayout; m_colorPanel = createPanel(&colorLayout, m_controls);
    auto paletteRow = new QHBoxLayout;
    const QList<QColor> colors {QColor(255, 70, 55), QColor(255, 170, 35), QColor(55, 220, 110), QColor(45, 140, 255), QColor(175, 70, 230), QColor(255, 255, 255)};
    for (const auto &color : colors) {
        auto swatch = new QPushButton(m_colorPanel); swatch->setFixedSize(36, 28);
        swatch->setToolTip(color == QColor(Qt::white) ? tr("白光") : color.name()); swatch->setStyleSheet(QStringLiteral("background:%1; border:1px solid #b8bcc0; border-radius:4px;").arg(color.name()));
        paletteRow->addWidget(swatch);
        connect(swatch, &QPushButton::clicked, this, [this, color] {
            const QSignalBlocker r(m_red), g(m_green), b(m_blue);
            m_red->setValue(color.red()); m_green->setValue(color.green()); m_blue->setValue(color.blue()); syncColorBrightness(); updatePreview(); applyLight(false);
        });
    }
    auto chooseColor = new QPushButton(tr("选择颜色…"), m_colorPanel); paletteRow->addStretch(); paletteRow->addWidget(chooseColor);
    connect(chooseColor, &QPushButton::clicked, this, [this] {
        const auto color = QColorDialog::getColor(QColor(m_red->value(), m_green->value(), m_blue->value()), this, tr("选择灯光颜色"));
        if (!color.isValid()) return;
        const QSignalBlocker r(m_red), g(m_green), b(m_blue);
        m_red->setValue(color.red()); m_green->setValue(color.green()); m_blue->setValue(color.blue()); syncColorBrightness(); updatePreview(); applyLight(false);
    });
    colorLayout->addLayout(paletteRow);
    m_rgbValue = new QLabel(m_colorPanel); colorLayout->addWidget(m_rgbValue);
    auto rgbForm = new QFormLayout;
    m_red = createSlider(m_colorPanel, 255, 255); m_green = createSlider(m_colorPanel, 255, 160); m_blue = createSlider(m_colorPanel, 255, 64);
    rgbForm->addRow(tr("红"), m_red); rgbForm->addRow(tr("绿"), m_green); rgbForm->addRow(tr("蓝"), m_blue); colorLayout->addLayout(rgbForm);
    for (auto slider : {m_red, m_green, m_blue}) connect(slider, &QSlider::valueChanged, this, [this] { syncColorBrightness(); updatePreview(); if (m_ready || m_demo) m_adjustTimer.start(); });
    m_effect = new QComboBox(m_colorPanel);
    // Match the original Android app; UI indexes are not protocol effect IDs.
    m_effect->addItem(tr("常亮"), 0);
    m_effect->addItem(tr("音乐律动"), 3);
    m_effect->addItem(tr("彩虹"), 4);
    m_effect->addItem(tr("呼吸"), 5);
    m_effect->addItem(tr("烛光"), 7);
    m_effect->setPlaceholderText(tr("未识别的灯效"));
    auto effectRow = new QFormLayout; effectRow->addRow(tr("灯效"), m_effect); colorLayout->addLayout(effectRow);
    connect(m_effect, QOverload<int>::of(&QComboBox::activated), this, [this](int index) {
        if (index < 0) return;
        m_adjustTimer.stop(); m_pendingWarm = false;
        const bool wasWhite = m_colorUsesWhite;
        m_colorUsesWhite = false;
        if (wasWhite) sendCommand(ilight::command(2, 3, 1, {1}));
        sendCommand(ilight::command(2, 3, 4, {m_effect->itemData(index).toInt()}));
    });
    lightLayout->addWidget(m_colorPanel); layout->addWidget(m_controls, 1);
    auto feedbackRow = new QHBoxLayout;
    m_confirmation = new QLabel(tr("连接并读取灯具状态后可调节灯光"), central); m_confirmation->setWordWrap(true); m_confirmation->setObjectName(QStringLiteral("hint")); feedbackRow->addWidget(m_confirmation, 1);
    m_refresh = new QPushButton(tr("读取状态"), central); feedbackRow->addWidget(m_refresh); layout->addLayout(feedbackRow);
    connect(m_refresh, &QPushButton::clicked, this, [this] {
        if (m_demo) m_confirmation->setText(tr("演示模式 · 没有灯具反馈"));
        else { m_confirmation->setText(tr("正在读取灯具状态…")); m_client->refresh(activeLampType()); }
    });
    bodyLayout->addWidget(central, 1);
    auto footer = new QHBoxLayout;
    auto showLog = new QPushButton(tr("通信日志"), this); showLog->setCheckable(true);
    footer->addWidget(showLog); footer->addStretch();
    footer->addWidget(new QLabel(tr("经典蓝牙 SPP · 本地灯控"), this));
    auto closeButton = new QPushButton(tr("关闭"), this); footer->addWidget(closeButton);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::reject);
    rootLayout->addLayout(footer);
    m_log = new QPlainTextEdit(this); m_log->setReadOnly(true); m_log->setMaximumBlockCount(300);
    m_log->setFixedHeight(130); rootLayout->addWidget(m_log); m_log->hide();
    connect(showLog, &QPushButton::toggled, m_log, &QWidget::setVisible);
    setStyleSheet(QStringLiteral(
        "QDialog { background:#f3f4f5; }"
        "QLabel#heading { font-size:21px; font-weight:600; color:#25303c; }"
        "QLabel#hint { color:#66717d; }"
        "QGroupBox { background:white; border:1px solid #d4d9de; border-radius:6px; margin-top:12px; padding-top:10px; }"
        "QGroupBox::title { subcontrol-origin:margin; left:12px; padding:0 5px; }"
        "QPushButton { min-height:24px; padding:3px 10px; }"
        "QLineEdit, QComboBox, QSpinBox { min-height:25px; }"
        "QListWidget { border:1px solid #d4d9de; background:white; }"
        "QListWidget::item { padding:8px; }"));
    if (m_settings.contains(QStringLiteral("geometry"))) restoreGeometry(m_settings.value(QStringLiteral("geometry")).toByteArray());
}

int MainDialog::lampType() const { return 2; }
int MainDialog::activeLampType() const { return m_colorUsesWhite ? 1 : lampType(); }
bool MainDialog::isWhiteColor() const {
    return m_red->value() > 0 && m_red->value() == m_green->value() && m_green->value() == m_blue->value();
}

void MainDialog::connectToAddress(const QString &address) {
    m_address->setText(address);
    connectSelected();
}

void MainDialog::searchDevices() { m_client->search(); }

void MainDialog::connectSelected() {
    const QString address = m_address->text().trimmed().toUpper();
    static const QRegularExpression pattern(QStringLiteral("^[0-9A-F]{2}(:[0-9A-F]{2}){5}$"));
    if (!pattern.match(address).hasMatch()) {
        QMessageBox::information(this, tr("设备地址"), tr("请输入完整蓝牙地址，例如 C9:77:01:9C:8F:0D。")); return;
    }
    if (m_demo) m_demoButton->setChecked(false);
    m_settings.setValue(QStringLiteral("address"), address); m_address->setText(address);
    QString name = address;
    if (auto item = m_devices->currentItem()) {
        if (item->data(Qt::UserRole).toString() == address && !item->data(Qt::UserRole + 1).toString().isEmpty()) name = item->data(Qt::UserRole + 1).toString();
    }
    m_deviceName->setText(name); m_coldWarmSupported.reset(); m_colorBrightnessSupported.reset(); m_pendingWarm = false;
    // This lamp's physical red/green channels are reversed (confirmed on hardware).
    m_swapRedGreen = address == QStringLiteral("C9:77:01:9C:8F:0D");
    // Original LampTypeManager selects the breathing ID by MAC prefix.
    m_effect->setItemData(3, address.startsWith(QStringLiteral("C9:7")) ? 5
        : address.startsWith(QStringLiteral("C9:8")) ? 11 : 1);
    m_colorUsesWhite = false;
    m_connectionStates = 0; m_connectionPoweredStates = 0;
    m_client->connectDevice(address, lampType());
}

void MainDialog::disconnectDevice() {
    m_adjustTimer.stop(); m_pendingWarm = false;
    if (m_demo) { m_demoButton->setChecked(false); return; }
    m_client->disconnectDevice();
}

void MainDialog::setDemo(bool enabled) {
    m_client->disconnectDevice(); m_client->stopSearch(); m_adjustTimer.stop();
    m_colorUsesWhite = false;
    m_demo = enabled; m_ready = false; m_connecting = false; m_pendingWarm = false; m_coldWarmSupported.reset(); m_colorBrightnessSupported.reset();
    m_status->setText(enabled ? tr("演示模式 · 操作不会发送至灯具") : tr("未连接 · 选择灯具开始"));
    m_deviceName->setText(enabled ? tr("界面演示") : tr("iLight 蓝牙灯控"));
    m_confirmation->setText(enabled ? tr("仅展示控制界面，没有实际灯具连接") : tr("连接并读取灯具状态后可调节灯光"));
    updateControls(); updatePreview();
}

void MainDialog::updateControls() {
    m_controls->setEnabled(m_ready || m_demo); m_refresh->setEnabled(m_ready || m_demo);
    m_colorPanel->setVisible(true);
    m_whitePanel->setVisible(m_colorUsesWhite && m_coldWarmSupported.value_or(false));
    m_warm->setEnabled(m_demo || !m_coldWarmSupported || *m_coldWarmSupported);
    m_brightness->setEnabled(true); m_brightnessValue->setEnabled(true);
    m_brightness->setToolTip(QString());
    m_disconnect->setEnabled(m_connecting || m_ready || m_demo);
    m_connect->setText(m_ready ? tr("重新连接") : tr("连接灯具"));
}

void MainDialog::updatePreview() {
    const int warm = m_warm->value();
    const QColor color = lampType() == 2 ? QColor(m_red->value(), m_green->value(), m_blue->value())
        : QColor(255, 210 + warm * 45 / 255, 150 + warm * 105 / 255);
    m_preview->setLight(color, m_brightness->value(), m_power);
    m_rgbValue->setText(QStringLiteral("RGB  %1 · %2 · %3").arg(m_red->value()).arg(m_green->value()).arg(m_blue->value()));
    m_warmValue->setText(m_coldWarmSupported && !*m_coldWarmSupported ? tr("此灯具不支持冷暖白")
        : tr("冷暖白  %1%").arg(qRound(warm * 100.0 / 255)));
}

void MainDialog::syncColorBrightness() {
    if (lampType() != 2) return;
    const int maximum = qMax(m_red->value(), qMax(m_green->value(), m_blue->value()));
    const int level = qBound(1, qRound(maximum * 16.0 / 255), 16);
    const QSignalBlocker slider(m_brightness), spin(m_brightnessValue);
    m_brightness->setValue(level); m_brightnessValue->setValue(level);
}

void MainDialog::applyLight(bool includeWarm) {
    m_adjustTimer.stop(); m_pendingWarm = false;
    if (includeWarm && m_colorUsesWhite && m_coldWarmSupported.value_or(false)) {
        sendCommand(ilight::command(1, 3, 4, {m_brightness->value(), m_warm->value()}));
        return;
    }
    if (lampType() == 2) {
        if (isWhiteColor()) {
            // Android LampManager routes neutral white to the dedicated white LEDs.
            m_colorUsesWhite = true;
            sendCommand(ilight::command(1, 3, 1, {1}));
            sendCommand(ilight::command(1, 3, 2, {m_brightness->value()}));
            return;
        }
        const bool wasWhite = m_colorUsesWhite;
        m_colorUsesWhite = false;
        if (wasWhite) sendCommand(ilight::command(2, 3, 1, {1}));
        const int wireRed = m_swapRedGreen ? m_green->value() : m_red->value();
        const int wireGreen = m_swapRedGreen ? m_red->value() : m_green->value();
        if (!m_colorBrightnessSupported.value_or(false) && !m_demo)
            sendCommand(ilight::command(2, 3, 3, {wireRed, wireGreen, m_blue->value()}));
        else sendCommand(ilight::command(2, 3, 12, {qMax(wireRed, qMax(wireGreen, m_blue->value())), wireRed, wireGreen, m_blue->value()}));
    }
    else if (includeWarm && (!m_coldWarmSupported || *m_coldWarmSupported)) sendCommand(ilight::command(1, 3, 4, {m_brightness->value(), m_warm->value()}));
    else sendCommand(ilight::command(1, 3, 2, {m_brightness->value()}));
}

void MainDialog::sendCommand(const QByteArray &command) {
    m_connectionStates = 3;
    if (m_demo) { m_confirmation->setText(tr("演示：已更新灯光设置")); return; }
    if (m_ready) {
        const int type = command.size() >= 3 ? ilight::u(command[2]) : activeLampType();
        m_client->control(command, type == 1 || type == 2 ? type : activeLampType());
    }
}

void MainDialog::receiveFeedback(const QByteArray &command) {
    auto feedback = ilight::parseFeedback(command);
    if (!feedback) return;
    // Track the powered channel without changing the user's visible control mode.
    if (m_connectionStates != 3 && feedback->power && (feedback->type == 1 || feedback->type == 2)) {
        const int bit = feedback->type == 1 ? 1 : 2;
        m_connectionStates |= bit;
        if (*feedback->power) m_connectionPoweredStates |= bit;
        else m_connectionPoweredStates &= ~bit;
        const int selectedBit = activeLampType() == 1 ? 1 : 2;
        if (*feedback->power && !(m_connectionPoweredStates & selectedBit)) {
            if (lampType() == 2) m_colorUsesWhite = feedback->type == 1;
        }
    }
    if (feedback->type != activeLampType()) return;
    if (feedback->coldWarmSupported) m_coldWarmSupported = *feedback->coldWarmSupported;
    if (feedback->colorBrightnessSupported) m_colorBrightnessSupported = *feedback->colorBrightnessSupported;
    if (feedback->power) m_power = *feedback->power;
    if (feedback->type == 1 && feedback->brightness && !m_brightness->isSliderDown() && !m_adjustTimer.isActive()) {
        const QSignalBlocker slider(m_brightness), spin(m_brightnessValue);
        m_brightness->setValue(*feedback->brightness); m_brightnessValue->setValue(*feedback->brightness);
        if (lampType() == 2 && m_colorUsesWhite) {
            const int component = qRound(*feedback->brightness * 255.0 / 16);
            const QSignalBlocker r(m_red), g(m_green), b(m_blue);
            m_red->setValue(component); m_green->setValue(component); m_blue->setValue(component);
            const QSignalBlocker effect(m_effect); m_effect->setCurrentIndex(m_effect->findData(0));
        }
    }
    if (feedback->warm && !m_warm->isSliderDown() && !m_adjustTimer.isActive()) { const QSignalBlocker blocker(m_warm); m_warm->setValue(*feedback->warm); }
    if (feedback->red && !m_brightness->isSliderDown() && !m_red->isSliderDown() && !m_green->isSliderDown() && !m_blue->isSliderDown() && !m_adjustTimer.isActive()) {
        const QSignalBlocker r(m_red), g(m_green), b(m_blue);
        QColor color(m_swapRedGreen ? *feedback->green : *feedback->red,
                     m_swapRedGreen ? *feedback->red : *feedback->green, *feedback->blue);
        if (m_colorBrightnessSupported.value_or(false) && feedback->brightness) {
            color = QColor::fromHsvF(qMax(0.0, color.hsvHueF()), color.hsvSaturationF(), *feedback->brightness / 255.0);
        }
        m_red->setValue(color.red()); m_green->setValue(color.green()); m_blue->setValue(color.blue());
        syncColorBrightness();
    }
    if (feedback->effect) {
        const int effect = *feedback->effect == 1 || *feedback->effect == 11
            ? m_effect->itemData(3).toInt() : *feedback->effect;
        const QSignalBlocker blocker(m_effect);
        m_effect->setCurrentIndex(m_effect->findData(effect));
    }
    m_confirmation->setText(feedback->power ? (*feedback->power ? tr("灯具反馈：已开灯") : tr("灯具反馈：已关灯")) : tr("已同步灯具设置"));
    updateControls(); updatePreview();
}

void MainDialog::appendLog(const QString &text) {
    m_log->appendPlainText(QStringLiteral("%1  %2").arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")), text));
    qInfo().noquote() << text;
}
