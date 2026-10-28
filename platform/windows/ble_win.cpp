/* Windows HID over GATT adapter. GATT layout follows src/ble_hid.c; reports
 * come from bridge.c (scrcpy keyboard / absolute tablet), not the prototype.
 * SPDX-License-Identifier: Apache-2.0 */
#include "ble_win.h"
#include "ble_win_queue.h"
#include "ble_win_wait.h"
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Storage.Streams.h>
extern "C" {
#include "bridge.h"
}
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

using namespace winrt;
using namespace Windows::Devices::Bluetooth;
using namespace Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace Windows::Storage::Streams;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {
struct State {
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> stopping{false};
    bool ready = false, failed = false, closing = false, in_flight = false, worker_done = false;
    bool suspended = false;
    uint64_t peer_address = 0, epoch = 0;
    std::string waiting;
    otg_ble_win_detail::Queue queue;
    std::array<uint8_t, 8> keyboard{}, pointer{}, last_mouse{};
    uint8_t leds = 0;
    std::vector<uint8_t> report_map;
};

static void log(const char *level, const std::string& message)
{
    static const auto started = Clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
    std::fprintf(stderr, "%s: Windows BLE [%lldms] %s\n", level, static_cast<long long>(elapsed), message.c_str());
}
static std::string describe(const hresult_error& e)
{
    return "HRESULT=" + std::to_string(e.code().value) + " " + to_string(e.message());
}
static std::string address_text(uint64_t address)
{
    char text[18];
    std::snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X",
        static_cast<unsigned>((address >> 40) & 0xff), static_cast<unsigned>((address >> 32) & 0xff),
        static_cast<unsigned>((address >> 24) & 0xff), static_cast<unsigned>((address >> 16) & 0xff),
        static_cast<unsigned>((address >> 8) & 0xff), static_cast<unsigned>(address & 0xff));
    return text;
}
static guid uuid(uint16_t id)
{
    return {id, 0x0000, 0x1000, {0x80,0x00,0x00,0x80,0x5f,0x9b,0x34,0xfb}};
}
static std::string session_text(const GattSession& session)
{
    try {
        return session.SessionStatus() == GattSessionStatus::Active ? "active" : "closed";
    } catch (...) { return "unavailable"; }
}
template<class Bytes> static IBuffer buffer(const Bytes& bytes)
{
    DataWriter writer;
    writer.WriteBytes(bytes);
    return writer.DetachBuffer();
}

/* Only the worker waits for WinRT operations. Cancellation/timeouts never
 * block Deskflow. Poll Status rather than reassigning the Completed delegate. */
template<class Operation> static auto wait(const std::shared_ptr<State>& s,
    const Operation& operation, const char *name, std::chrono::milliseconds timeout = 5s,
    std::optional<uint64_t> epoch = {})
{
    using namespace otg_ble_win_detail;
    const auto deadline = Clock::now() + timeout;
    return wait_operation(name,
        [&] { return operation.Status() == Windows::Foundation::AsyncStatus::Started; },
        [&] { operation.Cancel(); }, [&] { return operation.GetResults(); },
        [&](bool pending) {
            if (s->stopping) return WaitReason::stopping;
            if (epoch) {
                {
                    std::lock_guard<std::mutex> lock(s->mutex);
                    if (*epoch != s->epoch) return WaitReason::session_changed;
                }
                if (operation.Status() == Windows::Foundation::AsyncStatus::Canceled)
                    return WaitReason::session_changed;
            }
            return pending && Clock::now() >= deadline ? WaitReason::timed_out : WaitReason::none;
        }, [] { std::this_thread::sleep_for(1ms); });
}

/* A new readiness epoch discards unsent input and its read-back state. */
static void invalidate(const std::shared_ptr<State>& s, const char *reason, bool force = false)
{
    bool changed;
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        changed = s->waiting != reason;
        s->waiting = reason;
        if (s->ready || force) {
            s->ready = false;
            ++s->epoch;
            s->queue.clear();
            s->keyboard = {};
            s->pointer[0] = s->pointer[1] = 0;
            s->pointer[6] = s->pointer[7] = 0;
            s->last_mouse = s->pointer;
        }
    }
    s->cv.notify_all();
    if (changed) log("INFO", std::string("waiting: ") + reason);
}

