// PTT (Push-To-Talk) control for modemtnc
// Serial RTS/DTR and CM108 GPIO derived from Dire Wolf by John Langner, WB2OSZ (GPLv2)
// Original: https://github.com/wb2osz/direwolf
// Simplified for modemtnc: C++11, single channel, no global state
#include "ptt.h"
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <vector>

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
#include <termios.h>
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
//  Hex string parsing: "FEFE94E01C0001FD" → bytes
// ===========================================================================
static std::vector<uint8_t> hex_to_bytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        // Skip spaces
        while (i < hex.size() && hex[i] == ' ') i++;
        if (i + 1 >= hex.size()) break;
        char hi = hex[i], lo = hex[i + 1];
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int h = nibble(hi), l = nibble(lo);
        if (h >= 0 && l >= 0)
            bytes.push_back((uint8_t)((h << 4) | l));
    }
    return bytes;
}

// Build Icom CI-V PTT command
static std::vector<uint8_t> icom_ptt_cmd(int addr, bool tx) {
    // FE FE <addr> E0 1C 00 <01|00> FD
    return { 0xFE, 0xFE, (uint8_t)addr, 0xE0, 0x1C, 0x00, (uint8_t)(tx ? 0x01 : 0x00), 0xFD };
}

// Build Yaesu new CAT PTT command
static std::vector<uint8_t> yaesu_ptt_cmd(bool tx) {
    // "TX1;" or "TX0;"
    const char* cmd = tx ? "TX1;" : "TX0;";
    return std::vector<uint8_t>(cmd, cmd + 4);
}

// Build Kenwood PTT command
static std::vector<uint8_t> kenwood_ptt_cmd(bool tx) {
    // "TX;" or "RX;"
    const char* cmd = tx ? "TX;" : "RX;";
    return std::vector<uint8_t>(cmd, cmd + 3);
}

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
        case CAT:        return "CAT";
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

        case CAT: {
            if (!init_serial()) return false;
            const char* preset_name = "custom";
            if (cfg_.cat_preset == CAT_ICOM) preset_name = "Icom CI-V";
            else if (cfg_.cat_preset == CAT_YAESU) preset_name = "Yaesu CAT";
            else if (cfg_.cat_preset == CAT_KENWOOD) preset_name = "Kenwood";
            fprintf(stderr, "  PTT: CAT (%s) on %s @ %d baud%s\n",
                    preset_name, cfg_.device.c_str(), cfg_.cat_rate,
                    cfg_.invert ? " (inverted)" : "");
            // Configure serial port baud rate
            init_cat_serial();
            inited_ = true;
            return true;
        }

        case HAMLIB:
            fprintf(stderr, "  PTT: hamlib — use --ptt cat instead (direct CAT without hamlib)\n");
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

        case CAT:
            set_cat(tx);  // CAT uses logical tx, not hw_state (invert handled in command)
            break;

        case HAMLIB:
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
//  CAT — Direct serial CAT commands (no hamlib)
// ===========================================================================

void Controller::init_cat_serial() {
#if defined(__linux__) || defined(__APPLE__)
    if (fd_ < 0) return;

    struct termios tio;
    memset(&tio, 0, sizeof(tio));
    tcgetattr(fd_, &tio);

    // Raw mode — no echo, no canonical, no signals
    cfmakeraw(&tio);
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cflag &= ~CRTSCTS;   // No hardware flow control (we may use RTS for something else)

    // Baud rate
    speed_t spd = B19200;
    switch (cfg_.cat_rate) {
        case 4800:   spd = B4800;   break;
        case 9600:   spd = B9600;   break;
        case 19200:  spd = B19200;  break;
        case 38400:  spd = B38400;  break;
        case 57600:  spd = B57600;  break;
        case 115200: spd = B115200; break;
        default:     spd = B19200;  break;
    }
    cfsetispeed(&tio, spd);
    cfsetospeed(&tio, spd);

    // 8N1
    tio.c_cflag &= ~(CSIZE | PARENB | CSTOPB);
    tio.c_cflag |= CS8;

    tcsetattr(fd_, TCSANOW, &tio);
    tcflush(fd_, TCIOFLUSH);
#endif
}

void Controller::set_cat(bool tx) {
#if defined(__linux__) || defined(__APPLE__)
    if (fd_ < 0) return;

    std::vector<uint8_t> cmd;

    switch (cfg_.cat_preset) {
        case CAT_ICOM:
            cmd = icom_ptt_cmd(cfg_.cat_addr, tx);
            break;
        case CAT_YAESU:
            cmd = yaesu_ptt_cmd(tx);
            break;
        case CAT_KENWOOD:
            cmd = kenwood_ptt_cmd(tx);
            break;
        case CAT_CUSTOM:
            cmd = hex_to_bytes(tx ? cfg_.cat_tx_on : cfg_.cat_tx_off);
            break;
    }

    if (cmd.empty()) return;

    ssize_t n = ::write(fd_, cmd.data(), cmd.size());
    if (n != (ssize_t)cmd.size()) {
        fprintf(stderr, "[PTT] CAT write error: wrote %zd of %zu bytes: %s\n",
                n, cmd.size(), strerror(errno));
    }

    // Small delay for radio to process command
    usleep(50000);  // 50ms
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
