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
    CAT,         // Direct CAT commands over serial (no hamlib needed)
    HAMLIB,      // CAT control via hamlib (future — use CAT instead)
};

// ---------------------------------------------------------------------------
//  PTT configuration
// ---------------------------------------------------------------------------
// CAT radio presets
enum CatPreset {
    CAT_CUSTOM,   // User provides raw hex commands
    CAT_ICOM,     // Icom CI-V: FE FE <addr> E0 1C 00 <01|00> FD
    CAT_YAESU,    // Yaesu new CAT: "TX1;" / "TX0;"
    CAT_KENWOOD,  // Kenwood: "TX;" / "RX;"
};

struct Config {
    Method      method      = VOX;
    std::string device;          // Serial port, HID device, or CAT serial port
    int         gpio_pin    = 3; // CM108 GPIO pin (1-8) or sysfs GPIO number
    bool        invert      = false;  // Invert PTT signal
    int         hamlib_model = -1;    // Hamlib rig model (future)
    int         hamlib_rate = 0;      // Hamlib baud rate (future)

    // CAT settings
    CatPreset   cat_preset  = CAT_CUSTOM;
    int         cat_rate    = 19200;  // CAT serial baud rate
    int         cat_addr    = 0x94;   // Icom CI-V address (default: IC-7300)
    std::string cat_tx_on;            // Custom hex: "FEFE94E01C0001FD"
    std::string cat_tx_off;           // Custom hex: "FEFE94E01C0000FD"
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

    void init_cat_serial();
    void set_cat(bool tx);

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