static void fail(const std::shared_ptr<State>& s, const std::string& reason)
{
    invalidate(s, "transport failure", true);
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        s->failed = true;
    }
    s->cv.notify_all();
    log("ERROR", reason);
}

static Windows::Foundation::IAsyncOperation<bool> authorized(GattSession session,
    std::shared_ptr<State> s)
{
    auto device = co_await BluetoothLEDevice::FromIdAsync(session.DeviceId().Id());
    co_return device && device.BluetoothAddress() == s->peer_address &&
        device.DeviceInformation().Pairing().IsPaired() &&
        device.ConnectionStatus() == BluetoothConnectionStatus::Connected;
}

static fire_and_forget read_report(GattReadRequestedEventArgs args,
    std::shared_ptr<State> s, unsigned kind)
{
    auto deferral = args.GetDeferral();
    try {
        auto request = co_await args.GetRequestAsync();
        if (request) {
            if (!(co_await authorized(args.Session(), s))) {
                log("WARN", "report read rejected by identity/pairing gate: kind=" + std::to_string(kind) +
                    " DeviceId=" + to_string(args.Session().DeviceId().Id()));
                request.RespondWithProtocolError(0x08); // insufficient authorization
                deferral.Complete();
                co_return;
            }
            IBuffer value{nullptr};
            {
                std::lock_guard<std::mutex> lock(s->mutex);
                value = kind == 0 ? buffer(s->keyboard) : kind == 1 ? buffer(s->pointer) :
                    buffer(std::array<uint8_t, 1>{s->leds});
            }
            request.RespondWithValue(value);
        }
    } catch (const hresult_error& e) { log("WARN", "read: " + describe(e)); }
    deferral.Complete();
}

static fire_and_forget write_value(GattWriteRequestedEventArgs args,
    std::shared_ptr<State> s, bool control)
{
    auto deferral = args.GetDeferral();
    try {
        auto request = co_await args.GetRequestAsync();
        if (request) {
            uint8_t error = 0;
            if (!(co_await authorized(args.Session(), s))) {
                log("WARN", std::string(control ? "control" : "LED") +
                    " write rejected by identity/pairing gate: DeviceId=" + to_string(args.Session().DeviceId().Id()));
                error = 0x08;
            }
            else if (request.Offset() || request.Value().Length() != 1) error = 0x0d;
            else {
                auto value = DataReader::FromBuffer(request.Value()).ReadByte();
                if (control && value > 1) error = 0x13;
                else {
                    {
                        std::lock_guard<std::mutex> lock(s->mutex);
                        if (control) s->suspended = value == 0;
                        else s->leds = value;
                    }
                    if (control) invalidate(s, value == 0 ? "HID suspended" : "HID resume", true);
                }
            }
            if (request.Option() == GattWriteOption::WriteWithResponse) {
                if (error) request.RespondWithProtocolError(error);
                else request.Respond();
            }
        }
    } catch (const hresult_error& e) { log("WARN", "write: " + describe(e)); }
    deferral.Complete();
}

class Service {
    std::shared_ptr<State> s;
    GattServiceProvider provider{nullptr};
    GattLocalCharacteristic keyboard{nullptr}, pointer{nullptr};
    BluetoothLEDevice peer{nullptr};
    GattSubscribedClient key_client{nullptr}, pointer_client{nullptr};
    BluetoothLEDevice::ConnectionStatusChanged_revoker connection_changed;
    Windows::Foundation::IAsyncOperation<GattClientNotificationResult> notification{nullptr};
    Clock::time_point notification_started{};
    uint64_t sent_keyboard = 0, sent_pointer = 0;
    hstring peer_id;
    Clock::time_point pairing_refresh{};
    bool paired = false;

