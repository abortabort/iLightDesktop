#ifndef MAINDIALOG_H
#define MAINDIALOG_H

#include <QDialog>
#include <QColor>
#include <QSettings>
#include <QTimer>
#include <optional>

class BluetoothClient;
class QComboBox;
class QLabel;
class QListWidget;
class QLineEdit;
class QSlider;
class QSpinBox;
class QPushButton;
class QPlainTextEdit;
class LampPreview;
class NativeAudio;

class MainDialog : public QDialog {
    Q_OBJECT
public:
    explicit MainDialog(QWidget *parent = nullptr);
    ~MainDialog() override;
    void connectToAddress(const QString &address);
    void searchDevices();
private:
    void createUi();
    void connectSelected();
    void disconnectDevice();
    void setDemo(bool enabled);
    void updateControls();
    void updatePreview();
    void syncColorBrightness();
    void applyLight(bool includeWarm);
    void sendCommand(const QByteArray &command);
    void receiveFeedback(const QByteArray &command);
    void appendLog(const QString &text);
    int lampType() const;
    int activeLampType() const;
    bool isWhiteColor() const;

    BluetoothClient *m_client = nullptr;
    NativeAudio *m_audio = nullptr;
    QSettings m_settings;
    QTimer m_adjustTimer;
    QListWidget *m_devices = nullptr;
    QLineEdit *m_address = nullptr;
    QComboBox *m_effect = nullptr;
    QWidget *m_controls = nullptr;
    QWidget *m_colorPanel = nullptr;
    QWidget *m_whitePanel = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_searchStatus = nullptr;
    QLabel *m_deviceName = nullptr;
    QLabel *m_confirmation = nullptr;
    QLabel *m_rgbValue = nullptr;
    QLabel *m_warmValue = nullptr;
    LampPreview *m_preview = nullptr;
    QPushButton *m_search = nullptr;
    QPushButton *m_connect = nullptr;
    QPushButton *m_disconnect = nullptr;
    QPushButton *m_refresh = nullptr;
    QSlider *m_brightness = nullptr;
    QSpinBox *m_brightnessValue = nullptr;
    QSlider *m_warm = nullptr;
    QSlider *m_red = nullptr;
    QSlider *m_green = nullptr;
    QSlider *m_blue = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QPushButton *m_demoButton = nullptr;
    bool m_ready = false;
    bool m_demo = false;
    bool m_connecting = false;
    bool m_pendingWarm = false;
    bool m_power = true;
    bool m_swapRedGreen = false;
    bool m_colorUsesWhite = false;
    int m_connectionStates = 3;
    int m_connectionPoweredStates = 0;
    std::optional<bool> m_coldWarmSupported;
    std::optional<bool> m_colorBrightnessSupported;
};

#endif
