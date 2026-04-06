// kiss_modem — Software TNC with Soundcard DSP
// Demodulates/modulates AX.25 via soundcard, exposes KISS interface (PTY + TCP)
// DSP derived from Dire Wolf by John Langner, WB2OSZ (GPLv2)
// https://github.com/wb2osz/direwolf
//
// Usage:
//   kiss_modem -c W1AW -s 1200 -d default --link /tmp/kiss --monitor
//   kiss_modem -s 9600 --server-port 8001 --monitor
//   kiss_modem --loopback --monitor   (self-test: modulate → demodulate)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <getopt.h>
#include <string>
#include <vector>
#include <deque>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>

#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif

#include "audio.h"
#include "modem.h"
#include "hdlc.h"
#include "ptt.h"
#include "ax25lib.hpp"
#include "ax25dump.hpp"

// ---------------------------------------------------------------------------
//  Globals & config
// ---------------------------------------------------------------------------
static std::atomic<bool> g_running{true};

struct Config {
    std::string callsign;
    std::string audio_device;
    int         sample_rate   = 44100;
    int         baud          = 1200;
    modem::Type modem_type    = modem::AFSK_1200;
    std::string link_path     = "/tmp/kiss";
    int         server_port   = 0;
    bool        monitor       = false;
    bool        loopback      = false;
    int         txdelay       = 40;   // in 10ms units (40 = 400ms)
    int         txtail        = 10;   // in 10ms units (10 = 100ms)
    int         persist       = 63;
    int         slottime      = 10;   // in 10ms units (10 = 100ms)
    int         dwait         = 0;    // post-RX holdoff in ms (0 = auto: 1500ms @1200, 500ms @9600)
    int         volume        = 50;
    bool        list_devices  = false;
    bool        test_ptt      = false;
    std::string test_tx;
    int         debug_level   = 0;    // 0=off, 1=timing/TX/RX, 2=+queue/DCD/PTY, 3=+HDLC

    // PTT control
    ptt::Config ptt;
};

static void signal_handler(int) { g_running = false; }

// ---------------------------------------------------------------------------
//  Timestamp helper — used by --monitor and --debug
// ---------------------------------------------------------------------------
static const char* dbg_ts() {
    static thread_local char buf[32];
    auto now = std::chrono::system_clock::now();
    auto t   = std::chrono::system_clock::to_time_t(now);
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                   now.time_since_epoch()) % 1000;
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d",
             tm.tm_hour, tm.tm_min, tm.tm_sec, (int)ms.count());
    return buf;
}

// ---------------------------------------------------------------------------
//  PTY helpers (same pattern as bt_kiss_bridge)
// ---------------------------------------------------------------------------
static int open_pty(int* slave_fd, std::string& slave_name) {
    int master;
    char name[256];
    if (openpty(&master, slave_fd, name, nullptr, nullptr) < 0) {
        perror("openpty");
        return -1;
    }
    slave_name = name;
    // Set master non-blocking
    fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);
    return master;
}

static void create_symlink(const std::string& target, const std::string& link) {
    unlink(link.c_str());
    if (symlink(target.c_str(), link.c_str()) < 0)
        perror("symlink");
}

// ---------------------------------------------------------------------------
//  TCP server helpers
// ---------------------------------------------------------------------------
static int create_tcp_server(int port) {
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) { fd = socket(AF_INET, SOCK_STREAM, 0); }
    if (fd < 0) { perror("socket"); return -1; }
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(port);
    addr.sin6_addr = in6addr_any;
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(fd); return -1;
    }
    listen(fd, 4);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    return fd;
}

// ---------------------------------------------------------------------------
//  Queue supersession — replace queued S-frames for the same connection
// ---------------------------------------------------------------------------
// AX.25 S-frame (RR/REJ/RNR) control byte: bit0=1, bit1=0 → (ctrl & 0x03)==0x01
// Address block ends at the byte where bit0=1 (HDLC address extension bit).
static bool can_supersede(const std::vector<uint8_t>& queued,
                          const std::vector<uint8_t>& incoming)
{
    if (queued.size() < 15 || incoming.size() < 15) return false;

    // Find control byte offset: address fields are 7-byte blocks,
    // last block has bit0=1 on its final (SSID) byte.
    int q_ctrl = -1, i_ctrl = -1;
    for (size_t j = 6; j < queued.size() && j < 70; j += 7)
        if (queued[j] & 0x01) { q_ctrl = (int)j + 1; break; }
    for (size_t j = 6; j < incoming.size() && j < 70; j += 7)
        if (incoming[j] & 0x01) { i_ctrl = (int)j + 1; break; }

    if (q_ctrl < 0 || i_ctrl < 0 || q_ctrl != i_ctrl) return false;
    if ((size_t)q_ctrl >= queued.size() || (size_t)i_ctrl >= incoming.size()) return false;

    // Both must be S-frames
    if ((queued[q_ctrl] & 0x03) != 0x01) return false;
    if ((incoming[i_ctrl] & 0x03) != 0x01) return false;

    // Same address block → same connection + direction
    return memcmp(queued.data(), incoming.data(), q_ctrl) == 0;
}

