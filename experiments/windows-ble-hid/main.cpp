/* Windows BLE HID feasibility probe; HOGP layout follows platform/linux/ble_hid.c.
 * Not a Deskflow backend. SPDX-License-Identifier: Apache-2.0 */
#include "reports.h"
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Storage.Streams.h>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

using namespace winrt;
using namespace Windows::Devices::Bluetooth;
using namespace Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace Windows::Storage::Streams;

static std::mutex log_mutex;
static void log(const std::string& text)
{
    std::lock_guard<std::mutex> lock(log_mutex);
    std::cout << text << std::endl;
}
static void error(const char* where, const hresult_error& e)
{
    log(std::string(where) + ": HRESULT=" + std::to_string(e.code().value) +
        " " + to_string(e.message()));
}
static guid uuid(uint16_t short_id)
{
    return {short_id, 0x0000, 0x1000, {0x80,0x00,0x00,0x80,0x5f,0x9b,0x34,0xfb}};
}
template<class Bytes> static IBuffer buffer(const Bytes& bytes)
{
    DataWriter writer;
    writer.WriteBytes(bytes);
    return writer.DetachBuffer();
}

struct State {
    std::atomic<bool> suspended{false};
    std::atomic<bool> stopping{false};
    std::atomic<uint64_t> subscriptions{0};
    std::mutex mutex;
    std::array<uint8_t, 8> keyboard{};
    std::array<uint8_t, 8> pointer{};
    uint8_t leds = 0;
};

static fire_and_forget read_report(GattReadRequestedEventArgs args,
    std::shared_ptr<State> state, int kind)
{
    auto deferral = args.GetDeferral();
    try {
        auto request = co_await args.GetRequestAsync();
        if (request) {
            IBuffer value{nullptr};
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (kind == 0) value = buffer(state->keyboard);
                else if (kind == 1) value = buffer(state->pointer);
                else value = buffer(std::array<uint8_t, 1>{state->leds});
            }
            // Windows handles ATT long-read slicing using request.Offset().
            request.RespondWithValue(value);
        }
    } catch (const hresult_error& e) { error("ReadRequested", e); }
    deferral.Complete();
}

static fire_and_forget write_value(GattWriteRequestedEventArgs args,
    std::shared_ptr<State> state, bool control)
{
    auto deferral = args.GetDeferral();
    try {
        auto request = co_await args.GetRequestAsync();
        if (request) {
            if (request.Offset() != 0 || request.Value().Length() != 1) {
                if (request.Option() == GattWriteOption::WriteWithResponse)
                    request.RespondWithProtocolError(0x0d);
            } else {
                auto value = DataReader::FromBuffer(request.Value()).ReadByte();
                if (control && value > 1) {
                    if (request.Option() == GattWriteOption::WriteWithResponse)
                        request.RespondWithProtocolError(0x13);
                } else {
                    if (control) {
                        state->suspended = value == 0;
                        log(value == 0 ? "CONTROL: suspend" : "CONTROL: exit suspend");
                    } else {
                        std::lock_guard<std::mutex> lock(state->mutex);
                        state->leds = value;
                        log("LED output=" + std::to_string(value));
                    }
                    if (request.Option() == GattWriteOption::WriteWithResponse)
                        request.Respond();
                }
            }
        }
    } catch (const hresult_error& e) { error("WriteRequested", e); }
    deferral.Complete();
}

class Probe {
    std::shared_ptr<State> state = std::make_shared<State>();
    GattServiceProvider provider{nullptr};
    GattLocalCharacteristic keyboard{nullptr}, pointer{nullptr};
    bool keyboard_only;
    hstring trusted_id;
    uint64_t armed_generation = 0;

