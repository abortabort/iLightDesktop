#include <winrt/base.h>
// SDK 19041 uses this template before declaring it under /permissive-.
namespace winrt::impl {
template <typename Async>
auto wait_for(Async const &async, Windows::Foundation::TimeSpan const &timeout);
}
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include "bleadvertisements.h"
#include <QDebug>
#include <QElapsedTimer>
#include <QThread>
#include <memory>
#include <mutex>

void inspectBleAdvertisements(const QString &address, const std::atomic<bool> &stopping) {
    namespace advertisement = winrt::Windows::Devices::Bluetooth::Advertisement;
    struct State { std::mutex mutex; bool active = true; bool found = false; };
    auto state = std::make_shared<State>();
    QString digits = address; digits.remove(':');
    const auto target = digits.toULongLong(nullptr, 16);
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        advertisement::BluetoothLEAdvertisementWatcher watcher;
        watcher.ScanningMode(advertisement::BluetoothLEScanningMode::Active);
        const auto token = watcher.Received([state, target, address](auto const &, auto const &args) {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->active || args.BluetoothAddress() != target || state->found) return;
            state->found = true;
            const auto name = args.Advertisement().LocalName();
            qInfo().noquote() << QStringLiteral("BLE ADVERTISEMENT target=%1 name=%2 rssi=%3; address is present over BLE")
                .arg(address, QString::fromWCharArray(name.c_str())).arg(args.RawSignalStrengthInDBm());
            for (auto const &uuid : args.Advertisement().ServiceUuids())
                qInfo().noquote() << "BLE advertised service:" << QString::fromWCharArray(winrt::to_hstring(uuid).c_str());
        });
        watcher.Start(); QElapsedTimer timer; timer.start();
        while (!stopping && timer.elapsed() < 5000) QThread::msleep(50);
        { std::lock_guard<std::mutex> lock(state->mutex); state->active = false; }
        watcher.Received(token); watcher.Stop();
        qInfo().noquote() << QStringLiteral("BLE advertisement inspection target=%1 found=%2").arg(address).arg(state->found);
        winrt::uninit_apartment();
    } catch (const winrt::hresult_error &error) {
        { std::lock_guard<std::mutex> lock(state->mutex); state->active = false; }
        qWarning().noquote() << "BLE advertisement inspection error:" << QString::fromWCharArray(error.message().c_str());
    }
}
