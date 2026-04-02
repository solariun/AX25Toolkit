// PTT (Push-To-Talk) control for modemtnc
// Serial RTS/DTR and CM108 GPIO derived from Dire Wolf by John Langner, WB2OSZ (GPLv2)
// Original: https://github.com/wb2osz/direwolf
// Simplified for modemtnc: C++11, single channel, no global state
#include "ptt.h"
#include <cstdio>
#include <cstring>
#include <cerrno>

#ifdef __linux__
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <linux/hidraw.h>
#include <dirent.h>
#include <sys/stat.h>
#endif

#ifdef __APPLE__
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <IOKit/serial/ioss.h>
#endif

// Common POSIX serial line control
#if defined(__linux__) || defined(__APPLE__)
#include <sys/ioctl.h>
#ifndef TIOCM_RTS
#define TIOCM_RTS 0x004
#endif
#ifndef TIOCM_DTR
#define TIOCM_DTR 0x002
#endif
#endif

namespace ptt {

// ===========================================================================
//  Controller lifecycle
// ===========================================================================

Controller::~Controller() {
    if (inited_) close();
}

const char* Controller::method_name() const {
    switch (cfg_.method) {
        case VOX:        return "VOX";
        case SERIAL_RTS: return "Serial RTS";
        case SERIAL_DTR: return "Serial DTR";
        case CM108:      return "CM108 GPIO";
        case GPIO:       return "GPIO (sysfs)";
        case HAMLIB:     return "CAT (hamlib)";
        default:         return "unknown";
    }
}

bool Controller::init(const Config& cfg) {
    cfg_ = cfg;
    tx_state_ = false;

    switch (cfg_.method) {
        case VOX:
            // Nothing to initialize — audio triggers the radio
            fprintf(stderr, "  PTT: VOX (no hardware control)\n");
            inited_ = true;
            return true;

        case SERIAL_RTS:
        case SERIAL_DTR:
            if (!init_serial()) return false;
            fprintf(stderr, "  PTT: %s on %s%s\n", method_name(),
                    cfg_.device.c_str(), cfg_.invert ? " (inverted)" : "");
            inited_ = true;
            return true;

        case CM108:
            if (!init_cm108()) return false;
            fprintf(stderr, "  PTT: CM108 GPIO %d on %s%s\n",
                    cfg_.gpio_pin, cfg_.device.c_str(),
                    cfg_.invert ? " (inverted)" : "");
            inited_ = true;
            return true;

        case GPIO:
            if (!init_gpio()) return false;
            fprintf(stderr, "  PTT: GPIO %d (sysfs)%s\n",
                    cfg_.gpio_pin, cfg_.invert ? " (inverted)" : "");
            inited_ = true;
            return true;

        case HAMLIB:
            fprintf(stderr, "  PTT: CAT/hamlib — not yet implemented (use rigctld externally)\n");
            inited_ = true;
            return true;
    }
    return false;
}

void Controller::set(bool tx) {
    if (!inited_) return;
    bool hw_state = cfg_.invert ? !tx : tx;
    tx_state_ = tx;

    switch (cfg_.method) {
        case VOX:
            break;  // Nothing to do

        case SERIAL_RTS:
        case SERIAL_DTR:
            set_serial(hw_state);
            break;

        case CM108:
            set_cm108(hw_state);
            break;

        case GPIO:
            set_gpio(hw_state);
            break;

        case HAMLIB:
            // TODO: rig_set_ptt() via hamlib
            break;
    }
}

void Controller::close() {
    if (tx_state_) set(false);  // Release PTT before closing
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    inited_ = false;
}

// ===========================================================================
//  Serial RTS / DTR
// ===========================================================================

bool Controller::init_serial() {
#if defined(__linux__) || defined(__APPLE__)
    if (cfg_.device.empty()) {
        fprintf(stderr, "[PTT] Error: serial device path required for %s\n", method_name());
        return false;
    }

    fd_ = ::open(cfg_.device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd_ < 0) {
        fprintf(stderr, "[PTT] Error: cannot open %s: %s\n",
                cfg_.device.c_str(), strerror(errno));
        return false;
    }

    // Set initial state: PTT off
    set_serial(cfg_.invert ? true : false);
    return true;
#else
    fprintf(stderr, "[PTT] Serial PTT not supported on this platform\n");
    return false;
#endif
}

void Controller::set_serial(bool tx) {
#if defined(__linux__) || defined(__APPLE__)
    if (fd_ < 0) return;

    int modem_bits = 0;
    if (ioctl(fd_, TIOCMGET, &modem_bits) < 0) {
        fprintf(stderr, "[PTT] Error reading modem lines: %s\n", strerror(errno));
        return;
    }

    int target_bit = (cfg_.method == SERIAL_RTS) ? TIOCM_RTS : TIOCM_DTR;

    if (tx)
        modem_bits |= target_bit;
    else
        modem_bits &= ~target_bit;

    if (ioctl(fd_, TIOCMSET, &modem_bits) < 0) {
        fprintf(stderr, "[PTT] Error setting %s: %s\n", method_name(), strerror(errno));
    }
#else
    (void)tx;
#endif
}

// ===========================================================================
//  CM108 / CM119 USB HID GPIO
// ===========================================================================
//
// The CM108/CM119 USB audio chips (used in Digirig, cheap USB sound cards)
// have GPIO pins accessible via HID reports. The HID write format is:
//
//   Byte 0: 0x00  (report ID)
//   Byte 1: 0x00  (reserved)
//   Byte 2: iodata (GPIO output state: bit N-1 = pin N state)
//   Byte 3: iomask (GPIO direction: bit N-1 = 1 for output)
//   Byte 4: 0x00  (reserved)
//
// GPIO pin numbering: 1-8 (maps to CMedia physical pins 43,11,13,15,16,17,20,22)
// Most interfaces use GPIO 3 (CMedia pin 13 — the end pin, easy to solder)

bool Controller::init_cm108() {
#ifdef __linux__
    if (cfg_.device.empty()) {
        // Try auto-detect
        std::string found = cm108_find_hidraw();
        if (found.empty()) {
            fprintf(stderr, "[PTT] Error: CM108 device not specified and auto-detect failed\n");
            fprintf(stderr, "       Use --ptt-cm108 /dev/hidrawN or run --list-devices\n");
            return false;
        }
        cfg_.device = found;
        fprintf(stderr, "  [PTT] Auto-detected CM108: %s\n", cfg_.device.c_str());
    }

    if (cfg_.gpio_pin < 1 || cfg_.gpio_pin > 8) {
        fprintf(stderr, "[PTT] Error: CM108 GPIO pin must be 1-8, got %d\n", cfg_.gpio_pin);
        return false;
    }

    // Verify we can open the device
    int fd = ::open(cfg_.device.c_str(), O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "[PTT] Error: cannot open %s: %s\n",
                cfg_.device.c_str(), strerror(errno));
        if (errno == EACCES) {
            fprintf(stderr, "       Fix permissions:\n");
            fprintf(stderr, "         sudo chmod 666 %s\n", cfg_.device.c_str());
            fprintf(stderr, "       Or add udev rule:\n");
            fprintf(stderr, "         echo 'SUBSYSTEM==\"hidraw\", ATTRS{idVendor}==\"0d8c\", "
                            "GROUP=\"audio\", MODE=\"0660\"' | sudo tee /etc/udev/rules.d/99-cmedia.rules\n");
            fprintf(stderr, "         sudo udevadm control --reload-rules && sudo udevadm trigger\n");
        }
        return false;
    }
    ::close(fd);