    GattLocalCharacteristic characteristic(uint16_t id,
        GattCharacteristicProperties properties, const std::vector<uint8_t>& value = {})
    {
        GattLocalCharacteristicParameters parameters;
        parameters.CharacteristicProperties(properties);
        parameters.ReadProtectionLevel(GattProtectionLevel::EncryptionRequired);
        parameters.WriteProtectionLevel(GattProtectionLevel::EncryptionRequired);
        if (!value.empty()) parameters.StaticValue(buffer(value));
        auto result = provider.Service().CreateCharacteristicAsync(uuid(id), parameters).get();
        if (result.Error() != BluetoothError::Success)
            throw std::runtime_error("CreateCharacteristic " + std::to_string(id) +
                " BluetoothError=" + std::to_string(static_cast<int>(result.Error())));
        return result.Characteristic();
    }

    static void reference(const GattLocalCharacteristic& characteristic, uint8_t id, uint8_t type)
    {
        GattLocalDescriptorParameters parameters;
        parameters.ReadProtectionLevel(GattProtectionLevel::EncryptionRequired);
        parameters.StaticValue(buffer(std::array<uint8_t, 2>{id, type}));
        auto result = characteristic.CreateDescriptorAsync(uuid(0x2908), parameters).get();
        if (result.Error() != BluetoothError::Success)
            throw std::runtime_error("Create Report Reference failed: " +
                std::to_string(static_cast<int>(result.Error())));
    }

    GattLocalCharacteristic input(uint8_t id, int kind)
    {
        auto c = characteristic(0x2a4d,
            GattCharacteristicProperties::Read | GattCharacteristicProperties::Notify);
        reference(c, id, 1);
        auto shared = state;
        c.ReadRequested([shared, kind](auto const&, auto const& args) {
            read_report(args, shared, kind);
        });
        c.SubscribedClientsChanged([shared, id](auto const& sender, auto const&) {
            ++shared->subscriptions;
            log("SUBSCRIBERS report=" + std::to_string(id) +
                " count=" + std::to_string(sender.SubscribedClients().Size()) +
                " (input disarmed; run status then arm)");
        });
        return c;
    }

    // Never broadcast input. Both report subscriptions must belong to the
    // explicitly armed, paired client; any subscription change disarms input.
    GattSubscribedClient client(const GattLocalCharacteristic& c, bool arming = false)
    {
        if (state->suspended || state->stopping)
            throw std::runtime_error("Input suspended/stopping");
        if (!arming && (trusted_id.empty() || armed_generation != state->subscriptions.load()))
            throw std::runtime_error("Not armed: run status, then arm");
        auto clients = c.SubscribedClients();
        if (clients.Size() != 1)
            throw std::runtime_error("Need exactly one subscriber per input report");
        auto selected = clients.GetAt(0);
        auto id = selected.Session().DeviceId().Id();
        if (!arming && id != trusted_id)
            throw std::runtime_error("Subscriber identity changed");
        auto device = BluetoothLEDevice::FromIdAsync(id).get();
        if (!device || !device.DeviceInformation().Pairing().IsPaired() ||
            device.ConnectionStatus() != BluetoothConnectionStatus::Connected)
            throw std::runtime_error("Subscriber must be paired and connected");
        return selected;
    }

    void send(const GattLocalCharacteristic& c, const std::array<uint8_t, 8>& data, bool mouse)
    {
        auto selected = client(c);
        if (selected.MaxNotificationSize() < data.size())
            throw std::runtime_error("Notification size too small");
        auto result = c.NotifyValueAsync(buffer(data), selected).get();
        if (result.Status() != GattCommunicationStatus::Success) {
            trusted_id.clear();
            throw std::runtime_error("Notify failed, GattCommunicationStatus=" +
                std::to_string(static_cast<int>(result.Status())) + "; input disarmed");
        }
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            (mouse ? state->pointer : state->keyboard) = data;
        }
        log(mouse ? "SENT pointer (8 bytes)" : "SENT keyboard (8 bytes)");
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }

