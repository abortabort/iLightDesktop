#include <winrt/base.h>
namespace winrt::impl {
template <typename Async>
auto wait_for(Async const &async, Windows::Foundation::TimeSpan const &timeout);
}
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Storage.Streams.h>
#include "nativeble.h"
#include <QElapsedTimer>
#include <QMutexLocker>
#include <QDebug>
#include <QCoreApplication>
#include <QMetaObject>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace {
using namespace winrt::Windows::Devices::Bluetooth;
using namespace winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace winrt::Windows::Storage::Streams;
template<typename Operation>
auto result(Operation operation, const std::atomic<bool> &stopping) {
    QElapsedTimer timer; timer.start();
    while (operation.Status() == winrt::Windows::Foundation::AsyncStatus::Started) {
        if (stopping || timer.elapsed() > 12000) {
            operation.Cancel(); throw std::runtime_error(stopping ? "cancelled" : "GATT operation timed out");
        }
        QThread::msleep(25);
    }
    return operation.GetResults();
}
winrt::guid uuid(uint32_t value) { return winrt::guid{value, 0, 0x1000, {0x80, 0, 0, 0x80, 0x5f, 0x9b, 0x34, 0xfb}}; }
QString communicationStatus(GattCommunicationStatus status) {
    switch (status) {
    case GattCommunicationStatus::Success: return QStringLiteral("Success");
    case GattCommunicationStatus::Unreachable: return QStringLiteral("Unreachable");
    case GattCommunicationStatus::ProtocolError: return QStringLiteral("ProtocolError");
    case GattCommunicationStatus::AccessDenied: return QStringLiteral("AccessDenied");
    }
    return QStringLiteral("Unknown");
}
template<typename Factory>
auto createOnUi(Factory factory, const std::atomic<bool> &stopping) {
    using Operation = decltype(factory());
    struct State {
        Operation operation{nullptr};
        std::exception_ptr error;
        std::atomic<bool> ready{false}, cancelled{false};
    };
    auto state = std::make_shared<State>();
    QMetaObject::invokeMethod(QCoreApplication::instance(), [state, factory] {
        if (state->cancelled) return;
        try { state->operation = factory(); } catch (...) { state->error = std::current_exception(); }
        state->ready = true;
    }, Qt::QueuedConnection);
    QElapsedTimer timer; timer.start();
    while (!state->ready) {
        if (stopping || timer.elapsed() > 3000) { state->cancelled = true; throw std::runtime_error("BLE device creation cancelled"); }
        QThread::msleep(10);
    }
    if (state->error) std::rethrow_exception(state->error);
    return result(state->operation, stopping);
}
}

bool NativeBle::enqueue(const QByteArray &bytes) {
    QMutexLocker lock(&m_mutex);
    if (m_stopping || m_queuedBytes + bytes.size() > 16384) return false;
    m_queue.enqueue(bytes); m_queuedBytes += bytes.size(); return true;
}

