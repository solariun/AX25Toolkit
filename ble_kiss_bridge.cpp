// ble_kiss_bridge.cpp — BLE KISS TNC serial bridge + AX.25 monitor (C++17)
//
// Requires: SimpleBLE  →  make ble-deps
//
// Modes:
//   --scan                       Scan for nearby BLE devices
//   --inspect <ADDR>             List every service/characteristic
//   --device  <ADDR>             PTY serial bridge with AX.25 frame monitor
//                  --service / --write / --read   GATT UUIDs
//                  [--mtu <N>]                    Max chunk cap (default 517)
//                  [--write-with-response]        Force write-with-response
//
// Build:
//   make ble-deps        # clone + build SimpleBLE into vendor/simpleble
//   make ble_kiss_bridge

#ifdef __APPLE__
#  include <util.h>
#else
#  include <pty.h>
#endif

#include <simpleble/SimpleBLE.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <signal.h>
#include <sstream>
#include <string>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <sys/select.h>

// ─────────────────────────────────────────────────────────────────────────────
// Global state
// ─────────────────────────────────────────────────────────────────────────────
static std::atomic<bool> g_running{true};
static void sigint_handler(int) { g_running = false; }

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────
static std::string ts() {
    auto now = std::chrono::system_clock::now();
    auto t   = std::chrono::system_clock::to_time_t(now);
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                   now.time_since_epoch()) % 1000;
    std::ostringstream ss;
    struct tm tm_info{};
    localtime_r(&t, &tm_info);
    ss << std::put_time(&tm_info, "%H:%M:%S") << "."
       << std::setw(3) << std::setfill('0') << ms.count();
    return ss.str();
}

static std::string hexdump(const uint8_t* d, size_t n) {
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (size_t i = 0; i < n; i++) ss << std::setw(2) << (int)d[i];
    return ss.str();
}

static std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

static std::string hr(char c = '-', int n = 68) {
    return std::string(n, c);
}

// ─────────────────────────────────────────────────────────────────────────────
// KISS decoder
// ─────────────────────────────────────────────────────────────────────────────
static constexpr uint8_t FEND  = 0xC0;
static constexpr uint8_t FESC  = 0xDB;
static constexpr uint8_t TFEND = 0xDC;
static constexpr uint8_t TFESC = 0xDD;

struct KissFrame { int port, type; std::vector<uint8_t> payload; };

