// SPDX-License-Identifier: Apache-2.0

#include "nvt_stylus.hpp"
#include "tip-pressure.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <unistd.h>

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using Stamp = Clock::time_point;
constexpr std::size_t frame_size = 5432;
constexpr int width = 31999, height = 21359;
constexpr int reference_width = 30479, reference_height = 20319;
constexpr int pressure_max = 8191;
constexpr std::string_view hid_name = "Xiaomi Focus Pen";
constexpr std::string_view input_name = "Xiaomi Pad 7 Focus Pen";
static volatile std::sig_atomic_t stop;

struct Frame {
    std::uint64_t timestamp_ns;
    std::array<std::uint8_t, frame_size> bytes;
};
static_assert(sizeof(Frame) == 5440);

class File {
public:
    explicit File(int fd = -1) : fd_(fd) {}
    File(const File &) = delete;
    File &operator=(const File &) = delete;
    ~File() { reset(); }
    int get() const { return fd_; }
    explicit operator bool() const { return fd_ >= 0; }
    void reset(int fd = -1) {
        if (fd_ >= 0)
            close(fd_);
        fd_ = fd;
    }
private:
    int fd_;
};

[[noreturn]] void sys_failure(const char *operation)
{
    throw std::system_error(errno, std::generic_category(), operation);
}

template<typename Arg>
void set_ioctl(int fd, unsigned long command, Arg value)
{
    if (ioctl(fd, command, value) < 0)
        sys_failure("configuring uinput");
}

std::uint16_t le16(const std::uint8_t *bytes)
{
    return bytes[0] | static_cast<std::uint16_t>(bytes[1]) << 8;
}

bool decode_frame(const Frame &frame, nvt::RawStylusFrame &raw)
{
    const auto *header = frame.bytes.data() + 256;
    if (le16(header) != 0x61a5 || header[56] != 0x1d ||
        header[8] != 0x09 || header[9] != 0x05 || header[10] || header[11])
        return false;
    std::uint16_t sum = le16(header + 4);
    for (std::size_t offset = 20; offset < 5176; offset += 2)
        sum += le16(header + offset);
    if (sum)
        return false;
    std::array<std::uint8_t, frame_size + 1> reference{};
    std::copy(frame.bytes.begin(), frame.bytes.end(), reference.begin() + 1);
    return nvt::parseRawStylusFrame(reference.data(), reference.size(), raw);
}

int scale_axis(int value, int from, int to)
{
    return (std::clamp(value, 0, from) * to + from / 2) / from;
}

class Pen {
public:
    Pen() : fd_(open("/dev/uinput", O_WRONLY | O_CLOEXEC)) {
        if (!fd_)
            sys_failure("opening uinput");
        set_ioctl(fd_.get(), UI_SET_PROPBIT, INPUT_PROP_DIRECT);
        set_ioctl(fd_.get(), UI_SET_EVBIT, EV_KEY);
        set_ioctl(fd_.get(), UI_SET_EVBIT, EV_ABS);
        set_ioctl(fd_.get(), UI_SET_KEYBIT, BTN_TOOL_PEN);
        set_ioctl(fd_.get(), UI_SET_KEYBIT, BTN_TOUCH);
        axis(ABS_X, 0, width, 135);
        axis(ABS_Y, 0, height, 135);
        axis(ABS_PRESSURE, 0, pressure_max, 0);
        axis(ABS_DISTANCE, 0, 1, 0);
        axis(ABS_TILT_X, -60, 60, 0);
        axis(ABS_TILT_Y, -60, 60, 0);
        uinput_setup setup{};
        setup.id.bustype = BUS_VIRTUAL;
        std::copy(input_name.begin(), input_name.end(), setup.name);
        set_ioctl(fd_.get(), UI_DEV_SETUP, &setup);
        set_ioctl(fd_.get(), UI_DEV_CREATE, 0);
        char syspath[32]{};
        if (ioctl(fd_.get(), UI_GET_SYSNAME(sizeof(syspath)), syspath) >= 0)
            std::cout << "UINPUT " << syspath << std::endl;
    }