    GattLocalCharacteristic characteristic(uint16_t id, GattCharacteristicProperties properties,
        const std::vector<uint8_t>& value = {})
    {
        GattLocalCharacteristicParameters parameters;
        parameters.CharacteristicProperties(properties);
        parameters.ReadProtectionLevel(GattProtectionLevel::EncryptionRequired);
        parameters.WriteProtectionLevel(GattProtectionLevel::EncryptionRequired);
        if (!value.empty()) parameters.StaticValue(buffer(value));
        auto result = wait(s, provider.Service().CreateCharacteristicAsync(uuid(id), parameters), "create characteristic");
        if (result.Error() != BluetoothError::Success)
            throw std::runtime_error("create characteristic " + std::to_string(id) +
                ": BluetoothError=" + std::to_string(static_cast<int>(result.Error())));
        return result.Characteristic();
    }

    void reference(const GattLocalCharacteristic& c, uint8_t id, uint8_t type)
    {
        GattLocalDescriptorParameters parameters;
        parameters.ReadProtectionLevel(GattProtectionLevel::EncryptionRequired);
        parameters.StaticValue(buffer(std::array<uint8_t, 2>{id, type}));
        auto result = wait(s, c.CreateDescriptorAsync(uuid(0x2908), parameters), "create Report Reference");
        if (result.Error() != BluetoothError::Success)
            throw std::runtime_error("create Report Reference failed");
    }

    GattLocalCharacteristic input(uint8_t id, unsigned kind)
    {
        auto c = characteristic(0x2a4d,
            GattCharacteristicProperties::Read | GattCharacteristicProperties::Notify);
        reference(c, id, 1);
        auto state = s;
        c.ReadRequested([state, kind](auto const&, auto const& args) { read_report(args, state, kind); });
        c.SubscribedClientsChanged([state](auto const&, auto const&) {
            invalidate(state, "subscriptions changed", true);
        });
        return c;
    }

public:
    explicit Service(std::shared_ptr<State> state) : s(std::move(state)) {}
    ~Service()
    {
        connection_changed.revoke();
        if (provider) { try { provider.StopAdvertising(); } catch (...) {} }
    }

    void start()
    {
        auto adapter = wait(s, BluetoothAdapter::GetDefaultAsync(), "get default adapter");
        if (!adapter || !adapter.IsLowEnergySupported() || !adapter.IsPeripheralRoleSupported())
            throw std::runtime_error("default adapter/driver does not support BLE peripheral role");
        auto result = wait(s, GattServiceProvider::CreateAsync(uuid(0x1812)), "create HID service");
        if (result.Error() != BluetoothError::Success)
            throw std::runtime_error("create HID service: BluetoothError=" +
                std::to_string(static_cast<int>(result.Error())));
        provider = result.ServiceProvider();
        characteristic(0x2a4a, GattCharacteristicProperties::Read, {0x11,0x01,0,0x02});
        characteristic(0x2a4b, GattCharacteristicProperties::Read, s->report_map);
        keyboard = input(1, 0);
        pointer = input(2, 1);
        auto led = characteristic(0x2a4d, GattCharacteristicProperties::Read |
            GattCharacteristicProperties::Write | GattCharacteristicProperties::WriteWithoutResponse);
        reference(led, 1, 2);
        auto state = s;
        led.ReadRequested([state](auto const&, auto const& args) { read_report(args, state, 2); });
        led.WriteRequested([state](auto const&, auto const& args) { write_value(args, state, false); });
        auto control = characteristic(0x2a4c, GattCharacteristicProperties::WriteWithoutResponse);
        control.WriteRequested([state](auto const&, auto const& args) { write_value(args, state, true); });
        provider.AdvertisementStatusChanged([state](auto const&, auto const& args) {
            log("INFO", "advertisement status=" + std::to_string(static_cast<int>(args.Status())) +
                " BluetoothError=" + std::to_string(static_cast<int>(args.Error())));
            if (!state->stopping && args.Error() != BluetoothError::Success)
                fail(state, "advertisement failed");
        });
        GattServiceProviderAdvertisingParameters advertising;
        advertising.IsConnectable(true);
        advertising.IsDiscoverable(true);
        provider.StartAdvertising(advertising);
        log("INFO", "HID service started; pair/connect the specified phone from Android Bluetooth settings");
    }