public:
    explicit Probe(bool only) : keyboard_only(only) {}
    ~Probe()
    {
        // Best effort release before stopping advertisements/GATT lifetime.
        try { release(); } catch (...) {}
        state->stopping = true;
        if (provider) {
            try { provider.StopAdvertising(); } catch (...) {}
        }
    }

    void start()
    {
        auto result = GattServiceProvider::CreateAsync(uuid(0x1812)).get();
        log("Create HID service: BluetoothError=" + std::to_string(static_cast<int>(result.Error())));
        if (result.Error() != BluetoothError::Success)
            throw std::runtime_error("Cannot publish HID service (policy/driver/API restriction)");
        provider = result.ServiceProvider();
        characteristic(0x2a4a, GattCharacteristicProperties::Read, {0x11,0x01,0x00,0x02});
        characteristic(0x2a4b, GattCharacteristicProperties::Read, reports::map(keyboard_only));
        keyboard = input(1, 0);
        if (!keyboard_only) pointer = input(2, 1);
        auto led = characteristic(0x2a4d, GattCharacteristicProperties::Read |
            GattCharacteristicProperties::Write | GattCharacteristicProperties::WriteWithoutResponse);
        reference(led, 1, 2);
        auto shared = state;
        led.ReadRequested([shared](auto const&, auto const& args) { read_report(args, shared, 2); });
        led.WriteRequested([shared](auto const&, auto const& args) { write_value(args, shared, false); });
        auto control = characteristic(0x2a4c, GattCharacteristicProperties::WriteWithoutResponse);
        control.WriteRequested([shared](auto const&, auto const& args) { write_value(args, shared, true); });
        provider.AdvertisementStatusChanged([](auto const&, auto const& args) {
            log("ADVERTISEMENT status=" + std::to_string(static_cast<int>(args.Status())) +
                " BluetoothError=" + std::to_string(static_cast<int>(args.Error())));
        });
        GattServiceProviderAdvertisingParameters advertising;
        advertising.IsConnectable(true);
        advertising.IsDiscoverable(true);
        provider.StartAdvertising(advertising);
        log("Advertising requested. Pair from Android Bluetooth settings using this PC's name.");
    }

    void status()
    {
        log("STATUS suspended=" + std::to_string(state->suspended.load()) +
            " armed=" + std::to_string(!trusted_id.empty() &&
                armed_generation == state->subscriptions.load()));
        for (auto c : {keyboard, pointer}) {
            if (!c) continue;
            log("Report subscribers=" + std::to_string(c.SubscribedClients().Size()));
            for (auto const& subscriber : c.SubscribedClients()) {
                auto id = subscriber.Session().DeviceId().Id();
                log("  DeviceId=" + to_string(id));
                auto device = BluetoothLEDevice::FromIdAsync(id).get();
                if (device) log("  Name=" + to_string(device.Name()) +
                    " paired=" + std::to_string(device.DeviceInformation().Pairing().IsPaired()) +
                    " connected=" + std::to_string(device.ConnectionStatus() == BluetoothConnectionStatus::Connected));
            }
        }
    }

    void arm()
    {
        trusted_id.clear();
        auto generation = state->subscriptions.load();
        auto selected = client(keyboard, true);
        auto id = selected.Session().DeviceId().Id();
        if (pointer && client(pointer, true).Session().DeviceId().Id() != id)
            throw std::runtime_error("Keyboard and pointer subscribers differ");
        if (generation != state->subscriptions.load())
            throw std::runtime_error("Subscriptions changed; retry arm");
        trusted_id = id;
        armed_generation = generation;
        log("ARMED for " + to_string(id));
        release();
    }

    void release()
    {
        if (trusted_id.empty()) return;
        // Attempt both releases even if the first fails.
        try { send(keyboard, {}, false); } catch (const std::exception& e) { log(e.what()); }
        if (pointer) {
            try { send(pointer, reports::pointer(16384, 16384, 0), true); }
            catch (const std::exception& e) { log(e.what()); }
        }
    }

    void test(const std::string& command)
    {
        if (command == "key") {
            send(keyboard, {0,0,0x04,0,0,0,0,0}, false); // a, no modifiers
            send(keyboard, {}, false);
        } else if (command == "release") release();
        else if (command == "disarm") { release(); trusted_id.clear(); log("DISARMED"); }
        else if (!pointer) throw std::runtime_error("Pointer disabled by --keyboard-only");
        else if (command == "corners") {
            for (auto xy : {std::array<uint16_t,2>{0,0}, {32767,0}, {32767,32767}, {0,32767}, {16384,16384}}) {
                send(pointer, reports::pointer(xy[0], xy[1]), true);
                std::this_thread::sleep_for(std::chrono::milliseconds(700));
            }
        } else if (command == "center") send(pointer, reports::pointer(16384,16384), true);
        else if (command == "click") {
            send(pointer, reports::pointer(16384,16384), true);
            send(pointer, reports::pointer(16384,16384,3,1), true);
            send(pointer, reports::pointer(16384,16384), true);
        } else if (command == "wheel") {
            send(pointer, reports::pointer(16384,16384,2,0,-1), true);
            send(pointer, reports::pointer(16384,16384), true);
        } else if (command == "drag") {
            send(pointer, reports::pointer(8192,16384), true);
            for (uint16_t x = 8192; x <= 24576; x += 1024)
                send(pointer, reports::pointer(x,16384,3,1), true);
            send(pointer, reports::pointer(24576,16384), true);
        } else throw std::runtime_error("Unknown command (type help)");
    }
};