// ---------------------------------------------------------------------------
//  Monitor display
// ---------------------------------------------------------------------------
static void show_frame(const uint8_t* data, size_t len, const char* direction) {
    std::vector<uint8_t> raw(data, data + len);
    ax25::Frame frame;
    if (ax25::Frame::decode(raw, frame))
        printf("[%s]  %s  %s\n", dbg_ts(), direction, frame.format().c_str());
    else
        printf("[%s]  %s  [%zu bytes, decode failed]\n", dbg_ts(), direction, len);
    printf("%s", hex_dump(data, len, "           ").c_str());
    fflush(stdout);
}

// ---------------------------------------------------------------------------
//  Modem type from baud rate
// ---------------------------------------------------------------------------
static modem::Type baud_to_type(int baud) {
    switch (baud) {
        case 300:  return modem::AFSK_300;
        case 1200: return modem::AFSK_1200;
        case 9600: return modem::GMSK_9600;
        default:   return modem::AFSK_1200;
    }
}

// ---------------------------------------------------------------------------
//  Usage
// ---------------------------------------------------------------------------
static void usage() {
    printf(
        "kiss_modem — Software TNC with Soundcard DSP\n"
        "DSP derived from Dire Wolf by John Langner, WB2OSZ (GPLv2)\n"
        "\n"
        "Usage: kiss_modem [options]\n"
        "\n"
        "Audio:\n"
        "  -d DEVICE         Audio device:\n"
        "                      macOS: device number from --list-devices or name substring\n"
        "                             e.g. -d 1  or  -d \"USB Audio Device\"\n"
        "                      Linux: ALSA device name\n"
        "                             e.g. -d default  or  -d plughw:1,0\n"
        "                      If omitted, uses system default\n"
        "  -r RATE           Sample rate in Hz (default: 44100, auto 96000 for 9600)\n"
        "  --list-devices    List all audio devices and exit\n"
        "\n"
        "Modem:\n"
        "  -s SPEED          Baud rate: 300, 1200, 9600 (default: 1200)\n"
        "  --volume N        TX amplitude 0-100 (default: 50)\n"
        "\n"
        "KISS interface:\n"
        "  --link PATH       PTY symlink path (default: /tmp/kiss)\n"
        "  --server-port N   TCP KISS server port (disabled by default)\n"
        "\n"
        "PTT control:\n"
        "  --ptt METHOD      PTT method (default: vox):\n"
        "                      vox       audio-triggered (no hardware control)\n"
        "                      rts, +rts assert RTS on serial port\n"
        "                      -rts      assert RTS inverted (active low)\n"
        "                      dtr, +dtr assert DTR on serial port\n"
        "                      -dtr      assert DTR inverted (active low)\n"
        "                      cm108     CM108/CM119 USB GPIO (Digirig)\n"
        "                      gpio      Linux sysfs GPIO\n"
        "                      cat       Custom CAT commands (hex)\n"
        "                      icom      Icom CI-V PTT (IC-7300, IC-7100, IC-9700...)\n"
        "                      yaesu     Yaesu CAT PTT (FT-991A, FT-710, FT-891...)\n"
        "                      kenwood   Kenwood PTT (TS-590, TS-890...)\n"
        "  --ptt-device DEV  Serial port or HID device for PTT\n"
        "                      rts/dtr: /dev/ttyUSB0, /dev/cu.usbserial-*\n"
        "                      cm108:   /dev/hidraw0 (auto-detected if omitted)\n"
        "  --ptt-gpio N      GPIO pin number (cm108: 1-8, default 3; gpio: sysfs num)\n"
        "  --ptt-invert      Invert PTT signal (active low)\n"
        "  --cat-rate N      CAT serial baud rate (default: 19200)\n"
        "  --cat-addr 0xNN   Icom CI-V address (default: 0x94 = IC-7300)\n"
        "  --cat-tx-on HEX  Custom TX ON command in hex (e.g. FEFE94E01C0001FD)\n"
        "  --cat-tx-off HEX Custom TX OFF command in hex\n"
        "\n"
        "TX timing (values in 10ms units, like KISS standard):\n"
        "  --txdelay N       Preamble delay (default: 40 = 400ms)\n"
        "  --txtail N        TX tail (default: 10 = 100ms)\n"
        "  --persist N       CSMA persistence 0-255 (default: 63)\n"
        "  --slottime N      CSMA slot time (default: 10 = 100ms)\n"
        "  --dwait N         Post-RX holdoff in ms (default: auto 1500@1200, 500@9600)\n"
        "\n"
        "Display:\n"
        "  -c CALL           Callsign (shown in monitor output)\n"
        "  --monitor         Print decoded frames to stdout\n"
        "\n"
        "Testing:\n"
        "  --loopback        Self-test: TX -> RX in memory (no audio device)\n"
        "  --test-ptt        Toggle PTT 3 times (1s on, 1s off) and exit\n"
        "  --test-tx TEXT    Send one UI frame (CALL>CQ TEXT) via audio and exit\n"
        "                      e.g. --test-tx \"Hello from kiss_modem\"\n"
        "  --debug N         Debug level 1-3 (1=timing/TX/RX, 2=+queue/DCD/PTY, 3=+HDLC)\n"
        "  -h, --help        Show this help\n"
        "\n"
        "Examples:\n"
        "  kiss_modem --list-devices                           # find audio devices\n"
        "  kiss_modem --loopback --monitor                     # self-test\n"
        "  kiss_modem --ptt cm108 -d plughw:1,0 --monitor     # Digirig\n"
        "  kiss_modem --ptt rts --ptt-device /dev/ttyUSB0 -d plughw:1,0 --monitor\n"
        "  kiss_modem --ptt icom --ptt-device /dev/ttyUSB0 -d plughw:2,0 --monitor\n"
        "  kiss_modem --ptt yaesu --ptt-device /dev/ttyUSB0 --cat-rate 38400 --monitor\n"
        "  kiss_modem --ptt cat --cat-tx-on FEFE94E01C0001FD --cat-tx-off FEFE94E01C0000FD ...\n"
        "\n"
        "Connect a KISS client:\n"
        "  ax25tnc -c W1AW -r W1BBS /tmp/kiss\n"
        "  ax25send -c W1AW /tmp/kiss --pos 42.36,-71.06 \"Mobile\"\n"
    );
}

