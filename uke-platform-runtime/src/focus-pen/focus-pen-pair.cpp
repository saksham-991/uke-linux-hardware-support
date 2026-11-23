// SPDX-License-Identifier: Apache-2.0

#include <systemd/sd-bus.h>

#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
constexpr auto bluez = "org.bluez";
constexpr auto adapter = "/org/bluez/hci0";
constexpr auto adapter_iface = "org.bluez.Adapter1";
constexpr auto device_iface = "org.bluez.Device1";
constexpr auto manager_iface = "org.bluez.AgentManager1";
constexpr auto agent_path = "/org/uke/FocusPenAgent";
static volatile std::sig_atomic_t stopping;

template <typename T, auto Unref>
using Owner = std::unique_ptr<T, decltype(Unref)>;
using Bus = Owner<sd_bus, sd_bus_unref>;
using Message = Owner<sd_bus_message, sd_bus_message_unref>;
using Slot = Owner<sd_bus_slot, sd_bus_slot_unref>;

struct Dock {
    std::string address;
};

std::optional<Dock> read_dock(const std::filesystem::path &path)
{
    std::ifstream stream(path);
    std::string address, extra;
    unsigned hall3, hall4;
    if (!(stream >> hall3 >> hall4 >> address) || stream >> extra ||
        hall3 > 1 || hall4 > 1 || (hall3 == 1 && hall4 == 1) ||
        address.size() != 12 || address == "000000000000" ||
        address == "FFFFFFFFFFFF")
        return std::nullopt;

    std::string formatted;
    for (std::size_t i = 0; i < address.size(); ++i) {
        const auto ch = static_cast<unsigned char>(address[i]);
        if (!std::isxdigit(ch))
            return std::nullopt;
        if (i && i % 2 == 0)
            formatted += ':';
        formatted += static_cast<char>(std::toupper(ch));
    }
    return Dock{formatted};
}

std::filesystem::path find_dock()
{
    std::error_code error;
    const std::filesystem::path devices("/sys/bus/auxiliary/devices");
    for (auto entry = std::filesystem::directory_iterator(devices, error);
         !error && entry != std::filesystem::directory_iterator(); entry.increment(error)) {
        const auto path = entry->path() / "uke_pen_dock";
        std::error_code probe_error;
        if (std::filesystem::exists(path, probe_error))
            return path;
    }
    return {};
}

bool bool_property(sd_bus *bus, const char *path, const char *interface, const char *property)
{
    int value = 0;
    return sd_bus_get_property_trivial(bus, bluez, path, interface, property, nullptr, 'b', &value) >= 0 && value;
}

std::string string_property(sd_bus *bus, const char *path, const char *property)
{
    char *value = nullptr;
    if (sd_bus_get_property_string(bus, bluez, path, device_iface, property, nullptr, &value) < 0)
        return {};
    const std::unique_ptr<char, decltype(&std::free)> owner(value, std::free);
    return std::string(value);
}

struct PairState {
    std::string device;
    std::string error;
    bool complete = false;
    bool paired = false;
};

int finish_pair(sd_bus_message *message, void *userdata, sd_bus_error *)
{
    auto &state = *static_cast<PairState *>(userdata);
    if (const auto *error = sd_bus_message_get_error(message))
        state.error = error->name;
    else
        state.paired = true;
    state.complete = true;
    return 0;
}

int authorize_pen(sd_bus_message *message, void *userdata, sd_bus_error *)
{
    const auto &state = *static_cast<PairState *>(userdata);
    const char *path;
    if (sd_bus_message_read(message, "o", &path) < 0 || state.device != path)
        return sd_bus_reply_method_errorf(message, "org.bluez.Error.Rejected", "Unexpected pairing target");
    return sd_bus_reply_method_return(message, "");
}

int acknowledge(sd_bus_message *message, void *, sd_bus_error *)
{
    return sd_bus_reply_method_return(message, "");
}

const sd_bus_vtable agent[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Release", "", "", acknowledge, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Cancel", "", "", acknowledge, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RequestAuthorization", "o", "", authorize_pen, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RequestConfirmation", "ou", "", authorize_pen, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END
};

struct AdapterSession {
    explicit AdapterSession(sd_bus *connection) : bus(connection) {}
    ~AdapterSession() {
        if (changed_pairable)
            sd_bus_set_property(bus, bluez, adapter, adapter_iface, "Pairable", nullptr, "b", 0);
        if (discovering)
            sd_bus_call_method(bus, bluez, adapter, adapter_iface, "StopDiscovery", nullptr, nullptr, "");
        if (registered)
            sd_bus_call_method(bus, bluez, "/org/bluez", manager_iface, "UnregisterAgent",
                               nullptr, nullptr, "o", agent_path);
    }
    sd_bus *bus;
    bool changed_pairable = false;
    bool discovering = false;
    bool registered = false;
};

bool discover_le(sd_bus *bus)
{
    sd_bus_message *request = nullptr;
    int ret = sd_bus_message_new_method_call(bus, &request, bluez, adapter, adapter_iface, "SetDiscoveryFilter");
    const Message message(request, sd_bus_message_unref);
    if (ret < 0 ||
        sd_bus_message_open_container(request, 'a', "{sv}") < 0 ||
        sd_bus_message_append(request, "{sv}", "Transport", "s", "le") < 0 ||
        sd_bus_message_close_container(request) < 0 ||
        sd_bus_call(bus, request, 2'000'000, nullptr, nullptr) < 0)
        return false;

    return sd_bus_call_method(bus, bluez, adapter, adapter_iface, "StartDiscovery",
                              nullptr, nullptr, "") >= 0;
}