    void refresh()
    {
        uint64_t epoch;
        {
            std::lock_guard<std::mutex> lock(s->mutex);
            if (s->failed || s->closing) return;
            if (s->suspended) return;
            epoch = s->epoch;
        }
        // Cancel() is not completion. Keep the old operation alive and never
        // allow new-session reports to overtake an unsettled notification.
        if (notification) {
            if (notification.Status() == Windows::Foundation::AsyncStatus::Started) {
                if (Clock::now() - notification_started > 5s)
                    throw std::runtime_error("cancelled notification did not settle within 5 seconds");
                invalidate(s, "waiting for old notification cancellation to complete");
                return;
            }
            notification = nullptr; // old-session result is deliberately discarded
        }
        auto keys = keyboard.SubscribedClients();
        auto mice = pointer.SubscribedClients();
        if (keys.Size() != 1 || mice.Size() != 1) {
            invalidate(s, "need exactly one keyboard and one pointer subscription");
            return;
        }
        auto key = keys.GetAt(0), mouse = mice.GetAt(0);
        auto id = key.Session().DeviceId().Id();
        if (id != mouse.Session().DeviceId().Id()) {
            invalidate(s, "keyboard and pointer subscribers differ");
            return;
        }
        if (!peer || peer_id != id) {
            connection_changed.revoke();
            peer = wait(s, BluetoothLEDevice::FromIdAsync(id), "resolve subscriber", 5s, epoch);
            peer_id = id;
            paired = false;
            pairing_refresh = {};
            if (peer) {
                auto state = s;
                auto key_session = key.Session(), mouse_session = mouse.Session();
                connection_changed = peer.ConnectionStatusChanged(auto_revoke,
                    [state, key_session, mouse_session](auto const& device, auto const&) {
                        if (device.ConnectionStatus() != BluetoothConnectionStatus::Connected) {
                            log("INFO", "device ConnectionStatus=disconnected; observed sessions keyboard=" +
                                session_text(key_session) + " pointer=" + session_text(mouse_session));
                            invalidate(state, "phone disconnected", true);
                        }
                    });
            }
        }
        if (!peer) {
            const auto reason = "Windows could not resolve BluetoothLEDevice for subscriber; DeviceId=" + to_string(id);
            invalidate(s, reason.c_str());
            return;
        }
        if (peer.BluetoothAddress() != s->peer_address) {
            const auto reason = "subscriber address mismatch: expected=" + address_text(s->peer_address) +
                " actual=" + address_text(peer.BluetoothAddress()) + " DeviceId=" + to_string(id) +
                " paired=" + std::to_string(peer.DeviceInformation().Pairing().IsPaired()) +
                " connected=" + std::to_string(peer.ConnectionStatus() == BluetoothConnectionStatus::Connected);
            invalidate(s, reason.c_str());
            return;
        }
        if (peer.ConnectionStatus() != BluetoothConnectionStatus::Connected) {
            const auto reason = "phone disconnected; sessions keyboard=" + session_text(key.Session()) +
                " pointer=" + session_text(mouse.Session());
            invalidate(s, reason.c_str());
            return;
        }
        if (Clock::now() >= pairing_refresh) {
            // BluetoothLE# IDs are association endpoints, not necessarily
            // device interfaces (the default CreateFromIdAsync kind).
            auto existing = peer.DeviceInformation();
            auto info = wait(s, Windows::Devices::Enumeration::DeviceInformation::CreateFromIdAsync(
                existing.Id(), nullptr, existing.Kind()), "refresh pairing state", 1500ms, epoch);
            paired = info && info.Pairing().IsPaired();
            pairing_refresh = Clock::now() + 1s;
        }
        if (!paired) {
            invalidate(s, "target phone is not paired");
            return;
        }
        if (key.MaxNotificationSize() < 8 || mouse.MaxNotificationSize() < 8)
            throw std::runtime_error("negotiated notification size is less than 8 bytes");
        bool became_ready = false;
        {
            std::lock_guard<std::mutex> lock(s->mutex);
            if (epoch != s->epoch || s->failed || s->closing || s->suspended) return;
            key_client = key;
            pointer_client = mouse;
            if (!s->ready) {
                // Release reports precede ALL newly accepted Deskflow input.
                s->queue.clear();
                s->queue.push({1, {}, false, s->epoch});
                s->queue.push({2, {}, false, s->epoch});
                s->ready = true;
                s->waiting.clear();
                became_ready = true;
            }
        }
        if (became_ready) log("INFO", "target paired, connected and subscribed; input ready");
    }