// ---------------------------------------------------------------------------
//  Parse CLI
// ---------------------------------------------------------------------------
static Config parse_args(int argc, char* argv[]) {
    Config cfg;
    static struct option long_opts[] = {
        {"link",        required_argument, nullptr, 'L'},
        {"server-port", required_argument, nullptr, 'P'},
        {"monitor",     no_argument,       nullptr, 'M'},
        {"loopback",    no_argument,       nullptr, 'B'},
        {"list-devices",no_argument,       nullptr, 'D'},
        {"txdelay",     required_argument, nullptr, 1},
        {"txtail",      required_argument, nullptr, 2},
        {"persist",     required_argument, nullptr, 3},
        {"slottime",    required_argument, nullptr, 4},
        {"volume",      required_argument, nullptr, 5},
        {"ptt",         required_argument, nullptr, 6},
        {"ptt-device",  required_argument, nullptr, 7},
        {"ptt-gpio",    required_argument, nullptr, 8},
        {"ptt-invert",  no_argument,       nullptr, 9},
        {"hamlib-model", required_argument, nullptr, 10},
        {"hamlib-rate",  required_argument, nullptr, 11},
        {"cat-rate",     required_argument, nullptr, 12},
        {"cat-addr",     required_argument, nullptr, 13},
        {"cat-tx-on",    required_argument, nullptr, 14},
        {"cat-tx-off",   required_argument, nullptr, 15},
        {"test-ptt",     no_argument,       nullptr, 16},
        {"test-tx",      required_argument, nullptr, 17},
        {"dwait",        required_argument, nullptr, 19},
        {"debug",        required_argument, nullptr, 18},
        {"help",        no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "c:s:d:r:h", long_opts, nullptr)) != -1) {
        switch (c) {
            case 'c': cfg.callsign = optarg; break;
            case 's': cfg.baud = atoi(optarg); break;
            case 'd': cfg.audio_device = optarg; break;
            case 'r': cfg.sample_rate = atoi(optarg); break;
            case 'L': cfg.link_path = optarg; break;
            case 'P': cfg.server_port = atoi(optarg); break;
            case 'M': cfg.monitor = true; break;
            case 'B': cfg.loopback = true; break;
            case 'D': cfg.list_devices = true; break;
            case 1:   cfg.txdelay = atoi(optarg); break;
            case 2:   cfg.txtail = atoi(optarg); break;
            case 3:   cfg.persist = atoi(optarg); break;
            case 4:   cfg.slottime = atoi(optarg); break;
            case 5:   cfg.volume = atoi(optarg); break;
            case 6: { // --ptt METHOD
                std::string m = optarg;
                if (m == "vox")        cfg.ptt.method = ptt::VOX;
                else if (m == "rts"  || m == "+rts")  { cfg.ptt.method = ptt::SERIAL_RTS; cfg.ptt.invert = false; }
                else if (m == "-rts")                  { cfg.ptt.method = ptt::SERIAL_RTS; cfg.ptt.invert = true; }
                else if (m == "dtr"  || m == "+dtr")  { cfg.ptt.method = ptt::SERIAL_DTR; cfg.ptt.invert = false; }
                else if (m == "-dtr")                  { cfg.ptt.method = ptt::SERIAL_DTR; cfg.ptt.invert = true; }
                else if (m == "cm108") cfg.ptt.method = ptt::CM108;
                else if (m == "gpio") cfg.ptt.method = ptt::GPIO;
                else if (m == "cat")     { cfg.ptt.method = ptt::CAT; cfg.ptt.cat_preset = ptt::CAT_CUSTOM; }
                else if (m == "icom")    { cfg.ptt.method = ptt::CAT; cfg.ptt.cat_preset = ptt::CAT_ICOM; }
                else if (m == "yaesu")   { cfg.ptt.method = ptt::CAT; cfg.ptt.cat_preset = ptt::CAT_YAESU; }
                else if (m == "kenwood") { cfg.ptt.method = ptt::CAT; cfg.ptt.cat_preset = ptt::CAT_KENWOOD; }
                else if (m == "hamlib")  cfg.ptt.method = ptt::HAMLIB;
                else { fprintf(stderr, "Unknown PTT method: %s\n", optarg); exit(1); }
                break;
            }
            case 7:   cfg.ptt.device = optarg; break;
            case 8:   cfg.ptt.gpio_pin = atoi(optarg); break;
            case 9:   cfg.ptt.invert = true; break;
            case 10:  cfg.ptt.hamlib_model = atoi(optarg); break;
            case 11:  cfg.ptt.hamlib_rate = atoi(optarg); break;
            case 12:  cfg.ptt.cat_rate = atoi(optarg); break;
            case 13:  cfg.ptt.cat_addr = (int)strtol(optarg, NULL, 0); break;
            case 14:  cfg.ptt.cat_tx_on = optarg; break;
            case 15:  cfg.ptt.cat_tx_off = optarg; break;
            case 16:  cfg.test_ptt = true; break;
            case 17:  cfg.test_tx = optarg; break;
            case 18: {
                int lvl = atoi(optarg);
                cfg.debug_level = (lvl >= 1 && lvl <= 3) ? lvl : 1;
                break;
            }
            case 19:  cfg.dwait = atoi(optarg); break;
            case 'h': usage(); exit(0);
            default:  usage(); exit(1);
        }
    }

    cfg.modem_type = baud_to_type(cfg.baud);

    // Auto-select sample rate for 9600 baud if user didn't specify
    if (cfg.baud >= 9600 && cfg.sample_rate == 44100)
        cfg.sample_rate = 96000;

    // Auto DWAIT: allow remote to finish a window burst before we respond
    if (cfg.dwait == 0)
        cfg.dwait = (cfg.baud <= 1200) ? 1500 : 500;

    return cfg;
}