bool pair(const Dock &dock, const std::filesystem::path &dock_path)
{
    sd_bus *raw_bus = nullptr;
    if (sd_bus_open_system(&raw_bus) < 0)
        return false;
    const Bus bus(raw_bus, sd_bus_unref);
    sd_bus_set_method_call_timeout(raw_bus, 3'000'000);

    PairState state;
    state.device = std::string(adapter) + "/dev_" + dock.address;
    for (char &ch : state.device) {
        if (ch == ':')
            ch = '_';
    }
    const auto exact_pen = [&] {
        return string_property(raw_bus, state.device.c_str(), "Address") == dock.address &&
            string_property(raw_bus, state.device.c_str(), "Name") == "Xiaomi Focus Pen";
    };
    if (exact_pen() && bool_property(raw_bus, state.device.c_str(), device_iface, "Bonded"))
        return true;

    sd_bus_slot *raw_agent = nullptr;
    auto ret = sd_bus_add_object_vtable(raw_bus, &raw_agent, agent_path, "org.bluez.Agent1", agent, &state);
    const Slot agent_slot(raw_agent, sd_bus_slot_unref);
    AdapterSession session(raw_bus);
    if (ret < 0 || sd_bus_call_method(raw_bus, bluez, "/org/bluez", manager_iface, "RegisterAgent",
                                     nullptr, nullptr, "os", agent_path, "NoInputNoOutput") < 0)
        return false;
    session.registered = true;
    if (!discover_le(raw_bus))
        return false;
    session.discovering = true;

    const auto discovery_deadline = Clock::now() + 8s;
    while (!stopping && !exact_pen()) {
        if (Clock::now() >= discovery_deadline)
            return false;
        std::this_thread::sleep_for(200ms);
    }
    const auto still_docked = read_dock(dock_path);
    if (stopping || !still_docked || still_docked->address != dock.address)
        return false;
    if (bool_property(raw_bus, state.device.c_str(), device_iface, "Bonded"))
        return true;

    int pairable = 0;
    if (sd_bus_get_property_trivial(raw_bus, bluez, adapter, adapter_iface, "Pairable",
                                    nullptr, 'b', &pairable) < 0)
        return false;
    if (!pairable) {
        if (sd_bus_set_property(raw_bus, bluez, adapter, adapter_iface, "Pairable", nullptr, "b", 1) < 0)
            return false;
        session.changed_pairable = true;
    }

    sd_bus_message *request = nullptr;
    ret = sd_bus_message_new_method_call(raw_bus, &request, bluez, state.device.c_str(), device_iface, "Pair");
    const Message message(request, sd_bus_message_unref);
    if (ret < 0)
        return false;
    sd_bus_slot *raw_slot = nullptr;
    ret = sd_bus_call_async(raw_bus, &raw_slot, request, finish_pair, &state, 20'000'000);
    const Slot pair_slot(raw_slot, sd_bus_slot_unref);
    if (ret < 0)
        return false;
    while (!stopping && !state.complete) {
        ret = sd_bus_process(raw_bus, nullptr);
        if (ret < 0 || (ret == 0 && sd_bus_wait(raw_bus, 250'000) < 0))
            break;
    }
    if (!state.complete)
        sd_bus_call_method(raw_bus, bluez, state.device.c_str(), device_iface, "CancelPairing",
                           nullptr, nullptr, "");
    if (!state.paired || !bool_property(raw_bus, state.device.c_str(), device_iface, "Bonded")) {
        if (!state.error.empty())
            std::cerr << "BlueZ could not bond the docked Focus Pen (" << state.error << ")\n";
        return false;
    }
    if (sd_bus_set_property(raw_bus, bluez, state.device.c_str(), device_iface,
                            "Trusted", nullptr, "b", 1) < 0)
        std::cerr << "Focus Pen bonded; BlueZ declined the trust setting\n";
    std::cout << "Docked Focus Pen bonded\n";
    return true;
}

int run(const std::filesystem::path &test_path)
{
    std::string last_address;
    auto retry_after = Clock::time_point{};
    while (!stopping) {
        const auto path = test_path.empty() ? find_dock() : test_path;
        if (!path.empty()) {
            if (const auto dock = read_dock(path)) {
                const auto now = Clock::now();
                if (dock->address != last_address || now >= retry_after) {
                    last_address = dock->address;
                    const auto success = pair(*dock, path);
                    if (!test_path.empty())
                        return success ? 0 : 1;
                    retry_after = Clock::now() + (success ? 60s : 5s);
                }
            } else {
                last_address.clear();
            }
        }
        if (!test_path.empty())
            return 1;
        std::this_thread::sleep_for(1s);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 1 && (argc != 3 || std::string_view(argv[1]) != "--once"))
        return 2;
    std::signal(SIGTERM, [](int) { stopping = 1; });
    std::signal(SIGINT, [](int) { stopping = 1; });
    try {
        return run(argc == 3 ? argv[2] : "");
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