    // Set initial state: PTT off
    set_cm108(cfg_.invert ? true : false);
    return true;
#else
    fprintf(stderr, "[PTT] CM108 GPIO only supported on Linux (use hidraw)\n");
    fprintf(stderr, "       On macOS, use VOX or Serial RTS/DTR instead\n");
    return false;
#endif
}

void Controller::set_cm108(bool tx) {
#ifdef __linux__
    int fd = ::open(cfg_.device.c_str(), O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "[PTT] CM108 write error: cannot open %s: %s\n",
                cfg_.device.c_str(), strerror(errno));
        return;
    }

    int pin = cfg_.gpio_pin;
    unsigned char io[5];
    io[0] = 0x00;                           // Report ID
    io[1] = 0x00;                           // Reserved
    io[2] = tx ? (1 << (pin - 1)) : 0;     // GPIO output data
    io[3] = (unsigned char)(1 << (pin - 1)); // GPIO direction mask (output)
    io[4] = 0x00;                           // Reserved

    ssize_t n = ::write(fd, io, sizeof(io));
    if (n != sizeof(io)) {
        fprintf(stderr, "[PTT] CM108 write error: wrote %zd of 5 bytes: %s\n",
                n, strerror(errno));
    }
    ::close(fd);
#else
    (void)tx;
#endif
}

// ===========================================================================
//  GPIO (Linux sysfs)
// ===========================================================================