    void send(const otg_ble_win_detail::Report& report)
    {
        auto c = report.id == 1 ? keyboard : pointer;
        auto selected = report.id == 1 ? key_client : pointer_client;
        // Refresh snapshots are not authority to broadcast: always recheck
        // this characteristic and the live target immediately before sending.
        auto clients = c.SubscribedClients();
        if (!selected || !peer || peer.ConnectionStatus() != BluetoothConnectionStatus::Connected ||
            clients.Size() != 1 || clients.GetAt(0).Session().DeviceId().Id() != peer_id) {
            invalidate(s, "phone/subscription changed before send", true);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(s->mutex);
            if (!s->ready || report.epoch != s->epoch) return;
        }
        if (notification) throw std::runtime_error("attempt to overlap notifications");
        notification = c.NotifyValueAsync(buffer(report.bytes), selected);
        notification_started = Clock::now();
        GattClientNotificationResult result{nullptr};
        try {
            result = wait(s, notification, report.id == 1 ? "notify keyboard" : "notify pointer", 1500ms, report.epoch);
        } catch (const otg_ble_win_detail::WaitAborted& e) {
            size_t queued;
            { std::lock_guard<std::mutex> lock(s->mutex); queued = s->queue.size(); }
            log("WARN", std::string(e.what()) + " report=" + std::to_string(report.id) +
                " flags/modifiers=" + std::to_string(report.bytes[0]) +
                " buttons/reserved=" + std::to_string(report.bytes[1]) +
                " queue=" + std::to_string(queued) + " sent-keyboard=" + std::to_string(sent_keyboard) +
                " sent-pointer=" + std::to_string(sent_pointer) +
                " device=" + (peer.ConnectionStatus() == BluetoothConnectionStatus::Connected ? "connected" : "disconnected") +
                " keyboard-session=" + session_text(key_client.Session()) +
                " pointer-session=" + session_text(pointer_client.Session()));
            throw;
        }
        notification = nullptr;
        if (result.Status() != GattCommunicationStatus::Success) {
            log("WARN", "notification result=" + std::to_string(static_cast<int>(result.Status())));
            invalidate(s, "notification failed; waiting for phone", true);
            return;
        }
        if (report.id == 1) ++sent_keyboard;
        else ++sent_pointer;
        std::lock_guard<std::mutex> lock(s->mutex);
        if (report.epoch == s->epoch)
            (report.id == 1 ? s->keyboard : s->pointer) = report.bytes;
    }

    // A local overload/failure must not intentionally abandon held inputs on
    // a still-connected phone. This runs only on the worker, after its last
    // notification, and never broadcasts. Link loss can still prevent release.
    void release()
    {
        if (s->stopping || !peer || peer.BluetoothAddress() != s->peer_address ||
            peer.ConnectionStatus() != BluetoothConnectionStatus::Connected || !paired) return;
        if (notification && notification.Status() == Windows::Foundation::AsyncStatus::Started) {
            log("WARN", "final release skipped: previous notification is still in flight");
            return;
        }
        notification = nullptr;
        for (auto c : {keyboard, pointer}) {
            if (!c) continue;
            try {
                auto clients = c.SubscribedClients();
                if (clients.Size() != 1 || clients.GetAt(0).Session().DeviceId().Id() != peer_id) continue;
                notification = c.NotifyValueAsync(buffer(std::array<uint8_t, 8>{}), clients.GetAt(0));
                notification_started = Clock::now();
                auto result = wait(s, notification, "final release", 250ms);
                notification = nullptr;
                if (result.Status() != GattCommunicationStatus::Success)
                    log("WARN", "final release could not be delivered");
            } catch (const std::exception& e) {
                log("WARN", std::string("final release failed: ") + e.what());
                if (notification && notification.Status() == Windows::Foundation::AsyncStatus::Started) return;
                notification = nullptr;
            } catch (...) {
                log("WARN", "final release failed (WinRT error; best effort only)");
                return;
            }
        }
    }