void NativeBle::run() {
    struct CallbackState { std::mutex mutex; NativeBle *owner = nullptr; };
    auto state = std::make_shared<CallbackState>(); state->owner = this;
    BluetoothLEDevice device{nullptr};
    GattSession session{nullptr};
    GattCharacteristic notify{nullptr}, writer{nullptr};
    winrt::event_token token{};
    bool subscribed = false, apartment = false;
    std::vector<GattDeviceService> services;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded); apartment = true;
        QString digits = m_address; digits.remove(':');
        const auto address = digits.toULongLong(nullptr, 16);
        using namespace winrt::Windows::Devices::Bluetooth::Advertisement;
        auto seen = std::make_shared<std::atomic<bool>>(false);
        auto advertisementCount = std::make_shared<std::atomic<int>>(0);
        auto targetRssi = std::make_shared<std::atomic<int>>(0);
        BluetoothLEAdvertisementWatcher watcher;
        watcher.ScanningMode(BluetoothLEScanningMode::Active);
        const auto advertisementToken = watcher.Received([seen, address, advertisementCount, targetRssi](auto const &, auto const &args) {
            ++*advertisementCount;
            if (args.BluetoothAddress() == address) *targetRssi = args.RawSignalStrengthInDBm();
            if (args.BluetoothAddress() == address && !seen->exchange(true)) {
                qInfo().noquote() << QStringLiteral("BLE target advertisement type=%1 addressType=%2 connectable=%3 rssi=%4")
                    .arg(int(args.AdvertisementType())).arg(int(args.BluetoothAddressType()))
                    .arg(args.IsConnectable()).arg(args.RawSignalStrengthInDBm());
            }
        });
        watcher.Start(); QElapsedTimer scan; scan.start();
        emit diagnostic(QStringLiteral("BLE scanner status=%1").arg(int(watcher.Status())));
        qint64 firstSeen = -1;
        while (!m_stopping && scan.elapsed() < 20000) {
            if (*seen && firstSeen < 0) firstSeen = scan.elapsed();
            if (firstSeen >= 0 && scan.elapsed() - firstSeen >= 5000) break;
            msleep(25);
        }
        emit diagnostic(QStringLiteral("BLE target advertisement found=%1 received=%2 rssi=%3 dBm").arg(bool(*seen)).arg(int(*advertisementCount)).arg(int(*targetRssi)));
        if (m_stopping) throw std::runtime_error("cancelled");
        emit diagnostic(QStringLiteral("BLE opening %1").arg(m_address));
        device = createOnUi([address] { return BluetoothLEDevice::FromBluetoothAddressAsync(address); }, m_stopping);
        if (!device) device = createOnUi([address] { return BluetoothLEDevice::FromBluetoothAddressAsync(address, BluetoothAddressType::Random); }, m_stopping);
        if (!device) device = createOnUi([address] { return BluetoothLEDevice::FromBluetoothAddressAsync(address, BluetoothAddressType::Public); }, m_stopping);
        watcher.Received(advertisementToken); watcher.Stop();
        if (!device) {
            emit diagnostic(QStringLiteral("BLE address lookup returned null; trying device enumeration"));
            using namespace winrt::Windows::Devices::Enumeration;
            auto properties = winrt::single_threaded_vector<winrt::hstring>({L"System.Devices.Aep.DeviceAddress"});
            struct EnumerationState { std::mutex mutex; winrt::hstring id; int count = 0; };
            auto enumeration = std::make_shared<EnumerationState>();
            auto deviceWatcher = DeviceInformation::CreateWatcher(L"(System.Devices.Aep.ProtocolId:=\"{bb7bb05e-5972-42b5-94fc-76eaa7084d49}\")", properties, DeviceInformationKind::AssociationEndpoint);
            const auto targetAddress = m_address;
            const auto added = deviceWatcher.Added([enumeration, targetAddress](auto const &, auto const &info) {
                std::lock_guard<std::mutex> lock(enumeration->mutex); ++enumeration->count;
                const auto key = L"System.Devices.Aep.DeviceAddress";
                if (!info.Properties().HasKey(key)) return;
                auto value = info.Properties().Lookup(key).try_as<winrt::Windows::Foundation::IPropertyValue>();
                if (!value || value.Type() != winrt::Windows::Foundation::PropertyType::String) return;
                const auto text = value.GetString();
                if (QString::fromWCharArray(text.c_str()).compare(targetAddress, Qt::CaseInsensitive) == 0) enumeration->id = info.Id();
            });
            const auto updated = deviceWatcher.Updated([](auto const &, auto const &) {});
            const auto removed = deviceWatcher.Removed([](auto const &, auto const &) {});
            deviceWatcher.Start(); QElapsedTimer enumerationTimer; enumerationTimer.start();
            winrt::hstring id;
            while (!m_stopping && enumerationTimer.elapsed() < 12000) {
                { std::lock_guard<std::mutex> lock(enumeration->mutex); id = enumeration->id; }
                if (!id.empty()) break;
                msleep(25);
            }
            deviceWatcher.Stop(); deviceWatcher.Added(added); deviceWatcher.Updated(updated); deviceWatcher.Removed(removed);
            { std::lock_guard<std::mutex> lock(enumeration->mutex); emit diagnostic(QStringLiteral("BLE device watcher count=%1 matched=%2").arg(enumeration->count).arg(!id.empty())); }
            if (!id.empty()) {
                emit diagnostic(QStringLiteral("BLE matching device enumerated"));
                device = createOnUi([id] { return BluetoothLEDevice::FromIdAsync(id); }, m_stopping);
            }
        }
        if (!device) throw std::runtime_error("Windows could not create the BLE device object");
        m_hasDevice = true;
        emit diagnostic(QStringLiteral("BLE device created addressType=%1 connection=%2 paired=%3")
            .arg(int(device.BluetoothAddressType())).arg(int(device.ConnectionStatus())).arg(device.DeviceInformation().Pairing().IsPaired()));
        session = result(GattSession::FromDeviceIdAsync(device.BluetoothDeviceId()), m_stopping);
        if (!session) throw std::runtime_error("Windows could not create GATT session");
        emit diagnostic(QStringLiteral("BLE session canMaintain=%1 status=%2").arg(session.CanMaintainConnection()).arg(int(session.SessionStatus())));
        if (session.CanMaintainConnection()) {
            session.MaintainConnection(true);
            emit diagnostic(QStringLiteral("BLE waiting for physical connection (up to 15 seconds)"));
            QElapsedTimer connectionTimer; connectionTimer.start();
            while (!m_stopping && connectionTimer.elapsed() < 15000 && session.SessionStatus() != GattSessionStatus::Active) msleep(50);
        }
        if (m_stopping) throw std::runtime_error("cancelled");
        emit diagnostic(QStringLiteral("BLE physical connection=%1 session=%2 mtu=%3")
            .arg(int(device.ConnectionStatus())).arg(int(session.SessionStatus())).arg(session.MaxPduSize()));
        GattDeviceServicesResult discovered{nullptr};
        for (int attempt = 1; attempt <= 3 && !m_stopping; ++attempt) {
            emit diagnostic(QStringLiteral("BLE querying services attempt=%1").arg(attempt));
            discovered = result(device.GetGattServicesAsync(BluetoothCacheMode::Uncached), m_stopping);
            emit diagnostic(QStringLiteral("BLE services status=%1 (%2) count=%3")
                .arg(int(discovered.Status())).arg(communicationStatus(discovered.Status())).arg(discovered.Services().Size()));
            if (discovered.Status() != GattCommunicationStatus::Unreachable || attempt == 3) break;
            for (int i = 0; i < 20 && !m_stopping; ++i) msleep(50);
        }
        if (m_stopping) throw std::runtime_error("cancelled");
        if (discovered.Status() != GattCommunicationStatus::Success) {
            const auto code = discovered.ProtocolError();
            if (code) emit diagnostic(QStringLiteral("BLE ATT protocol error=0x%1").arg(code.Value(), 2, 16, QLatin1Char('0')));
            throw std::runtime_error(discovered.Status() == GattCommunicationStatus::Unreachable
                ? "GATT device unreachable after connection wait and 3 discovery attempts"
                : "GATT service discovery rejected");
        }
        for (auto const &service : discovered.Services()) {
            services.push_back(service);
            emit diagnostic(QStringLiteral("BLE service %1").arg(QString::fromWCharArray(winrt::to_hstring(service.Uuid()).c_str())));
            const bool notification = service.Uuid() == uuid(0x6666);
            const bool writing = service.Uuid() == uuid(0x7777);
            if (!notification && !writing) continue;
            auto chars = result(service.GetCharacteristicsForUuidAsync(uuid(notification ? 0x8888 : 0x8877), BluetoothCacheMode::Uncached), m_stopping);
            if (chars.Status() != GattCommunicationStatus::Success || chars.Characteristics().Size() == 0) continue;
            if (notification) notify = chars.Characteristics().GetAt(0);
            else writer = chars.Characteristics().GetAt(0);
        }
        if (!notify || !writer) throw std::runtime_error("iLight GATT characteristics 8888/8877 missing");
        token = notify.ValueChanged([state](auto const &, auto const &args) {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->owner) return;
            auto reader = DataReader::FromBuffer(args.CharacteristicValue());
            QByteArray bytes(int(reader.UnconsumedBufferLength()), '\0');
            reader.ReadBytes(winrt::array_view<uint8_t>(reinterpret_cast<uint8_t *>(bytes.data()), reinterpret_cast<uint8_t *>(bytes.data()) + bytes.size()));
            emit state->owner->received(bytes);
        });
        subscribed = true;
        auto status = result(notify.WriteClientCharacteristicConfigurationDescriptorAsync(GattClientCharacteristicConfigurationDescriptorValue::Notify), m_stopping);
        if (status != GattCommunicationStatus::Success) throw std::runtime_error("GATT notification subscription failed");
        emit diagnostic(QStringLiteral("BLE notification enabled; write characteristic properties=%1").arg(int(writer.CharacteristicProperties())));
        emit diagnostic(QStringLiteral("BLE MTU after discovery=%1").arg(session.MaxPduSize()));
        const auto option = (uint32_t(writer.CharacteristicProperties()) & uint32_t(GattCharacteristicProperties::WriteWithoutResponse))
            ? GattWriteOption::WriteWithoutResponse : GattWriteOption::WriteWithResponse;
        // Original BluzDeviceBle sends this channel initialization before framed commands.
        DataWriter initialization;
        const std::array<uint8_t, 8> preamble{{48, 49, 50, 51, 52, 53, 54, 55}};
        initialization.WriteBytes(preamble);
        if (result(writer.WriteValueAsync(initialization.DetachBuffer(), option), m_stopping) != GattCommunicationStatus::Success)
            throw std::runtime_error("GATT channel initialization failed");
        emit diagnostic(QStringLiteral("BLE channel initialized: 3031323334353637"));
        msleep(500); if (m_stopping) throw std::runtime_error("cancelled");
        emit connected();
        while (!m_stopping) {
            if (device.ConnectionStatus() == BluetoothConnectionStatus::Disconnected)
                throw std::runtime_error("BLE device disconnected");
            QByteArray packet;
            { QMutexLocker lock(&m_mutex); if (!m_queue.isEmpty()) { packet = m_queue.dequeue(); m_queuedBytes -= packet.size(); } }
            if (packet.isEmpty()) { msleep(20); continue; }
            const int chunkSize = qMax(20, int(session.MaxPduSize()) - 3);
            for (int offset = 0; offset < packet.size() && !m_stopping; offset += chunkSize) {
                const auto chunk = packet.mid(offset, chunkSize);
                DataWriter buffer;
                const auto begin = reinterpret_cast<const uint8_t *>(chunk.constData());
                buffer.WriteBytes(winrt::array_view<const uint8_t>(begin, begin + chunk.size()));
                const auto writeStatus = result(writer.WriteValueAsync(buffer.DetachBuffer(), option), m_stopping);
                emit diagnostic(QStringLiteral("BLE WRITE offset=%1 length=%2 mtu=%3 option=%4 status=%5 bytes=%6")
                    .arg(offset).arg(chunk.size()).arg(session.MaxPduSize()).arg(int(option))
                    .arg(communicationStatus(writeStatus)).arg(QString::fromLatin1(chunk.toHex(' '))));
                if (writeStatus != GattCommunicationStatus::Success)
                    throw std::runtime_error("GATT write failed");
                msleep(50);
            }
        }
    } catch (const winrt::hresult_error &error) {
        if (!m_stopping) emit failed(QStringLiteral("BLE: %1 (0x%2)").arg(QString::fromWCharArray(error.message().c_str())).arg(uint32_t(error.code().value), 8, 16, QLatin1Char('0')));
    } catch (const std::exception &error) {
        if (!m_stopping) emit failed(QStringLiteral("BLE: %1").arg(QString::fromUtf8(error.what())));
    }
    { std::lock_guard<std::mutex> lock(state->mutex); state->owner = nullptr; }
    if (subscribed) { try { notify.ValueChanged(token); } catch (...) {} }
    for (auto const &service : services) { try { service.Close(); } catch (...) {} }
    if (session) { try { session.MaintainConnection(false); session.Close(); } catch (...) {} }
    if (device) { try { device.Close(); } catch (...) {} }
    if (apartment) winrt::uninit_apartment();
}