class KissDecoder {
    std::vector<uint8_t> buf_;
    bool in_ = false, esc_ = false;
public:
    std::vector<KissFrame> feed(const uint8_t* data, size_t len) {
        std::vector<KissFrame> frames;
        for (size_t i = 0; i < len; i++) {
            uint8_t b = data[i];
            if (b == FEND) {
                if (in_ && buf_.size() > 1) {
                    int cmd = buf_[0];
                    frames.push_back({(cmd >> 4) & 0xF, cmd & 0xF,
                                      {buf_.begin() + 1, buf_.end()}});
                }
                buf_.clear(); in_ = true; esc_ = false;
            } else if (!in_) {
            } else if (b == FESC) {
                esc_ = true;
            } else if (esc_) {
                esc_ = false;
                buf_.push_back(b == TFEND ? FEND : b == TFESC ? FESC : b);
            } else {
                buf_.push_back(b);
            }
        }
        return frames;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// AX.25 decoder (display only)
// ─────────────────────────────────────────────────────────────────────────────
struct Ax25Info {
    std::string dest, src, type, ctrl_hex, summary;
    std::vector<std::string> via;
    std::vector<uint8_t> info;
};

static std::pair<std::string, bool> decode_addr(const uint8_t* d, int off) {
    std::string call;
    for (int i = 0; i < 6; i++) {
        char c = (char)(d[off + i] >> 1);
        if (c != ' ') call += c;
    }
    uint8_t sb = d[off + 6];
    int ssid = (sb >> 1) & 0xF;
    if (ssid) call += "-" + std::to_string(ssid);
    return {call, (sb & 0x01) != 0};
}

static Ax25Info decode_ax25(const uint8_t* d, size_t n) {
    Ax25Info r;
    r.type = "?";
    if (n < 15) {
        r.type = "TRUNCATED";
        r.summary = "[too short: " + std::to_string(n) + " bytes]";
        return r;
    }
    auto [dest, _d]      = decode_addr(d, 0);
    auto [src, end_src]  = decode_addr(d, 7);
    r.dest = dest; r.src = src;

    int off = 14;
    while (!end_src && (size_t)(off + 7) <= n) {
        auto [rep, e] = decode_addr(d, off);
        r.via.push_back(rep);
        end_src = e;
        off += 7;
    }
    if ((size_t)off >= n) {
        r.type = "NO-CTRL";
        r.summary = src + " -> " + dest + "  [no control byte]";
        return r;
    }

    uint8_t ctrl = d[off];
    {
        std::ostringstream ss;
        ss << "0x" << std::hex << std::setw(2) << std::setfill('0') << (int)ctrl;
        r.ctrl_hex = ss.str();
    }
    bool pf = (ctrl & 0x10) != 0;

    if ((ctrl & 0x01) == 0) {                               // I-frame
        int ns = (ctrl >> 1) & 7, nr = (ctrl >> 5) & 7;
        r.type = "I";
        if ((size_t)(off + 2) < n) r.info = {d + off + 2, d + n};
        r.summary = src + " -> " + dest + "  [I(NS=" + std::to_string(ns)
                  + ",NR=" + std::to_string(nr) + (pf ? "P" : "") + ")]";
    } else if ((ctrl & 0x03) == 0x01) {                    // S-frame
        int nr = (ctrl >> 5) & 7;
        static constexpr std::pair<uint8_t, const char*> st[] =
            {{0x01,"RR"},{0x05,"RNR"},{0x09,"REJ"},{0x0D,"SREJ"}};
        const char* stype = "S?";
        for (auto& [v, n] : st) if ((ctrl & 0xF) == v) { stype = n; break; }
        r.type = stype;
        r.summary = src + " -> " + dest + "  [" + stype
                  + "(NR=" + std::to_string(nr) + (pf ? "P/F" : "") + ")]";
    } else {                                                 // U-frame
        uint8_t base = ctrl & ~0x10u;
        static constexpr std::pair<uint8_t, const char*> ut[] =
            {{0x2F,"SABM"},{0x43,"DISC"},{0x63,"UA"},{0x0F,"DM"},
             {0x87,"FRMR"},{0x03,"UI"}};
        const char* utype = nullptr;
        for (auto& [v, n] : ut)
            if (ctrl == v || ctrl == (uint8_t)(v | 0x10u) || base == v)
                { utype = n; break; }
        std::string ft;
        if (utype) { ft = utype; if (pf) ft += "(P/F)"; }
        else { std::ostringstream ss; ss << "U?0x" << std::hex << (int)ctrl; ft = ss.str(); }
        r.type = ft;
        std::string via_str;
        for (auto& v : r.via) via_str += " via " + v;
        r.summary = src + " -> " + dest + via_str + "  [" + ft + "]";
    }
    return r;
}

static void print_ax25(const Ax25Info& ax) {
    std::cout << "  +- " << ax.summary << "\n";
    std::cout << "  |  ctrl=" << ax.ctrl_hex << "  type=" << ax.type << "\n";
    if (!ax.via.empty()) {
        std::cout << "  |  via:";
        for (auto& v : ax.via) std::cout << " " << v;
        std::cout << "\n";
    }
    if (!ax.info.empty()) {
        std::string txt(ax.info.begin(), ax.info.end());
        for (auto& c : txt)
            if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7E) c = '.';
        std::cout << "  +- info (" << ax.info.size() << "b): \"" << txt << "\"\n";
    } else {
        std::cout << "  +-\n";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// PTY setup
// ─────────────────────────────────────────────────────────────────────────────
static bool open_pty(int& master, int& slave, std::string& path) {
    char name[256]{};
    if (openpty(&master, &slave, name, nullptr, nullptr) < 0) {
        std::cerr << "openpty: " << strerror(errno) << "\n";
        return false;
    }
    struct termios t{};
    tcgetattr(slave, &t);
    cfmakeraw(&t);
    tcsetattr(slave, TCSANOW, &t);

    int fl = fcntl(master, F_GETFL);
    fcntl(master, F_SETFL, fl | O_NONBLOCK);
    path = name;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// BLE adapter helpers
// ─────────────────────────────────────────────────────────────────────────────
static std::optional<SimpleBLE::Adapter> get_adapter() {
    if (!SimpleBLE::Adapter::bluetooth_enabled()) {
        std::cerr << "Bluetooth not enabled.\n";
        return {};
    }
    auto adapters = SimpleBLE::Adapter::get_adapters();
    if (adapters.empty()) {
        std::cerr << "No Bluetooth adapters found.\n";
        return {};
    }
    return adapters[0];
}

static std::optional<SimpleBLE::Peripheral>
find_peripheral(SimpleBLE::Adapter& adapter,
                const std::string& address,
                int timeout_ms)
{
    std::string target = lower(address);
    std::optional<SimpleBLE::Peripheral> found;
    std::mutex mx;
    std::atomic<bool> done{false};

    // Match by address OR by name (identifier) — case-insensitive
    auto check = [&](SimpleBLE::Peripheral p) {
        if (lower(p.address())    == target ||
            lower(p.identifier()) == target) {
            std::lock_guard<std::mutex> lk(mx);
            if (!found) { found = p; done = true; }
        }
    };

    // set_callback_on_scan_found  → fires for devices seen for the first time
    // set_callback_on_scan_updated → fires for devices already cached by the adapter
    // Both are needed: without _updated, a recently-seen device is never matched.
    adapter.set_callback_on_scan_found  ([&](SimpleBLE::Peripheral p) { check(p); });
    adapter.set_callback_on_scan_updated([&](SimpleBLE::Peripheral p) { check(p); });

    adapter.scan_start();
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(timeout_ms);
    while (!done && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    adapter.scan_stop();
    return found;
}

// ─────────────────────────────────────────────────────────────────────────────
// SCAN mode
// ─────────────────────────────────────────────────────────────────────────────
static void do_scan(double timeout_s) {
    auto opt = get_adapter();
    if (!opt) return;
    auto& adapter = *opt;

    struct Entry { SimpleBLE::Peripheral p; int rssi; };
    std::vector<Entry> found;
    std::mutex mx;

    adapter.set_callback_on_scan_found([&](SimpleBLE::Peripheral p) {
        std::lock_guard<std::mutex> lk(mx);
        std::string addr = lower(p.address());
        for (auto& e : found)
            if (lower(e.p.address()) == addr) return;
        found.push_back({p, p.rssi()});
    });

    std::cout << "Scanning for BLE devices (" << (int)timeout_s << "s)...\n\n";
    adapter.scan_for((int)(timeout_s * 1000));

    std::sort(found.begin(), found.end(),
              [](const Entry& a, const Entry& b){ return a.rssi > b.rssi; });

    for (auto& [p, rssi] : found) {
        std::cout << hr() << "\n";
        std::cout << "  Name   : " << (p.identifier().empty() ? "(no name)" : p.identifier()) << "\n";
        std::cout << "  Address: " << p.address() << "\n";
        std::cout << "  RSSI   : " << rssi << " dBm\n";
        auto svcs = p.services();
        if (!svcs.empty()) {
            std::cout << "  Services advertised:\n";
            for (auto& s : svcs) std::cout << "    " << s.uuid() << "\n";
        }
        std::cout << "\n";
    }
    std::cout << hr('=') << "\n";
    std::cout << "Found " << found.size() << " device(s).\n";
    std::cout << "\nNext step:\n  ble_kiss_bridge --inspect <ADDRESS>\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// INSPECT mode
// ─────────────────────────────────────────────────────────────────────────────
static void do_inspect(const std::string& address) {
    auto opt = get_adapter();
    if (!opt) return;
    auto& adapter = *opt;

    std::cout << "Searching for " << address << "...\n";
    auto popt = find_peripheral(adapter, address, 10000);
    if (!popt) { std::cerr << "Device not found.\n"; return; }
    auto& p = *popt;

    std::cout << "Connecting...\n";
    try { p.connect(); }
    catch (const std::exception& e) { std::cerr << "Connect failed: " << e.what() << "\n"; return; }

    std::cout << "Connected.  MTU=" << p.mtu() << "\n\n";

    for (auto& svc : p.services()) {
        std::cout << hr('=') << "\n";
        std::cout << "Service : " << svc.uuid() << "\n";
        for (auto& chr : svc.characteristics()) {
            std::cout << "\n  Characteristic: " << chr.uuid() << "\n";
            std::cout << "  Capabilities  : ";
            bool first = true;
            auto cap = [&](bool ok, const char* name) {
                if (!ok) return;
                if (!first) std::cout << " | ";
                std::cout << name;
                first = false;
            };
            cap(chr.can_read(),            "read");
            cap(chr.can_write_request(),   "write");
            cap(chr.can_write_command(),   "write-without-response");
            cap(chr.can_notify(),          "notify");
            cap(chr.can_indicate(),        "indicate");
            std::cout << "\n";
            if (chr.can_read()) {
                try {
                    auto val = p.read(svc.uuid(), chr.uuid());
                    std::cout << "  Value         : "
                              << hexdump(val.data(), val.size()) << "\n";
                } catch (...) {
                    std::cout << "  Value         : (read error)\n";
                }
            }
        }
    }
    std::cout << "\n" << hr('=') << "\n";
    std::cout << "\nBridge command:\n";
    std::cout << "  ble_kiss_bridge \\\n"
              << "      --device   " << address << " \\\n"
              << "      --service  <SERVICE-UUID> \\\n"
              << "      --write    <WRITE-CHAR-UUID> \\\n"
              << "      --read     <NOTIFY-CHAR-UUID>\n";
    p.disconnect();
}

// ─────────────────────────────────────────────────────────────────────────────
// BRIDGE (device) mode
// ─────────────────────────────────────────────────────────────────────────────
struct BridgeConfig {
    std::string address, service_uuid, write_uuid, read_uuid;
    int    mtu     = 517;
    double timeout = 10.0;
    std::optional<bool> force_response; // nullopt = auto-detect
};

static void do_bridge(const BridgeConfig& cfg) {
    int master_fd = -1, slave_fd = -1;
    std::string slave_path;
    if (!open_pty(master_fd, slave_fd, slave_path)) return;

    std::cout << hr('=') << "\n";
    std::cout << "  BLE KISS Serial Bridge + AX.25 Monitor\n";
    std::cout << hr('=') << "\n";
    std::cout << "  Device     : " << cfg.address << "\n";
    std::cout << "  Service    : " << cfg.service_uuid << "\n";
    std::cout << "  Read char  : " << cfg.read_uuid << "  (notify -> PTY)\n";
    std::cout << "  Write char : " << cfg.write_uuid << "  (PTY -> BLE)\n";
    std::cout << hr() << "\n";
    std::cout << "  Virtual serial port:\n\n      " << slave_path << "\n\n";
    std::cout << "  Example:\n      ax25client -c W1AW -r W1BBS-1 " << slave_path << "\n";
    std::cout << hr() << "\n  Connecting to BLE...\n";
    std::cout.flush();

    auto opt = get_adapter();
    if (!opt) { close(master_fd); close(slave_fd); return; }
    auto& adapter = *opt;

    auto popt = find_peripheral(adapter, cfg.address, (int)(cfg.timeout * 1000));
    if (!popt) {
        std::cerr << "Device " << cfg.address << " not found.\n";
        close(master_fd); close(slave_fd); return;
    }
    auto& peripheral = *popt;

    try { peripheral.connect(); }
    catch (const std::exception& e) {
        std::cerr << "Connect failed: " << e.what() << "\n";
        close(master_fd); close(slave_fd); return;
    }

    // Capability check on write characteristic
    bool can_wwr = false, can_wr = false;
    for (auto& svc : peripheral.services()) {
        if (lower(svc.uuid()) != lower(cfg.service_uuid)) continue;
        for (auto& chr : svc.characteristics()) {
            if (lower(chr.uuid()) != lower(cfg.write_uuid)) continue;
            can_wwr = chr.can_write_command();
            can_wr  = chr.can_write_request();
        }
    }

    bool use_response = cfg.force_response.has_value()
                      ? *cfg.force_response
                      : (!can_wwr && can_wr);  // prefer write-without-response

    uint16_t mtu_val = 23;
    try { mtu_val = peripheral.mtu(); } catch (...) {}
    if (mtu_val < 23) mtu_val = 23;

    int chunk_size = std::max(1, std::min(cfg.mtu, (int)mtu_val) - 3);

    std::cout << "  Connected.  MTU=" << mtu_val
              << "  chunk=" << chunk_size << "b"
              << "  wwr=" << (can_wwr ? "yes" : "no")
              << "  response=" << (use_response ? "yes" : "no") << "\n";
    std::cout << "  Monitoring traffic.  Ctrl-C to stop.\n\n";
    std::cout.flush();

    KissDecoder decoder;
    std::mutex mx;                        // protects decoder + stdout
    std::atomic<int> rx_frames{0}, tx_frames{0};

    // Disconnect callback
    peripheral.set_callback_on_disconnected([&]() {
        std::lock_guard<std::mutex> lk(mx);
        std::cout << "\n  [BLE disconnected]\n";
        std::cout.flush();
        g_running = false;
    });

    // ── BLE -> PTY ────────────────────────────────────────────────────────
    peripheral.notify(cfg.service_uuid, cfg.read_uuid,
        [&](SimpleBLE::ByteArray raw) {
            const uint8_t* d = raw.data();
            size_t n = raw.size();

            std::lock_guard<std::mutex> lk(mx);

            // Write raw bytes to PTY master
            if (write(master_fd, d, n) < 0) {
                std::cout << "  PTY write error: " << strerror(errno) << "\n";
            }
            ++rx_frames;

            std::string t = ts();
            auto frames = decoder.feed(d, n);

            std::cout << "\n" << hr() << "\n";
            std::cout << "[" << t << "]  <- BLE->PTY  " << n
                      << " bytes  raw: " << hexdump(d, n) << "\n";

            if (frames.empty()) {
                std::cout << "  (buffering - no complete KISS frame yet)\n";
            }
            for (auto& kf : frames) {
                const char* cmd_names[] = {"DATA","TXDELAY","P","SLOTTIME",
                                           "TXTAIL","FULLDUPLEX","SETHW","?","?",
                                           "?","?","?","?","?","?","RETURN"};
                std::cout << "  KISS  port=" << kf.port
                          << "  type=" << kf.type
                          << "(" << cmd_names[kf.type & 0xF] << ")\n";
                std::cout << "  AX25  payload (" << kf.payload.size() << "b): "
                          << hexdump(kf.payload.data(), kf.payload.size()) << "\n";
                if (kf.type == 0 && !kf.payload.empty()) {
                    auto ax = decode_ax25(kf.payload.data(), kf.payload.size());
                    print_ax25(ax);
                }
            }
            std::cout.flush();
        });

    // ── PTY -> BLE main loop ──────────────────────────────────────────────
    signal(SIGINT,  sigint_handler);
    signal(SIGTERM, sigint_handler);

    while (g_running && peripheral.is_connected()) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(master_fd, &rfds);
        struct timeval tv{0, 100000};  // 100 ms

        if (select(master_fd + 1, &rfds, nullptr, nullptr, &tv) <= 0)
            continue;

        uint8_t buf[4096];
        ssize_t nr = read(master_fd, buf, sizeof(buf));
        if (nr <= 0) continue;

        ++tx_frames;
        {
            std::lock_guard<std::mutex> lk(mx);
            std::cout << "\n" << hr() << "\n";
            std::cout << "[" << ts() << "]  -> PTY->BLE  " << nr
                      << " bytes  raw: " << hexdump(buf, nr) << "\n";
            std::cout.flush();
        }

        // Chunk and write to BLE
        try {
            for (int i = 0; i < (int)nr; i += chunk_size) {
                int len = std::min(chunk_size, (int)nr - i);
                SimpleBLE::ByteArray chunk(buf + i, buf + i + len);
                if (use_response)
                    peripheral.write_request(cfg.service_uuid, cfg.write_uuid, chunk);
                else
                    peripheral.write_command(cfg.service_uuid, cfg.write_uuid, chunk);
            }
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lk(mx);
            std::cout << "  BLE write error: " << e.what() << "\n";
            std::cout.flush();
        }
    }

    // ── Cleanup ───────────────────────────────────────────────────────────
    try { peripheral.unsubscribe(cfg.service_uuid, cfg.read_uuid); } catch (...) {}
    try { peripheral.disconnect(); } catch (...) {}
    close(master_fd);
    close(slave_fd);

    std::cout << "\n" << hr() << "\n";
    std::cout << "  Session ended.  RX frames: " << rx_frames.load()
              << "  TX frames: " << tx_frames.load() << "\n";
    std::cout << hr() << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
static void usage(const char* prog) {
    std::cerr <<
        "BLE KISS TNC serial bridge + AX.25 monitor\n\n"
        "Usage:\n"
        "  " << prog << " --scan [--timeout <s>]\n"
        "  " << prog << " --inspect <ADDRESS>\n"
        "  " << prog << " --device <ADDRESS>\n"
        "             --service <UUID> --write <UUID> --read <UUID>\n"
        "             [--mtu <bytes>] [--write-with-response]\n\n"
        "Examples:\n"
        "  " << prog << " --scan --timeout 15\n"
        "  " << prog << " --inspect AA:BB:CC:DD:EE:FF\n"
        "  " << prog << " --device AA:BB:CC:DD:EE:FF \\\n"
        "             --service 00000001-ba2a-46c9-ae49-01b0961f68bb \\\n"
        "             --write   00000003-ba2a-46c9-ae49-01b0961f68bb \\\n"
        "             --read    00000002-ba2a-46c9-ae49-01b0961f68bb\n\n"
        "Build:\n"
        "  make ble-deps        # clone + build SimpleBLE (one time)\n"
        "  make ble_kiss_bridge\n";
}

int main(int argc, char* argv[]) {
    std::string mode;
    BridgeConfig cfg;
    double timeout = 10.0;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if      (a == "--scan")    { mode = "scan"; }
        else if (a == "--inspect"           && i+1 < argc) { mode = "inspect"; cfg.address      = argv[++i]; }
        else if (a == "--device"            && i+1 < argc) { mode = "device";  cfg.address      = argv[++i]; }
        else if (a == "--service"           && i+1 < argc) { cfg.service_uuid = argv[++i]; }
        else if (a == "--write"             && i+1 < argc) { cfg.write_uuid   = argv[++i]; }
        else if (a == "--read"              && i+1 < argc) { cfg.read_uuid    = argv[++i]; }
        else if (a == "--mtu"               && i+1 < argc) { cfg.mtu          = std::stoi(argv[++i]); }
        else if (a == "--timeout"           && i+1 < argc) { timeout          = std::stod(argv[++i]); }
        else if (a == "--write-with-response")             { cfg.force_response = true; }
        else { std::cerr << "Unknown argument: " << a << "\n"; usage(argv[0]); return 1; }
    }

    cfg.timeout = timeout;

    if (mode.empty()) { usage(argv[0]); return 1; }
    if (mode == "device" &&
        (cfg.service_uuid.empty() || cfg.write_uuid.empty() || cfg.read_uuid.empty())) {
        std::cerr << "--device requires --service, --write, and --read\n";
        return 1;
    }

    if      (mode == "scan")    do_scan(timeout);
    else if (mode == "inspect") do_inspect(cfg.address);
    else                        do_bridge(cfg);
    return 0;
}