    bool recover(const otg_ble_win_detail::WaitAborted& e)
    {
        if (e.reason == otg_ble_win_detail::WaitReason::stopping) return false;
        if (e.reason == otg_ble_win_detail::WaitReason::session_changed ||
            (peer && peer.ConnectionStatus() != BluetoothConnectionStatus::Connected)) {
            invalidate(s, "session ended; waiting for phone reconnect", true);
            pairing_refresh = {};
            log("INFO", std::string(e.what()) + "; retaining GATT service");
            return true;
        }
        return false;
    }

    bool recover_disconnected(const hresult_error& e)
    {
        if (!peer || peer.ConnectionStatus() == BluetoothConnectionStatus::Connected) return false;
        invalidate(s, "session ended; waiting for phone reconnect", true);
        pairing_refresh = {};
        log("INFO", describe(e) + "; phone disconnected, retaining GATT service");
        return true;
    }
};

static void worker(const std::shared_ptr<State>& s)
{
    try {
        struct Apartment {
            Apartment() { init_apartment(apartment_type::multi_threaded); }
            ~Apartment() { uninit_apartment(); }
        } apartment;
        {
            Service service(s);
            try {
                service.start();
                auto next_refresh = Clock::now();
                Clock::time_point next_notification{};
                while (!s->stopping) {
                    try {
                        if (Clock::now() >= next_refresh) {
                            service.refresh();
                            next_refresh = Clock::now() + 100ms;
                        }
                        otg_ble_win_detail::Report report;
                        {
                            std::unique_lock<std::mutex> lock(s->mutex);
                            if (s->failed) break;
                            if (!s->ready || s->queue.empty()) {
                                s->cv.wait_for(lock, 5ms);
                                continue;
                            }
                            if (Clock::now() < next_notification) {
                                s->cv.wait_until(lock, next_notification);
                                continue;
                            }
                            report = s->queue.pop();
                            s->in_flight = true;
                        }
                        service.send(report);
                        next_notification = Clock::now() +
                            std::chrono::milliseconds(otg_ble_win_detail::notification_interval_ms);
                        {
                            std::lock_guard<std::mutex> lock(s->mutex);
                            s->in_flight = false;
                        }
                        s->cv.notify_all();
                    } catch (const otg_ble_win_detail::WaitAborted& e) {
                        if (!service.recover(e)) {
                            // Disable input immediately, BEFORE slow best-effort
                            // releases/provider teardown. Otherwise new presses
                            // can still queue while a fatal send is unwinding.
                            if (!s->stopping) fail(s, e.what());
                            throw;
                        }
                        {
                            std::lock_guard<std::mutex> lock(s->mutex);
                            s->in_flight = false;
                        }
                        s->cv.notify_all();
                        next_refresh = Clock::now() + 100ms;
                    } catch (const hresult_error& e) {
                        if (!service.recover_disconnected(e)) throw;
                        {
                            std::lock_guard<std::mutex> lock(s->mutex);
                            s->in_flight = false;
                        }
                        s->cv.notify_all();
                        next_refresh = Clock::now() + 100ms;
                    }
                }
                service.release();
            } catch (...) {
                service.release();
                throw;
            }
        }
    } catch (const hresult_error& e) {
        if (!s->stopping) fail(s, describe(e));
    } catch (const std::exception& e) {
        if (!s->stopping) fail(s, e.what());
    } catch (...) {
        if (!s->stopping) fail(s, "unexpected worker exception");
    }
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        s->in_flight = false;
        s->ready = false;
        s->worker_done = true;
    }
    s->cv.notify_all();
}
} // namespace

struct otg_ble_win {
    std::shared_ptr<State> state = std::make_shared<State>();
    std::thread thread;
    unsigned registered = 0;
};

