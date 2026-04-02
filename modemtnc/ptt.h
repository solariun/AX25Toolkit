// PTT (Push-To-Talk) control for modemtnc
// Supports: VOX, Serial RTS/DTR, CM108/CM119 USB GPIO, GPIO (sysfs), CAT (hamlib)
// PTT methods derived from Dire Wolf by John Langner, WB2OSZ (GPLv2)
// Simplified for modemtnc: C++11 classes, single channel
#pragma once
#include <cstdint>
#include <string>

namespace ptt {

// ---------------------------------------------------------------------------
//  PTT method selection
// ---------------------------------------------------------------------------
enum Method {
    VOX,         // No hardware PTT — audio triggers radio's VOX or interface VOX
    SERIAL_RTS,  // Assert RTS on a serial port
    SERIAL_DTR,  // Assert DTR on a serial port
    CM108,       // GPIO pin on CM108/CM119 USB audio chip (Digirig, cheap USB cards)
    GPIO,        // Linux sysfs GPIO (/sys/class/gpio/gpioN/value)
    HAMLIB,      // CAT control via hamlib (Icom CI-V, Yaesu CAT, Kenwood, etc.)
};

// ---------------------------------------------------------------------------
//  PTT configuration
// ---------------------------------------------------------------------------
struct Config {
    Method      method      = VOX;
    std::string device;          // Serial port, HID device, or hamlib device path
    int         gpio_pin    = 3; // CM108 GPIO pin (1-8) or sysfs GPIO number
    bool        invert      = false;  // Invert PTT signal
    int         hamlib_model = -1;    // Hamlib rig model (-1 = auto, 2 = rigctld)
    int         hamlib_rate = 0;      // CAT serial baud rate (0 = default)
};

// ---------------------------------------------------------------------------
//  PTT controller
// ---------------------------------------------------------------------------
class Controller {
public:
    ~Controller();

    // Initialize PTT hardware. Returns true on success.
    bool init(const Config& cfg);

    // Set PTT state: true = transmit, false = receive
    void set(bool tx);

    // Current state
    bool is_transmitting() const { return tx_state_; }

    // Close and release resources
    void close();

    // Get description of current PTT method
    const char* method_name() const;

private:
    Config cfg_;
    int    fd_       = -1;    // Serial port or HID fd
    bool   tx_state_ = false;
    bool   inited_   = false;

    // Platform-specific implementations
    bool init_serial();
    void set_serial(bool tx);

    bool init_cm108();
    void set_cm108(bool tx);

    bool init_gpio();
    void set_gpio(bool tx);
};

// ---------------------------------------------------------------------------
//  CM108 device discovery
// ---------------------------------------------------------------------------
// Find the HID device path for a CM108/CM119 USB audio chip.
// If `audio_device` is provided, tries to match the USB sound card
// to its corresponding HID device.
// Returns empty string if not found.
std::string cm108_find_hidraw(const std::string& audio_device = "");

// List all CM108/CM119 devices found on the system
void cm108_list();

} // namespace ptt
