// ============================================================================
// ax25send — Fire-and-forget APRS & UI frame sender for KISS TNC
//
// Modes:
//   --pos LAT,LON   APRS position report
//   --msg CALL      APRS message
//   --ui  DEST      Generic UI frame
//
// Usage:
//   ax25send -c PU1ABC /tmp/kiss --pos -23.55,-46.63 "Mobile"
//   ax25send -c PU1ABC /tmp/kiss --msg G2UGK "Hello"
//   ax25send -c PU1ABC /tmp/kiss --ui  CQ "Hello on packet"
// ============================================================================

#include "ax25lib.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <getopt.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <chrono>
#include <thread>

using namespace ax25;

// ─── TCP helper ─────────────────────────────────────────────────────────────

static bool is_tcp_address(const std::string& s) {
    // host:port or N.N.N.N:port
    auto colon = s.rfind(':');
    if (colon == std::string::npos || colon == 0) return false;
    for (size_t i = colon + 1; i < s.size(); ++i)
        if (!isdigit((unsigned char)s[i])) return false;
    return true;
}

static int tcp_connect(const std::string& addr) {
    auto colon = addr.rfind(':');
    std::string host = addr.substr(0, colon);
    int port = std::atoi(addr.substr(colon + 1).c_str());

    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);
    if (getaddrinfo(host.c_str(), port_str, &hints, &res) != 0 || !res) return -1;

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return -1; }
    if (::connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        close(fd);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);
    return fd;
}

// ─── Usage ──────────────────────────────────────────────────────────────────

static void usage(const char* prog) {
    fprintf(stderr,
        "Usage: %s -c CALL [options] DEVICE --pos LAT,LON [COMMENT]\n"
        "       %s -c CALL [options] DEVICE --msg CALL TEXT\n"
        "       %s -c CALL [options] DEVICE --ui  DEST PAYLOAD\n"
        "\n"
        "Modes (exactly one required):\n"
        "  --pos LAT,LON    Send APRS position report\n"
        "  --msg CALL       Send APRS message to CALL\n"
        "  --ui  DEST       Send generic UI frame to DEST\n"
        "\n"
        "Options:\n"
        "  -c CALL          Source callsign (required)\n"
        "  -b BAUD          Serial baud rate (default: 9600)\n"
        "  -p PATH          Digipeater path (e.g., WIDE1-1,WIDE2-1)\n"
        "  -d DEST          Override APRS dest (default: APRS)\n"
        "  -S TC            APRS symbol table+code (default: /> = car)\n"
        "  -n COUNT         Repeat N times (default: 1)\n"
        "  -i SECS          Interval between repeats (default: 30)\n"
        "  --pid 0xNN       Override PID byte (default: 0xF0)\n"
        "  --tnc            Send KISS TNC init commands first\n"
        "  -h, --help       Show this help\n"
        "\n"
        "DEVICE: serial port, PTY symlink (/tmp/kiss), or host:port for TCP\n"
        "\n"
        "Examples:\n"
        "  %s -c PU1ABC /tmp/kiss --pos -23.5505,-46.6333 \"Mobile station\"\n"
        "  %s -c PU1ABC /tmp/kiss --msg G2UGK \"Hello from bridge\"\n"
        "  %s -c PU1ABC /tmp/kiss --ui CQ \"Hello on packet radio\"\n"
        "  %s -c PU1ABC -p WIDE1-1,WIDE2-1 /tmp/kiss --pos -23.55,-46.63\n"
        "  %s -c PU1ABC /tmp/kiss --ui BEACON -n 5 -i 60 \"Net at 2100Z\"\n",
        prog, prog, prog, prog, prog, prog, prog, prog);
}

// ─── Parse digipeater path ──────────────────────────────────────────────────

static std::vector<Addr> parse_digis(const std::string& path) {
    std::vector<Addr> out;
    if (path.empty()) return out;
    std::string s = path;
    size_t pos;
    while ((pos = s.find(',')) != std::string::npos) {
        out.push_back(Addr::make(s.substr(0, pos)));
        s = s.substr(pos + 1);
    }
    if (!s.empty()) out.push_back(Addr::make(s));
    return out;
}

// ─── Build and send a UI frame ──────────────────────────────────────────────

static void send_frame(Kiss& kiss, const Addr& src, const Addr& dest,
                       const std::vector<Addr>& digis, uint8_t pid,
                       const std::string& info) {
    Frame f;
    f.dest = dest;
    f.src  = src;
    f.digis = digis;
    f.ctrl = 0x03;  // UI
    f.pid  = pid;
    f.has_pid = true;
    f.info.assign(info.begin(), info.end());

    auto encoded = f.encode();
    kiss.send_frame(encoded);
}