// ---------------------------------------------------------------------------
//  KISS parameter handling
// ---------------------------------------------------------------------------
struct KissParams {
    int txdelay  = 40;   // in 10ms units (KISS standard)
    int persist  = 63;
    int slottime = 10;   // in 10ms units
    int txtail   = 10;   // in 10ms units
    bool fullduplex = false;
};

// ---------------------------------------------------------------------------
//  Loopback self-test
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
//  Test PTT — toggle 3 times
// ---------------------------------------------------------------------------
static void run_test_ptt(const Config& cfg) {
    printf("=== PTT Test ===\n");
    ptt::Controller ptt_ctl;
    if (!ptt_ctl.init(cfg.ptt)) {
        fprintf(stderr, "Failed to initialize PTT\n");
        return;
    }
    for (int i = 1; i <= 3 && g_running; i++) {
        printf("  [%d/3] PTT ON  ...", i); fflush(stdout);
        ptt_ctl.set(true);
        usleep(1000000);  // 1s
        printf(" OFF\n");
        ptt_ctl.set(false);
        if (i < 3) usleep(1000000);  // 1s pause between
    }
    ptt_ctl.close();
    printf("PTT test done.\n");
}

// ---------------------------------------------------------------------------
//  Test TX — send one UI frame via audio and exit
// ---------------------------------------------------------------------------
static void run_test_tx(const Config& cfg) {
    printf("=== TX Test ===\n");

    // Open audio
    AudioDevice* audio = AudioDevice::create();
    int amp = 16000 * cfg.volume / 100;
    if (!audio->open(cfg.audio_device.c_str(), cfg.sample_rate, false, true)) {
        fprintf(stderr, "Failed to open audio device for playback\n");
        return;
    }

    // Init PTT
    ptt::Controller ptt_ctl;
    if (!ptt_ctl.init(cfg.ptt)) {
        fprintf(stderr, "Failed to initialize PTT\n");
        delete audio;
        return;
    }

    // Init modem
    modem::Modulator modulator;
    hdlc::Encoder hdlc_enc;
    modulator.init(cfg.modem_type, cfg.sample_rate, amp);

    std::vector<int16_t> tx_audio;
    modulator.set_on_sample([&tx_audio](int16_t s) { tx_audio.push_back(s); });
    hdlc_enc.set_on_bit([&modulator](int bit) { modulator.put_bit(bit); });

    // Build UI frame: CALL>CQ text
    ax25::Frame f;
    f.dest = ax25::Addr::make("CQ");
    f.src  = ax25::Addr::make(cfg.callsign.empty() ? "TEST" : cfg.callsign.c_str());
    f.ctrl = 0x03;  // UI
    f.pid  = 0xF0;
    f.info.assign(cfg.test_tx.begin(), cfg.test_tx.end());
    auto raw = f.encode();

    int txdelay_ms = cfg.txdelay * 10;   // 10ms units → ms
    int txtail_ms  = cfg.txtail * 10;
    int flags = txdelay_ms * cfg.baud / (8 * 1000);
    if (flags < 5) flags = 5;

    printf("  TX: %s > CQ [UI] \"%s\"\n", f.src.str().c_str(), cfg.test_tx.c_str());
    printf("  Modem: %d baud, %d Hz, txdelay=%dms, txtail=%dms\n",
           cfg.baud, cfg.sample_rate, txdelay_ms, txtail_ms);

    // Encode
    hdlc_enc.send_frame(raw.data(), raw.size(), flags, 2);
    modulator.put_quiet_ms(txtail_ms);

    printf("  Audio: %zu samples (%.1f ms)\n",
           tx_audio.size(), 1000.0 * tx_audio.size() / cfg.sample_rate);

    // PTT ON → send audio → PTT OFF
    printf("  PTT ON\n");
    ptt_ctl.set(true);

    size_t off = 0;
    while (off < tx_audio.size()) {
        int chunk = std::min((int)(tx_audio.size() - off), 1024);
        int written = audio->write(tx_audio.data() + off, chunk);
        if (written > 0) off += written;
        else break;
    }
    audio->flush();
    audio->wait_drain();  // block until all audio played

    printf("  PTT OFF\n");
    ptt_ctl.set(false);

    ptt_ctl.close();
    audio->close();
    delete audio;
    printf("TX test done.\n");
}