    ~Pen() { ioctl(fd_.get(), UI_DEV_DESTROY); }

    void frame(const nvt::StylusCoordinates &point, int pressure) {
        const int x = scale_axis(point.tip_y, reference_width, width);
        const int y = height - scale_axis(point.tip_x, reference_height, height);
        const bool contact = pressure != 0;
        write_events({
            make_event(EV_KEY, BTN_TOOL_PEN, 1),
            make_event(EV_KEY, BTN_TOUCH, contact),
            make_event(EV_ABS, ABS_X, x),
            make_event(EV_ABS, ABS_Y, y),
            make_event(EV_ABS, ABS_PRESSURE, pressure),
            make_event(EV_ABS, ABS_DISTANCE, !contact),
            make_event(EV_ABS, ABS_TILT_X, std::clamp(point.tilt_x, -60, 60)),
            make_event(EV_ABS, ABS_TILT_Y, std::clamp(-point.tilt_y, -60, 60)),
            make_event(EV_SYN, SYN_REPORT, 0)
        });
        active_ = true;
        contact_ = contact;
    }

    void lift() {
        if (!contact_)
            return;
        write_events({
            make_event(EV_KEY, BTN_TOUCH, 0),
            make_event(EV_ABS, ABS_PRESSURE, 0),
            make_event(EV_ABS, ABS_DISTANCE, 1),
            make_event(EV_SYN, SYN_REPORT, 0)
        });
        contact_ = false;
    }

    void release() {
        if (!active_)
            return;
        write_events({
            make_event(EV_KEY, BTN_TOUCH, 0),
            make_event(EV_ABS, ABS_PRESSURE, 0),
            make_event(EV_ABS, ABS_DISTANCE, 1),
            make_event(EV_KEY, BTN_TOOL_PEN, 0),
            make_event(EV_SYN, SYN_REPORT, 0)
        });
        active_ = contact_ = false;
    }

private:
    void axis(int code, int min, int max, int resolution) {
        uinput_abs_setup setup{};
        setup.code = code;
        setup.absinfo.minimum = min;
        setup.absinfo.maximum = max;
        setup.absinfo.resolution = resolution;
        set_ioctl(fd_.get(), UI_SET_ABSBIT, code);
        set_ioctl(fd_.get(), UI_ABS_SETUP, &setup);
    }

    static input_event make_event(int type, int code, int value) {
        input_event event{};
        event.type = type;
        event.code = code;
        event.value = value;
        return event;
    }

    void write_events(std::initializer_list<input_event> items) {
        const auto amount = items.size() * sizeof(input_event);
        const auto written = write(fd_.get(), items.begin(), amount);
        if (written != static_cast<ssize_t>(amount)) {
            if (written >= 0)
                errno = EIO;
            sys_failure("writing pen frame");
        }
    }

    File fd_;
    bool active_ = false, contact_ = false;
};