// ─── Main ───────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    // Config
    std::string mycall;
    std::string device;
    std::string digi_path;
    std::string aprs_dest = "APRS";
    char sym_table = '/';
    char sym_code  = '>';
    int  baud = 9600;
    int  repeat = 1;
    int  interval = 30;
    uint8_t pid = 0xF0;
    bool tnc_init = false;

    // Mode
    enum Mode { NONE, POS, MSG, UI } mode = NONE;
    std::string mode_arg;  // lat,lon for POS; dest call for MSG; dest for UI

    // Long options
    static struct option long_opts[] = {
        {"pos",   required_argument, nullptr, 'P'},
        {"msg",   required_argument, nullptr, 'M'},
        {"ui",    required_argument, nullptr, 'U'},
        {"pid",   required_argument, nullptr, 'x'},
        {"tnc",   no_argument,       nullptr, 'T'},
        {"help",  no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "c:b:p:d:S:n:i:h", long_opts, nullptr)) != -1) {
        switch (opt) {
        case 'c': mycall = optarg; break;
        case 'b': baud = std::atoi(optarg); break;
        case 'p': digi_path = optarg; break;
        case 'd': aprs_dest = optarg; break;
        case 'S':
            if (strlen(optarg) >= 2) { sym_table = optarg[0]; sym_code = optarg[1]; }
            else if (strlen(optarg) == 1) { sym_code = optarg[0]; }
            break;
        case 'n': repeat = std::max(1, std::atoi(optarg)); break;
        case 'i': interval = std::max(1, std::atoi(optarg)); break;
        case 'P': mode = POS; mode_arg = optarg; break;
        case 'M': mode = MSG; mode_arg = optarg; break;
        case 'U': mode = UI;  mode_arg = optarg; break;
        case 'x': pid = (uint8_t)strtol(optarg, nullptr, 0); break;
        case 'T': tnc_init = true; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    // Validate
    if (mycall.empty()) { fprintf(stderr, "Error: -c CALL is required.\n\n"); usage(argv[0]); return 1; }
    if (mode == NONE) { fprintf(stderr, "Error: specify --pos, --msg, or --ui.\n\n"); usage(argv[0]); return 1; }
    if (optind >= argc) { fprintf(stderr, "Error: DEVICE argument required.\n\n"); usage(argv[0]); return 1; }

    device = argv[optind++];

    // Remaining args joined as text/comment
    std::string text;
    for (int i = optind; i < argc; ++i) {
        if (!text.empty()) text += " ";
        text += argv[i];
    }

    // Parse digipeaters
    auto digis = parse_digis(digi_path);

    // Open KISS transport
    Kiss kiss;
    if (is_tcp_address(device)) {
        int fd = tcp_connect(device);
        if (fd < 0) {
            fprintf(stderr, "Error: cannot connect to %s\n", device.c_str());
            return 2;
        }
        kiss.open_fd(fd);
        fprintf(stderr, "Connected to %s (TCP)\n", device.c_str());
    } else {
        if (!kiss.open(device, baud)) {
            fprintf(stderr, "Error: cannot open %s at %d baud\n", device.c_str(), baud);
            return 2;
        }
        fprintf(stderr, "Opened %s at %d baud\n", device.c_str(), baud);
    }

    // Optional TNC init
    if (tnc_init) {
        fprintf(stderr, "Sending TNC init sequence...\n");
        const char* cmds[] = {"KISS ON\r", "RESTART\r", "INTERFACE KISS\r", "RESET\r"};
        for (const char* cmd : cmds) {
            ::write(kiss.fd(), cmd, strlen(cmd));
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
        // KISS parameter frames
        kiss.set_txdelay(400);
        kiss.set_persistence(63);
        kiss.set_slottime(100);
    }

    Addr src = Addr::make(mycall);

    // Send loop
    for (int i = 0; i < repeat; ++i) {
        if (i > 0) {
            fprintf(stderr, "  waiting %ds...\n", interval);
            std::this_thread::sleep_for(std::chrono::seconds(interval));
        }

        std::string info;
        Addr dest;

        switch (mode) {
        case POS: {
            // Parse lat,lon from mode_arg
            double lat = 0, lon = 0;
            auto comma = mode_arg.find(',');
            if (comma == std::string::npos) {
                fprintf(stderr, "Error: --pos requires LAT,LON (e.g., --pos -23.55,-46.63)\n");
                return 1;
            }
            lat = std::strtod(mode_arg.c_str(), nullptr);
            lon = std::strtod(mode_arg.c_str() + comma + 1, nullptr);
            info = aprs::make_pos(lat, lon, sym_code, sym_table, text);
            dest = Addr::make(aprs_dest);
            fprintf(stderr, "  [%d/%d] APRS POS: %s\n", i + 1, repeat, info.c_str());
            break;
        }
        case MSG: {
            info = aprs::make_msg(mode_arg, text);
            dest = Addr::make(aprs_dest);
            fprintf(stderr, "  [%d/%d] APRS MSG -> %s: %s\n", i + 1, repeat, mode_arg.c_str(), text.c_str());
            break;
        }
        case UI: {
            info = text;
            dest = Addr::make(mode_arg);
            fprintf(stderr, "  [%d/%d] UI -> %s: %s\n", i + 1, repeat, mode_arg.c_str(), text.c_str());
            break;
        }
        default: break;
        }

        send_frame(kiss, src, dest, digis, pid, info);
    }

    kiss.close();
    fprintf(stderr, "Done. Sent %d frame(s).\n", repeat);
    return 0;
}