// ---------------------------------------------------------------------------
//  Loopback self-test
// ---------------------------------------------------------------------------
static void run_loopback(const Config& cfg) {
    printf("=== kiss_modem loopback self-test ===\n");
    printf("Modem: %d baud, sample rate: %d\n\n", cfg.baud, cfg.sample_rate);

    modem::Modulator mod;
    modem::Demodulator demod;
    hdlc::Encoder enc;
    hdlc::Decoder dec;

    int amp = 16000 * cfg.volume / 100;
    mod.init(cfg.modem_type, cfg.sample_rate, amp);
    demod.init(cfg.modem_type, cfg.sample_rate);
    dec.init();

    int frames_decoded = 0;
    dec.set_on_frame([&](const uint8_t* data, size_t len) {
        frames_decoded++;
        printf("  [RX] Frame %d: %zu bytes\n", frames_decoded, len);
        if (cfg.monitor) show_frame(data, len, "<- LOOPBACK");
    });

    // Wire: modulator → demodulator
    demod.set_on_bit([&dec](int bit) { dec.receive_bit(bit); });

    std::vector<int16_t> audio_buf;
    mod.set_on_sample([&](int16_t s) {
        audio_buf.push_back(s);
        demod.process_sample(s);
    });

    // HDLC encoder → modulator
    enc.set_on_bit([&mod](int bit) { mod.put_bit(bit); });

    // Build a test AX.25 UI frame
    ax25::Frame f;
    f.dest = ax25::Addr::make("CQ");
    f.src  = ax25::Addr::make(cfg.callsign.empty() ? "TEST" : cfg.callsign.c_str());
    f.ctrl = 0x03;  // UI
    f.pid  = 0xF0;
    const char* msg = "kiss_modem loopback test 1234567890";
    f.info.assign(msg, msg + strlen(msg));
    auto raw = f.encode();

    printf("TX: %s -> %s [UI] \"%s\"\n", f.src.str().c_str(), f.dest.str().c_str(), msg);

    // Calculate preamble flags for txdelay (cfg.txdelay is in 10ms units)
    int flags = (cfg.txdelay * 10) * cfg.baud / (8 * 1000);
    if (flags < 5) flags = 5;

    // Encode and modulate
    enc.send_frame(raw.data(), raw.size(), flags, 2);

    // Add some trailing silence for the demodulator to flush
    mod.put_quiet_ms(100);

    printf("\nGenerated %zu audio samples (%.1f ms)\n",
           audio_buf.size(), 1000.0 * audio_buf.size() / cfg.sample_rate);
    printf("Frames decoded: %d\n", frames_decoded);
    printf("Result: %s\n", frames_decoded > 0 ? "PASS" : "FAIL");
}

