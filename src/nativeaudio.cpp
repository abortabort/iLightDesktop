#include <winsock2.h>
#include <windows.h>
#include <bthsdpdef.h>
#include <bluetoothapis.h>
#include "nativeaudio.h"
#include <vector>

namespace {
QString errorText(DWORD code) {
    wchar_t *message = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
        | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, reinterpret_cast<wchar_t *>(&message), 0, nullptr);
    const QString text = length ? QString::fromWCharArray(message, int(length)).trimmed() : QStringLiteral("Windows error");
    if (message) LocalFree(message);
    return QStringLiteral("%1 (%2)").arg(text).arg(code);
}
}

NativeAudio::NativeAudio(const QString &address, quintptr window, QObject *parent)
    : QThread(parent), m_address(address), m_window(window) {}

void NativeAudio::run() {
    QString digits = m_address; digits.remove(':');
    bool valid = false;
    const ULONGLONG address = digits.toULongLong(&valid, 16);
    if (!valid || digits.size() != 12) { emit status(QStringLiteral("音箱：蓝牙地址无效")); return; }
    emit status(QStringLiteral("音箱：正在查找设备…"));
    BLUETOOTH_DEVICE_INFO device {}; device.dwSize = sizeof(device); device.Address.ullLong = address;
    DWORD result = BluetoothGetDeviceInfo(nullptr, &device);
    if (result != ERROR_SUCCESS) {
        BLUETOOTH_DEVICE_SEARCH_PARAMS search {}; search.dwSize = sizeof(search);
        search.fReturnAuthenticated = TRUE; search.fReturnRemembered = TRUE;
        search.fReturnUnknown = TRUE; search.fReturnConnected = TRUE;
        search.fIssueInquiry = TRUE; search.cTimeoutMultiplier = 4;
        BLUETOOTH_DEVICE_INFO candidate {}; candidate.dwSize = sizeof(candidate);
        const HBLUETOOTH_DEVICE_FIND finder = BluetoothFindFirstDevice(&search, &candidate);
        bool found = false;
        if (finder) {
            do {
                if (candidate.Address.ullLong == address) { device = candidate; found = true; break; }
            } while (BluetoothFindNextDevice(finder, &candidate));
            BluetoothFindDeviceClose(finder);
        }
        if (!found) {
            emit diagnostic(QStringLiteral("AUDIO device not found address=%1 initialError=%2").arg(m_address).arg(result));
            emit status(QStringLiteral("音箱：未找到设备，请确认灯具通电且手机已断开")); return;
        }
    }
    emit diagnostic(QStringLiteral("AUDIO address=%1 paired=%2 remembered=%3 connected=%4")
        .arg(m_address).arg(device.fAuthenticated).arg(device.fRemembered).arg(device.fConnected));
    if (!device.fAuthenticated) {
        emit status(QStringLiteral("音箱：正在配对，请完成 Windows 配对提示…"));
        result = BluetoothAuthenticateDeviceEx(reinterpret_cast<HWND>(m_window), nullptr, &device,
            nullptr, MITMProtectionNotRequired);
        emit diagnostic(QStringLiteral("AUDIO pairing result=%1").arg(result));
        if (result != ERROR_SUCCESS && result != ERROR_NO_MORE_ITEMS) {
            emit status(QStringLiteral("音箱配对失败：%1").arg(errorText(result))); return;
        }
    }
    // Enable the remote Audio Sink service so Windows can install its audio endpoint.
    // Service installation success does not imply an active audio connection.
    const GUID sink {0x0000110b, 0x0000, 0x1000, {0x80,0x00,0x00,0x80,0x5f,0x9b,0x34,0xfb}};
    emit status(QStringLiteral("音箱：正在启用音频服务…"));
    result = BluetoothSetServiceState(nullptr, &device, &sink, BLUETOOTH_SERVICE_ENABLE);
    emit diagnostic(QStringLiteral("AUDIO enable AudioSink result=%1").arg(result));
    if (result == DWORD(E_INVALIDARG)) {
        DWORD count = 0;
        DWORD enumerated = BluetoothEnumerateInstalledServices(nullptr, &device, &count, nullptr);
        if ((enumerated == ERROR_SUCCESS || enumerated == ERROR_MORE_DATA) && count > 0 && count <= 256) {
            std::vector<GUID> services(count);
            enumerated = BluetoothEnumerateInstalledServices(nullptr, &device, &count, services.data());
            if (enumerated == ERROR_SUCCESS) {
                for (DWORD i = 0; i < count; ++i) if (IsEqualGUID(services[i], sink)) { result = ERROR_SUCCESS; break; }
            }
        }
    }
    if (result != ERROR_SUCCESS) {
        emit status(QStringLiteral("音箱音频服务启用失败：%1").arg(errorText(result))); return;
    }
    emit status(QStringLiteral("音箱：音频服务已启用，请在声音输出中选择 iLight"));
}