bool Controller::init_gpio() {
#ifdef __linux__
    char path[128];

    // Export GPIO
    int fd = ::open("/sys/class/gpio/export", O_WRONLY);
    if (fd >= 0) {
        char num[16];
        int len = snprintf(num, sizeof(num), "%d", cfg_.gpio_pin);
        ::write(fd, num, len);
        ::close(fd);
        usleep(250000);  // Wait for udev
    }

    // Set direction to output
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", cfg_.gpio_pin);
    fd = ::open(path, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "[PTT] Error: cannot open %s: %s\n", path, strerror(errno));
        return false;
    }
    const char* dir = cfg_.invert ? "high" : "low";
    ::write(fd, dir, strlen(dir));
    ::close(fd);

    // Verify value file is accessible
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", cfg_.gpio_pin);
    fd = ::open(path, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "[PTT] Error: cannot open %s: %s\n", path, strerror(errno));
        return false;
    }
    ::close(fd);

    return true;
#else
    fprintf(stderr, "[PTT] GPIO sysfs only supported on Linux\n");
    return false;
#endif
}

void Controller::set_gpio(bool tx) {
#ifdef __linux__
    char path[128];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", cfg_.gpio_pin);
    int fd = ::open(path, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "[PTT] GPIO write error: %s\n", strerror(errno));
        return;
    }
    const char* val = tx ? "1" : "0";
    ::write(fd, val, 1);
    ::close(fd);
#else
    (void)tx;
#endif
}

// ===========================================================================
//  CM108 device discovery (Linux only)
// ===========================================================================

std::string cm108_find_hidraw(const std::string& /*audio_device*/) {
#ifdef __linux__
    // Scan /dev/hidraw* devices for CM108/CM119 chips
    DIR* dir = opendir("/dev");
    if (!dir) return "";

    std::string found;
    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "hidraw", 6) != 0) continue;

        char devpath[64];
        snprintf(devpath, sizeof(devpath), "/dev/%s", ent->d_name);

        int fd = ::open(devpath, O_RDONLY);
        if (fd < 0) continue;

        struct hidraw_devinfo info;
        memset(&info, 0, sizeof(info));
        if (ioctl(fd, HIDIOCGRAWINFO, &info) == 0) {
            // Check for CM108/CM119 vendor ID 0x0d8c
            bool is_cmedia = (info.vendor == 0x0d8c);
            // Also check for SSS chips (0x0c76) and AIOC (0x1209/0x7388)
            bool is_sss  = (info.vendor == 0x0c76);
            bool is_aioc = (info.vendor == 0x1209 && info.product == 0x7388);

            if (is_cmedia || is_sss || is_aioc) {
                if (found.empty()) found = devpath;
            }
        }
        ::close(fd);
    }
    closedir(dir);
    return found;
#else
    return "";
#endif
}

void cm108_list() {
#ifdef __linux__
    printf("CM108/CM119 USB audio devices with GPIO:\n\n");
    printf("%-15s  %-6s  %-6s  %s\n", "HID Device", "VID", "PID", "Info");
    printf("%-15s  %-6s  %-6s  %s\n", "---------------", "------", "------", "----");

    DIR* dir = opendir("/dev");
    if (!dir) { printf("Cannot scan /dev\n"); return; }

    int count = 0;
    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "hidraw", 6) != 0) continue;

        char devpath[64];
        snprintf(devpath, sizeof(devpath), "/dev/%s", ent->d_name);

        int fd = ::open(devpath, O_RDONLY);
        if (fd < 0) continue;

        struct hidraw_devinfo info;
        memset(&info, 0, sizeof(info));
        if (ioctl(fd, HIDIOCGRAWINFO, &info) == 0) {
            bool is_cmedia = (info.vendor == 0x0d8c);
            bool is_sss  = (info.vendor == 0x0c76);
            bool is_aioc = (info.vendor == 0x1209 && info.product == 0x7388);

            if (is_cmedia || is_sss || is_aioc) {
                const char* chip = "CM108/CM119";
                if (is_sss) chip = "SSS1621/1623";
                if (is_aioc) chip = "AIOC";

                printf("%-15s  %04x    %04x    %s\n",
                       devpath, (unsigned)info.vendor, (unsigned)info.product, chip);
                count++;
            }
        }
        ::close(fd);
    }
    closedir(dir);

    if (count == 0) {
        printf("  (none found)\n");
    }
    printf("\nUse: modemtnc --ptt cm108 --ptt-device /dev/hidrawN --ptt-gpio 3\n");
    printf("  (GPIO 3 is most common — CMedia pin 13)\n");
#else
    printf("CM108/CM119 device listing only supported on Linux\n");
#endif
}

} // namespace ptt