bool find_pressure_device(File &hid)
{
    std::error_code error;
    for (const auto &node : std::filesystem::directory_iterator("/sys/class/hidraw", error)) {
        std::ifstream uevent(node.path() / "device/uevent");
        std::string line;
        while (std::getline(uevent, line)) {
            if (line != "HID_NAME=" + std::string(hid_name))
                continue;
            const auto device = "/dev/" / node.path().filename();
            hid.reset(open(device.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
            if (hid)
                return true;
            break;
        }
    }
    return false;
}

void quiet_pressure_evdev(File &grab)
{
    std::error_code error;
    for (const auto &node : std::filesystem::directory_iterator("/sys/class/input", error)) {
        if (!node.path().filename().string().starts_with("event"))
            continue;
        std::ifstream name(node.path() / "device/name");
        std::string value;
        if (!std::getline(name, value) || value != hid_name)
            continue;
        const auto device = "/dev/" / node.path().filename();
        grab.reset(open(device.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
        if (grab && ioctl(grab.get(), EVIOCGRAB, 1) < 0)
            grab.reset();
        break;
    }
}

int run()
{
    File frames, pressure_hid, pressure_grab;
    Pen pen;
    nvt::StylusDecoder decoder(nvt::StylusModel::M80p);
    Stamp last_active{}, next_scan{}, next_stream_open{};
    TipPressure pressure;
    std::size_t active_frames = 0, invalid_frames = 0, hid_reports = 0;
    bool reset_decoder = false;
    while (!stop) {
        auto now = Clock::now();
        if (!pressure_hid && now >= next_scan) {
            next_scan = now + 500ms;
            if (find_pressure_device(pressure_hid)) {
                quiet_pressure_evdev(pressure_grab);
                std::cout << "BLE_PRESSURE_READY" << std::endl;
            }
        }
        if (pressure_hid && !frames && now >= next_stream_open) {
            next_stream_open = now + 500ms;
            frames.reset(open("/dev/uke-focus-pen-frames", O_RDONLY | O_NONBLOCK | O_CLOEXEC));
        }
        pollfd poll_items[] = {{frames.get(), POLLIN, 0}, {pressure_hid.get(), POLLIN, 0}};
        auto deadline = now + 20ms;
        if (const auto tip_deadline = pressure.deadline())
            deadline = std::min(deadline, *tip_deadline);
        const auto remaining = std::max(deadline - Clock::now(), Clock::duration::zero());
        const auto wait_ms = std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
        if (poll(poll_items, 2, static_cast<int>(wait_ms)) < 0) {
            if (errno == EINTR)
                continue;
            sys_failure("polling pen devices");
        }
        if (pressure.expire(Clock::now()))
            pen.lift();
        if (poll_items[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            pressure_hid.reset();
            pressure_grab.reset();
            frames.reset();
            reset_decoder = true;
            pressure.sample(0, Clock::now());
            pen.release();
        } else if (poll_items[1].revents & POLLIN) {
            std::array<std::uint8_t, 64> bytes;
            for (;;) {
                auto size = read(pressure_hid.get(), bytes.data(), bytes.size());
                if (size <= 0)
                    break;
                if (size != 15 || bytes[0] != 5)
                    continue;
                const auto sample = std::min<int>(le16(bytes.data() + 1), pressure_max);
                if (pressure.sample(sample, Clock::now()))
                    pen.lift();
                ++hid_reports;
            }
        }
        if (frames && (poll_items[0].revents & (POLLERR | POLLHUP | POLLNVAL)))
            throw std::runtime_error("Novatek pen frame stream closed");
        if (frames && (poll_items[0].revents & POLLIN)) {
            for (int batch = 0; batch < 32; ++batch) {
                Frame frame{};
                const auto size = read(frames.get(), &frame, sizeof(frame));
                if (size < 0 && errno == EAGAIN)
                    break;
                if (size != sizeof(frame))
                    throw std::runtime_error("short Novatek pen frame");
                now = Clock::now();
                if (pressure.expire(now))
                    pen.lift();
                const Stamp timestamp{std::chrono::nanoseconds(frame.timestamp_ns)};
                nvt::RawStylusFrame raw;
                if (now - timestamp > 55ms || !decode_frame(frame, raw)) {
                    ++invalid_frames;
                    continue;
                }
                if (reset_decoder) {
                    decoder.reset();
                    reset_decoder = false;
                }
                const auto result = decoder.process(raw);
                if (!result.active)
                    continue;
                pen.frame(result.coordinates, pressure.for_frame(timestamp, now));
                last_active = now;
                ++active_frames;
            }
        }
        if (!reset_decoder && Clock::now() - last_active > 65ms) {
            pressure.sample(0, Clock::now());
            pen.release();
            reset_decoder = true;
        }
    }
    pen.release();
    std::cout << "BRIDGE_COUNTS active=" << active_frames << " invalid=" << invalid_frames
              << " ble_reports=" << hid_reports << std::endl;
    return 0;
}

int main()
{
    std::signal(SIGTERM, [](int) { stop = 1; });
    std::signal(SIGINT, [](int) { stop = 1; });
    try {
        return run();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