// ---------------------------------------------------------------------------
//  Main bridge loop
// ---------------------------------------------------------------------------
static void run_bridge(const Config& cfg) {
    // Open PTY
    int slave_fd;
    std::string slave_name;
    int pty_master = open_pty(&slave_fd, slave_name);
    if (pty_master < 0) { fprintf(stderr, "Failed to open PTY\n"); exit(1); }
    // Keep slave_fd open — on macOS, closing the last slave fd
    // causes the PTY master to return EIO on subsequent reads.
    // Clients (ax25send, ax25tnc) open/close the slave independently.

    create_symlink(slave_name, cfg.link_path);

    // Open TCP server (optional)
    int tcp_srv = -1;
    std::vector<int> tcp_clients;
    if (cfg.server_port > 0)
        tcp_srv = create_tcp_server(cfg.server_port);

    // Open audio device
    AudioDevice* audio = AudioDevice::create();
    int amp = 16000 * cfg.volume / 100;
    if (!audio->open(cfg.audio_device.c_str(), cfg.sample_rate, true, true)) {
        fprintf(stderr, "Failed to open audio device\n");
        exit(1);
    }

    printf("====================================================================\n");
    printf("  kiss_modem — Software TNC with Soundcard DSP\n");
    printf("====================================================================\n");
    printf("  Modem      : %d baud\n", cfg.baud);
    printf("  Audio      : %s @ %d Hz\n",
           cfg.audio_device.empty() ? "default" : cfg.audio_device.c_str(), cfg.sample_rate);
    printf("--------------------------------------------------------------------\n");
    printf("  PTY device : %s\n", slave_name.c_str());
    printf("  Symlink    : %s  -> %s\n", cfg.link_path.c_str(), slave_name.c_str());
    if (cfg.server_port > 0)
        printf("  TCP server : port %d\n", cfg.server_port);
    printf("\n  Example:\n      ax25tnc -c W1AW -r W1BBS-1 %s\n", cfg.link_path.c_str());
    printf("--------------------------------------------------------------------\n");
    // Init PTT
    ptt::Controller ptt_ctl;
    if (!ptt_ctl.init(cfg.ptt)) {
        fprintf(stderr, "Failed to initialize PTT — exiting\n");
        exit(1);
    }

    // Use the actual sample rate from the audio device (may differ from requested)
    int actual_rate = audio->sample_rate();
    if (actual_rate != cfg.sample_rate)
        fprintf(stderr, "  Note: using device sample rate %d Hz (requested %d)\n",
                actual_rate, cfg.sample_rate);

    if (cfg.monitor) printf("  Monitor on.  Ctrl-C to stop.\n\n");

    // Init modem with ACTUAL audio sample rate
    modem::Demodulator demod;
    modem::Modulator modulator;
    hdlc::Decoder hdlc_dec;
    hdlc::Encoder hdlc_enc;

    demod.init(cfg.modem_type, actual_rate);
    modulator.init(cfg.modem_type, actual_rate, amp);
    hdlc_dec.init();
    hdlc_dec.set_debug(cfg.debug_level);

    // KISS decoder for host → radio TX path
    ax25::kiss::Decoder kiss_dec;

    // ── Half-duplex RX mute ────────────────────────────────────────────────
    // Standard TNC behavior: suppress RX during TX.
    // Without this, the demodulator decodes our own transmitted audio and
    // echoes it back to the host — corrupting the AX.25 state machine.
    std::atomic<bool> tx_active{false};

    // Last RX timestamp — used by DWAIT to avoid keying up mid-burst
    auto now_ms = []() -> long long {
        auto tp = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            tp.time_since_epoch()).count();
    };
    std::atomic<long long> last_rx_ts{0};

    // Wire RX: demod bit → HDLC → frame → KISS encode → PTY/TCP
    demod.set_on_bit([&hdlc_dec](int bit) { hdlc_dec.receive_bit(bit); });

    hdlc_dec.set_on_frame([&](const uint8_t* data, size_t len) {
        // Suppress self-echo: drop any frame decoded while TX is active
        if (tx_active.load(std::memory_order_acquire)) {
            if (cfg.debug_level >= 1) {
                std::vector<uint8_t> raw(data, data + len);
                ax25::Frame ef;
                if (ax25::Frame::decode(raw, ef))
                    fprintf(stderr, "[%s]  [RX] dropped echo: %s\n", dbg_ts(), ef.format().c_str());
                else
                    fprintf(stderr, "[%s]  [RX] dropped echo (%zu bytes, decode failed)\n", dbg_ts(), len);
                if (cfg.debug_level >= 2)
                    fprintf(stderr, "%s", hex_dump(data, len, "           ").c_str());
            }
            return;
        }

        // Stamp last RX time for DWAIT
        last_rx_ts.store(now_ms(), std::memory_order_release);

        if (cfg.monitor) show_frame(data, len, "<- AIR");

        // KISS-wrap and send to PTY + TCP clients
        auto kissed = ax25::kiss::encode(std::vector<uint8_t>(data, data + len));
        ::write(pty_master, kissed.data(), kissed.size());
        int tcp_count = 0;
        for (int fd : tcp_clients) {
            ::write(fd, kissed.data(), kissed.size());
            tcp_count++;
        }
        if (cfg.debug_level >= 1)
            fprintf(stderr, "[%s]  [RX] -> host: %zu bytes (pty+%d tcp)\n",
                    dbg_ts(), len, tcp_count);
    });

    // KISS params
    KissParams kp;
    kp.txdelay  = cfg.txdelay;   // already in 10ms units
    kp.persist  = cfg.persist;
    kp.slottime = cfg.slottime;
    kp.txtail   = cfg.txtail;

    // ── TX queue + thread ──────────────────────────────────────────────────
    // State machine per frame:
    //   WAIT_WORK → DWAIT → DCD_WAIT → SETTLE → CSMA → DEQUEUE →
    //   MODULATE → PTT ON → write → drain → PTT OFF → COOLDOWN → loop
    //
    // DWAIT prevents keying up in gaps between I-frames in a window burst.
    // CSMA uses exponential backoff on DCD collisions.
    // Queue supersession replaces queued S-frames for the same connection.
    //
    std::mutex tx_queue_mtx;
    std::condition_variable tx_queue_cv;
    std::deque<std::vector<uint8_t>> tx_queue;

    // Enqueue with S-frame supersession: returns queue depth after enqueue
    auto enqueue_frame = [&](const std::vector<uint8_t>& data) -> size_t {
        std::lock_guard<std::mutex> lk(tx_queue_mtx);
        for (auto& queued : tx_queue) {
            if (can_supersede(queued, data)) {
                if (cfg.debug_level >= 2)
                    fprintf(stderr, "[%s]  [QUEUE] supersede S-frame (%zu->%zu bytes)\n",
                            dbg_ts(), queued.size(), data.size());
                queued = data;
                return tx_queue.size();
            }
        }
        tx_queue.push_back(data);
        return tx_queue.size();
    };

    std::thread tx_thread([&]() {
        srand((unsigned)time(nullptr));  // seed CSMA p-persistence RNG

        std::vector<int16_t> tx_audio;
        modulator.set_on_sample([&tx_audio](int16_t s) { tx_audio.push_back(s); });
        hdlc_enc.set_on_bit([&modulator](int bit) { modulator.put_bit(bit); });

        while (g_running) {
            // ── WAIT_WORK: block until at least one frame is queued ──
            // cv.wait predicate returns immediately if queue is non-empty,
            // so the cooldown loop-back never blocks here unnecessarily.
            {
                std::unique_lock<std::mutex> lk(tx_queue_mtx);
                tx_queue_cv.wait(lk, [&] { return !tx_queue.empty() || !g_running; });
                if (!g_running) break;
            }

            // ── CSMA/CA: DWAIT + p-persistence + exponential backoff ──
            //
            // 1. DWAIT: if a frame was received recently, wait until dwait ms
            //    have elapsed since the last RX — prevents keying up in the
            //    gap between back-to-back I-frames in a window burst.
            // 2. DCD_WAIT: poll DCD every 10ms until channel clear.
            // 3. SETTLE: 20ms squelch tail guard.
            // 4. CSMA: p-persistence slots.  DCD during slot → restart from 2.
            //    Each collision (DCD restart) halves effective persist (backoff).
            //    Backoff resets to base persist on successful TX.
            //
            if (!kp.fullduplex) {
                int collision_count = 0;

            csma_restart:
                // ── DWAIT: post-RX holdoff ──
                {
                    long long rx_ts = last_rx_ts.load(std::memory_order_acquire);
                    if (rx_ts > 0) {
                        long long elapsed = now_ms() - rx_ts;
                        if (elapsed < cfg.dwait) {
                            long long remain = cfg.dwait - elapsed;
                            if (cfg.debug_level >= 2)
                                fprintf(stderr, "[%s]  [DWAIT] %lld ms remaining\n",
                                        dbg_ts(), remain);
                            usleep((unsigned)(remain * 1000));
                            if (!g_running) break;
                        }
                    }
                }

                // ── DCD_WAIT ──
                if (cfg.debug_level >= 2 && demod.dcd())
                    fprintf(stderr, "[%s]  [DCD] waiting...\n", dbg_ts());
                while (g_running && demod.dcd())
                    usleep(10000);
                if (!g_running) break;
                if (cfg.debug_level >= 2)
                    fprintf(stderr, "[%s]  [DCD] clear\n", dbg_ts());

                // ── SETTLE: squelch tail guard ──
                usleep(20000);
                if (demod.dcd()) goto csma_restart;

                // ── P-persistence with exponential backoff ──
                {
                    int eff_persist = cfg.persist >> std::min(collision_count, 3);
                    for (;;) {
                        usleep(cfg.slottime * 10000);
                        if (!g_running) break;
                        if (demod.dcd()) {
                            collision_count++;
                            if (cfg.debug_level >= 2)
                                fprintf(stderr, "[%s]  [CSMA] DCD during slot, backoff=%d, restart\n",
                                        dbg_ts(), collision_count);
                            goto csma_restart;
                        }
                        if ((rand() & 0xFF) <= eff_persist) {
                            if (cfg.debug_level >= 2)
                                fprintf(stderr, "[%s]  [CSMA] transmit (persist=%d eff=%d backoff=%d)\n",
                                        dbg_ts(), cfg.persist, eff_persist, collision_count);
                            collision_count = 0;  // reset on successful transmit
                            break;
                        }
                    }
                }
            }

            // ── DEQUEUE: pop one frame ──
            std::vector<uint8_t> frame;
            size_t depth;
            {
                std::lock_guard<std::mutex> lk(tx_queue_mtx);
                if (tx_queue.empty()) continue;
                frame = std::move(tx_queue.front());
                tx_queue.pop_front();
                depth = tx_queue.size();
            }
            if (cfg.debug_level >= 2)
                fprintf(stderr, "[%s]  [QUEUE] dequeue %zu bytes, remaining=%zu\n",
                        dbg_ts(), frame.size(), depth);

            // ── MODULATE: preamble + frame + tail ──
            int preamble_flags = cfg.txdelay * cfg.baud / (8 * 100);
            if (preamble_flags < 15) preamble_flags = 15;

            tx_audio.clear();
            hdlc_enc.send_frame(frame.data(), frame.size(), preamble_flags, 2);
            modulator.put_quiet_ms(cfg.txtail * 10);

            if (tx_audio.empty()) continue;

            if (cfg.debug_level >= 1)
                fprintf(stderr, "[%s]  [TX] frame: %zu bytes, %zu samples (%.0f ms)\n",
                        dbg_ts(), frame.size(), tx_audio.size(),
                        1000.0 * tx_audio.size() / actual_rate);

            // ── TX: PTT ON → write → drain → PTT OFF ──
            tx_active.store(true, std::memory_order_release);
            ptt_ctl.set(true);
            if (cfg.debug_level >= 1) fprintf(stderr, "[%s]  [TX] PTT ON\n", dbg_ts());

            size_t off = 0;
            while (off < tx_audio.size()) {
                int chunk = std::min((int)(tx_audio.size() - off), 1024);
                int written = audio->write(tx_audio.data() + off, chunk);
                if (written > 0) off += written;
                else break;
            }
            audio->flush();
            audio->wait_drain();

            ptt_ctl.set(false);
            if (cfg.debug_level >= 1) fprintf(stderr, "[%s]  [TX] PTT OFF\n", dbg_ts());

            // Reset HDLC decoder — flush any partial frame from self-echo
            hdlc_dec.init();
            hdlc_dec.set_debug(cfg.debug_level);
            tx_active.store(false, std::memory_order_release);

            // ── COOLDOWN: 50ms hard guard before next PTT ──
            usleep(50000);
            // Loop back to WAIT_WORK — cv.wait will not block if queue is non-empty.
        }
    });

    // RX audio thread
    std::thread rx_thread([&]() {
        int16_t buf[1024];
        while (g_running) {
            int n = audio->read(buf, 1024);
            for (int i = 0; i < n; i++)
                demod.process_sample(buf[i]);
        }
    });

    // Main select() loop — handles PTY, TCP, and TX
    while (g_running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(pty_master, &rfds);
        int maxfd = pty_master;

        if (tcp_srv >= 0) { FD_SET(tcp_srv, &rfds); if (tcp_srv > maxfd) maxfd = tcp_srv; }
        for (int fd : tcp_clients) { FD_SET(fd, &rfds); if (fd > maxfd) maxfd = fd; }

        struct timeval tv = {0, 10000}; // 10ms — fast response for KISS frames
        int ret = select(maxfd + 1, &rfds, nullptr, nullptr, &tv);
        if (ret < 0) { if (errno == EINTR) continue; break; }

        // Accept TCP connections
        if (tcp_srv >= 0 && FD_ISSET(tcp_srv, &rfds)) {
            int cfd = accept(tcp_srv, nullptr, nullptr);
            if (cfd >= 0) {
                fcntl(cfd, F_SETFL, fcntl(cfd, F_GETFL) | O_NONBLOCK);
                tcp_clients.push_back(cfd);
                if (cfg.monitor) printf("  TCP client connected fd=%d\n", cfd);
            }
        }

        // Read from PTY
        if (FD_ISSET(pty_master, &rfds)) {
            uint8_t buf[2048];
            ssize_t n = ::read(pty_master, buf, sizeof(buf));
            if (n > 0) {
                if (cfg.debug_level >= 2) {
                    fprintf(stderr, "[%s]  [PTY] read %zd bytes:", dbg_ts(), n);
                    for (ssize_t i = 0; i < n && i < 32; i++) fprintf(stderr, " %02x", buf[i]);
                    if (n > 32) fprintf(stderr, " ...");
                    fprintf(stderr, "\n");
                }
                auto frames = kiss_dec.feed(buf, n);
                for (auto& kf : frames) {
                    if (kf.command == ax25::kiss::Cmd::Data && kf.data.size() > 0) {
                        if (cfg.debug_level >= 1)
                            fprintf(stderr, "[%s]  [TX] <- pty: %zu bytes\n", dbg_ts(), kf.data.size());
                        if (cfg.monitor) show_frame(kf.data.data(), kf.data.size(), "-> AIR");
                        size_t depth = enqueue_frame(kf.data);
                        tx_queue_cv.notify_one();
                        if (cfg.debug_level >= 2)
                            fprintf(stderr, "[%s]  [QUEUE] +1 frame (%zu bytes), depth=%zu\n",
                                    dbg_ts(), kf.data.size(), depth);
                    }
                    // Handle KISS parameter commands
                    else if (kf.command == ax25::kiss::Cmd::TxDelay && kf.data.size() >= 1)
                        kp.txdelay = kf.data[0];
                    else if (kf.command == ax25::kiss::Cmd::Persistence && kf.data.size() >= 1)
                        kp.persist = kf.data[0];
                    else if (kf.command == ax25::kiss::Cmd::SlotTime && kf.data.size() >= 1)
                        kp.slottime = kf.data[0];
                    else if (kf.command == ax25::kiss::Cmd::TxTail && kf.data.size() >= 1)
                        kp.txtail = kf.data[0];
                }
            }
        }

        // Read from TCP clients
        for (auto it = tcp_clients.begin(); it != tcp_clients.end(); ) {
            if (FD_ISSET(*it, &rfds)) {
                uint8_t buf[2048];
                ssize_t n = ::read(*it, buf, sizeof(buf));
                if (n <= 0) {
                    if (cfg.monitor) printf("  TCP client disconnected fd=%d\n", *it);
                    ::close(*it);
                    it = tcp_clients.erase(it);
                    continue;
                }
                auto frames = kiss_dec.feed(buf, n);
                for (auto& kf : frames) {
                    if (kf.command == ax25::kiss::Cmd::Data && kf.data.size() > 0) {
                        if (cfg.debug_level >= 1)
                            fprintf(stderr, "[%s]  [TX] <- tcp: %zu bytes\n", dbg_ts(), kf.data.size());
                        if (cfg.monitor) show_frame(kf.data.data(), kf.data.size(), "-> AIR");
                        size_t depth = enqueue_frame(kf.data);
                        tx_queue_cv.notify_one();
                        if (cfg.debug_level >= 2)
                            fprintf(stderr, "[%s]  [QUEUE] +1 frame (%zu bytes), depth=%zu\n",
                                    dbg_ts(), kf.data.size(), depth);
                    }
                }
            }
            ++it;
        }
    }

    g_running = false;
    tx_queue_cv.notify_all();
    tx_thread.join();
    audio->close();
    rx_thread.join();

    unlink(cfg.link_path.c_str());
    ::close(slave_fd);
    ::close(pty_master);
    if (tcp_srv >= 0) ::close(tcp_srv);
    for (int fd : tcp_clients) ::close(fd);

    ptt_ctl.close();
    delete audio;
    printf("\n  Session ended.\n");
}

// ---------------------------------------------------------------------------
//  main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    Config cfg = parse_args(argc, argv);

    if (cfg.list_devices) {
        AudioDevice::list_devices();
    } else if (cfg.test_ptt) {
        run_test_ptt(cfg);
    } else if (!cfg.test_tx.empty()) {
        run_test_tx(cfg);
    } else if (cfg.loopback) {
        run_loopback(cfg);
    } else {
        run_bridge(cfg);
    }

    return 0;
}
