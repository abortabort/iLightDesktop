#include <winrt/base.h>
namespace winrt::impl {
template <typename Async>
auto wait_for(Async const &async, Windows::Foundation::TimeSpan const &timeout);
}
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include "nativeblescanner.h"
#include <QElapsedTimer>
#include <QHash>
#include <QStringList>
#include <memory>
#include <mutex>

void NativeBleScanner::run() {
    using namespace winrt::Windows::Devices::Bluetooth::Advertisement;
    struct State { std::mutex mutex; NativeBleScanner *owner = nullptr; QHash<QString, QString> devices; int packets = 0; };
    auto state = std::make_shared<State>(); state->owner = this;
    BluetoothLEAdvertisementWatcher watcher{nullptr};
    winrt::event_token received{};
    bool apartment = false, registered = false;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded); apartment = true;
        watcher = BluetoothLEAdvertisementWatcher();
        watcher.ScanningMode(BluetoothLEScanningMode::Active);
        received = watcher.Received([state](auto const &, auto const &args) {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->owner) return;
            ++state->packets;
            const auto advertisement = args.Advertisement();
            const auto localName = advertisement.LocalName();
            QString name = QString::fromWCharArray(localName.c_str());
            const winrt::guid lampService{0x6666, 0, 0x1000, {0x80, 0, 0, 0x80, 0x5f, 0x9b, 0x34, 0xfb}};
            bool lamp = name.contains(QStringLiteral("ilight"), Qt::CaseInsensitive);
            for (auto const &service : advertisement.ServiceUuids()) if (service == lampService) lamp = true;
            const auto digits = QStringLiteral("%1").arg(args.BluetoothAddress(), 12, 16, QLatin1Char('0')).toUpper();
            QStringList parts; for (int i = 0; i < digits.size(); i += 2) parts.append(digits.mid(i, 2));
            const auto address = parts.join(QLatin1Char(':'));
            if (!lamp && !state->devices.contains(address)) return;
            if (name.isEmpty()) name = state->devices.value(address, QStringLiteral("iLight (BLE)"));
            if (state->devices.value(address) == name) return;
            state->devices.insert(address, name);
            emit state->owner->deviceFound(name, address);
            emit state->owner->diagnostic(QStringLiteral("BLE DISCOVER name=%1 address=%2 rssi=%3 dBm").arg(name, address).arg(args.RawSignalStrengthInDBm()));
        }); registered = true;
        watcher.Start(); emit diagnostic(QStringLiteral("BLE discovery started (20 seconds)"));
        QElapsedTimer timer; timer.start();
        while (!m_stopping && timer.elapsed() < 20000 && watcher.Status() != BluetoothLEAdvertisementWatcherStatus::Aborted) msleep(50);
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            emit diagnostic(QStringLiteral("BLE discovery finished status=%1 advertisements=%2 lamps=%3")
                .arg(int(watcher.Status())).arg(state->packets).arg(state->devices.size()));
        }
    } catch (const winrt::hresult_error &error) {
        if (!m_stopping) emit diagnostic(QStringLiteral("BLE discovery error: %1").arg(QString::fromWCharArray(error.message().c_str())));
    }
    { std::lock_guard<std::mutex> lock(state->mutex); state->owner = nullptr; }
    if (registered) { try { watcher.Received(received); watcher.Stop(); } catch (...) {} }
    if (apartment) winrt::uninit_apartment();
}