static bool register_report(void *ctx, uint16_t id, const uint8_t *desc, uint16_t size)
{
    auto b = static_cast<otg_ble_win *>(ctx);
    try {
        if ((id != 1 && id != 2) || b->registered != static_cast<unsigned>(id - 1) ||
            !desc || size < 7 || (id == 1 && (desc[1] != 1 || desc[3] != 6)) ||
            (id == 2 && (desc[1] != 0x0d || desc[3] != 1)) ||
            !otg_ble_win_detail::append_descriptor(b->state->report_map, desc, size, static_cast<uint8_t>(id)))
            return false;
        ++b->registered;
        if (id == 2) b->thread = std::thread(worker, b->state);
        return true;
    } catch (const std::exception& e) { log("ERROR", e.what()); return false; }
}

static bool send_report(void *ctx, uint16_t id, const uint8_t *data, uint16_t size)
{
    auto s = static_cast<otg_ble_win *>(ctx)->state;
    if (!data || size != 8 || (id != 1 && id != 2)) return false;
    bool overflow = false;
    try {
        {
            std::lock_guard<std::mutex> lock(s->mutex);
            if (!s->ready || s->failed || s->closing) return false;
            otg_ble_win_detail::Report report{id, {}, false, s->epoch};
            std::memcpy(report.bytes.data(), data, 8);
            overflow = !s->queue.push(report);
            if (id == 2 && !overflow) s->last_mouse = report.bytes;
        }
        if (overflow) fail(s, "report queue overflow; closing transport rather than dropping input transitions");
        s->cv.notify_all();
        return !overflow;
    } catch (const std::exception& e) { fail(s, e.what()); return false; }
}

static bool poll_transport(void *ctx)
{
    auto s = static_cast<otg_ble_win *>(ctx)->state;
    std::lock_guard<std::mutex> lock(s->mutex);
    return !s->failed && !s->stopping;
}
static bool ready(void *ctx)
{
    auto s = static_cast<otg_ble_win *>(ctx)->state;
    std::lock_guard<std::mutex> lock(s->mutex);
    return s->ready && !s->failed && !s->closing;
}
static void close_transport(void *ctx)
{
    auto b = static_cast<otg_ble_win *>(ctx);
    if (!b->thread.joinable()) return;
    auto s = b->state;
    {
        std::unique_lock<std::mutex> lock(s->mutex);
        s->closing = true;
        // On exit, discard unsent actions, then release after any in-flight
        // notification. Never send queued presses after the final release.
        s->queue.clear();
        if (s->ready && !s->failed) {
            auto mouse = s->last_mouse;
            mouse[1] = mouse[6] = mouse[7] = 0;
            mouse[0] = (mouse[0] & 2) ? 2 : 0;
            s->queue.push({1, {}, false, s->epoch});
            s->queue.push({2, mouse, false, s->epoch});
            mouse[0] = 0;
            s->queue.push({2, mouse, false, s->epoch});
        }
        s->cv.notify_all();
        s->cv.wait_for(lock, 2s, [&] {
            // Let a failed worker attempt its own releases before cancelling
            // it. Otherwise a queue overflow could race shutdown and skip them.
            return s->worker_done || (!s->failed &&
                (!s->ready || (s->queue.empty() && !s->in_flight)));
        });
    }
    s->stopping = true;
    s->cv.notify_all();
    b->thread.join();
}

extern "C" otg_ble_win *otg_ble_win_create(const char *peer)
{
    try {
        if (!peer || std::strlen(peer) != 17) return nullptr;
        uint64_t address = 0;
        for (unsigned i = 0; i < 17; ++i) {
            if (i % 3 == 2) { if (peer[i] != ':') return nullptr; continue; }
            char c = peer[i];
            int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 :
                c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (digit < 0) return nullptr;
            address = (address << 4) | static_cast<unsigned>(digit);
        }
        if (!address) return nullptr;
        auto b = std::make_unique<otg_ble_win>();
        b->state->peer_address = address;
        return b.release();
    } catch (...) { return nullptr; }
}
extern "C" const otg_hid_transport_ops *otg_ble_win_ops(void)
{
    static const otg_hid_transport_ops ops = {register_report, send_report, poll_transport, ready, close_transport};
    return &ops;
}
extern "C" void otg_ble_win_destroy(otg_ble_win *b)
{
    if (b) { close_transport(b); delete b; }
}