static int self_test()
{
    if (reports::map(true).size() != sizeof(reports::keyboard_desc) + 2 ||
        reports::map(false).size() != sizeof(reports::keyboard_desc) + sizeof(reports::pointer_desc) + 4 ||
        reports::map(false)[6] != 0x85 || reports::map(false)[7] != 1 ||
        reports::map(false)[sizeof(reports::keyboard_desc) + 9] != 2 ||
        reports::pointer(32767,16384,3,1,-1) !=
            std::array<uint8_t,8>{3,1,0xff,0x7f,0,0x40,0xff,0}) {
        log("FAIL: report encoding");
        return 1;
    }
    log("PASS: Report Map IDs, sizes and absolute pointer encoding (no Bluetooth accessed)");
    return 0;
}

static void help()
{
    log("Commands: status | arm | disarm | key | center | corners | click | wheel | drag | release | quit");
    log("No input is sent until arm. Check status identifies YOUR phone before arming.");
    log("key types 'a'; click/drag act on the phone screen. Use a safe test app.");
}

int main(int argc, char** argv)
{
    bool only = false, probe_only = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--self-test") return self_test();
        if (arg == "--keyboard-only") only = true;
        else if (arg == "--probe") probe_only = true;
        else if (arg == "--help") {
            log("deskflow-otg-ble-probe [--probe | --keyboard-only | --self-test]");
            help();
            return 0;
        } else { log("Unknown option: " + arg); return 2; }
    }
    try {
        init_apartment(apartment_type::multi_threaded);
        auto adapter = BluetoothAdapter::GetDefaultAsync().get();
        if (!adapter) throw std::runtime_error("No default Bluetooth adapter");
        log("Adapter=" + to_string(adapter.DeviceId()));
        log("LE supported=" + std::to_string(adapter.IsLowEnergySupported()) +
            " peripheral supported=" + std::to_string(adapter.IsPeripheralRoleSupported()));
        if (!adapter.IsLowEnergySupported() || !adapter.IsPeripheralRoleSupported())
            throw std::runtime_error("Default adapter/driver cannot serve as BLE peripheral");
        if (probe_only) return 0;
        Probe probe(only);
        probe.start();
        help();
        std::string command;
        while (std::getline(std::cin, command)) {
            if (command == "quit" || command == "q") break;
            try {
                if (command == "help") help();
                else if (command == "status") probe.status();
                else if (command == "arm") probe.arm();
                else if (!command.empty()) probe.test(command);
            } catch (const hresult_error& e) { error("Command", e); probe.release(); }
            catch (const std::exception& e) { log(std::string("Command: ") + e.what()); probe.release(); }
        }
        log("Exiting; best-effort release and stop advertising.");
    } catch (const hresult_error& e) { error("Fatal", e); return 1; }
    catch (const std::exception& e) { log(std::string("Fatal: ") + e.what()); return 1; }
    return 0;
}
