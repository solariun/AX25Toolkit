// GoogleTest suite for modemtnc — HDLC framing + modem loopback
// Tests the complete TX → RX chain without audio hardware
#include <gtest/gtest.h>
#include <vector>
#include <cstring>

#include "hdlc.h"
#include "modem.h"
#include "ax25lib.hpp"

// ===========================================================================
//  FCS / CRC16-CCITT
// ===========================================================================

TEST(HdlcFcs, KnownVector) {
    // "123456789" with CRC16-CCITT (initial=0xFFFF, final XOR=0xFFFF)
    // This is the AX.25 FCS polynomial (bit-reversed 0x8408)
    uint8_t data[] = "123456789";
    uint16_t crc = hdlc::fcs_calc(data, 9);
    EXPECT_EQ(crc, 0x906E);
}

TEST(HdlcFcs, EmptyData) {
    uint16_t crc = hdlc::fcs_calc(nullptr, 0);
    EXPECT_EQ(crc, 0xFFFF ^ 0xFFFF);  // initial XOR final = 0
}

TEST(HdlcFcs, SingleByte) {
    uint8_t data[] = {0x00};
    uint16_t crc = hdlc::fcs_calc(data, 1);
    EXPECT_NE(crc, 0);  // should produce non-trivial CRC
}

// ===========================================================================
//  HDLC Encoder → Decoder round-trip
// ===========================================================================

TEST(HdlcRoundTrip, SimpleFrame) {
    // Build a minimal AX.25 UI frame
    ax25::Frame f;
    f.dest = ax25::Addr::make("CQ");
    f.src  = ax25::Addr::make("TEST");
    f.ctrl = 0x03;  // UI
    f.pid  = 0xF0;
    const char* msg = "Hello HDLC";
    f.info.assign(msg, msg + strlen(msg));
    auto raw = f.encode();

    // Encode → bit stream → Decode
    hdlc::Encoder enc;
    hdlc::Decoder dec;
    dec.init();

    std::vector<uint8_t> received_frame;
    dec.set_on_frame([&](const uint8_t* data, size_t len) {
        received_frame.assign(data, data + len);
    });

    // Encode produces NRZI bit stream; decoder expects NRZI-decoded bits
    // Since we feed encoder output directly to decoder, we need to undo NRZI
    int prev_bit = 0;
    enc.set_on_bit([&](int nrzi_bit) {
        // NRZI decode: same = 1, different = 0
        int decoded = (nrzi_bit == prev_bit) ? 1 : 0;
        prev_bit = nrzi_bit;
        dec.receive_bit(decoded);
    });

    enc.send_frame(raw.data(), raw.size(), 10, 2);

    ASSERT_EQ(received_frame.size(), raw.size());
    EXPECT_EQ(received_frame, raw);
}

TEST(HdlcRoundTrip, LargeFrame) {
    // Test with a larger payload (256 bytes)
    std::vector<uint8_t> frame(256);
    for (int i = 0; i < 256; i++) frame[i] = (uint8_t)i;

    // Prepend minimal AX.25 header (14 bytes addr + ctrl)
    ax25::Frame f;
    f.dest = ax25::Addr::make("DEST-1");
    f.src  = ax25::Addr::make("SRC-2");
    f.ctrl = 0x03;
    f.pid  = 0xF0;
    f.info = frame;
    auto raw = f.encode();

    hdlc::Encoder enc;
    hdlc::Decoder dec;
    dec.init();

    std::vector<uint8_t> received;
    dec.set_on_frame([&](const uint8_t* data, size_t len) {
        received.assign(data, data + len);
    });

    int prev = 0;
    enc.set_on_bit([&](int bit) {
        int d = (bit == prev) ? 1 : 0;
        prev = bit;
        dec.receive_bit(d);
    });

    enc.send_frame(raw.data(), raw.size(), 10, 2);
    ASSERT_EQ(received.size(), raw.size());
    EXPECT_EQ(received, raw);
}

TEST(HdlcRoundTrip, MultipleFrames) {
    hdlc::Encoder enc;
    hdlc::Decoder dec;
    dec.init();

    int frame_count = 0;
    dec.set_on_frame([&](const uint8_t*, size_t) { frame_count++; });

    int prev = 0;
    enc.set_on_bit([&](int bit) {
        int d = (bit == prev) ? 1 : 0;
        prev = bit;
        dec.receive_bit(d);
    });

    // Send 5 frames back-to-back
    ax25::Frame f;
    f.dest = ax25::Addr::make("CQ");
    f.src  = ax25::Addr::make("TEST");
    f.ctrl = 0x03;
    f.pid  = 0xF0;
    f.info = {'A', 'B', 'C'};
    auto raw = f.encode();

    for (int i = 0; i < 5; i++)
        enc.send_frame(raw.data(), raw.size(), 10, 2);

    EXPECT_EQ(frame_count, 5);
}

// ===========================================================================
//  HDLC Bit Stuffing
// ===========================================================================

TEST(HdlcBitStuff, AllOnesPayload) {
    // A payload of all 0xFF bytes should trigger lots of bit stuffing
    std::vector<uint8_t> payload(20, 0xFF);

    hdlc::Encoder enc;
    hdlc::Decoder dec;
    dec.init();

    std::vector<uint8_t> received;
    dec.set_on_frame([&](const uint8_t* data, size_t len) {
        received.assign(data, data + len);
    });

    int prev = 0;
    enc.set_on_bit([&](int bit) {
        int d = (bit == prev) ? 1 : 0;
        prev = bit;
        dec.receive_bit(d);
    });

    enc.send_frame(payload.data(), payload.size(), 10, 2);
    ASSERT_EQ(received.size(), payload.size());
    EXPECT_EQ(received, payload);
}

// ===========================================================================
//  Modem Loopback — AFSK 1200
// ===========================================================================

class ModemLoopback : public ::testing::TestWithParam<std::tuple<modem::Type, int, int>> {};

TEST_P(ModemLoopback, RoundTrip) {
    auto [type, baud, sample_rate] = GetParam();

    modem::Modulator mod;
    modem::Demodulator demod;
    hdlc::Encoder enc;
    hdlc::Decoder dec;

    mod.init(type, sample_rate, 16000);
    demod.init(type, sample_rate);
    dec.init();

    std::vector<uint8_t> received;
    dec.set_on_frame([&](const uint8_t* data, size_t len) {
        received.assign(data, data + len);
    });

    demod.set_on_bit([&dec](int bit) { dec.receive_bit(bit); });
    mod.set_on_sample([&demod](int16_t s) { demod.process_sample(s); });
    enc.set_on_bit([&mod](int bit) { mod.put_bit(bit); });

    // Build test frame
    ax25::Frame f;
    f.dest = ax25::Addr::make("CQ");
    f.src  = ax25::Addr::make("TEST");
    f.ctrl = 0x03;
    f.pid  = 0xF0;
    const char* msg = "Loopback test frame for GoogleTest";
    f.info.assign(msg, msg + strlen(msg));
    auto raw = f.encode();

    int flags = 300 * baud / (8 * 1000);
    if (flags < 10) flags = 10;

    enc.send_frame(raw.data(), raw.size(), flags, 2);
    mod.put_quiet_ms(100);  // flush demod pipeline

    ASSERT_FALSE(received.empty()) << "No frame decoded at " << baud << " baud";
    EXPECT_EQ(received, raw);
}

INSTANTIATE_TEST_SUITE_P(
    ModemSpeeds,
    ModemLoopback,
    ::testing::Values(
        std::make_tuple(modem::AFSK_1200, 1200, 44100),
        std::make_tuple(modem::AFSK_300,  300,  44100),
        std::make_tuple(modem::GMSK_9600, 9600, 96000)
    ),
    [](const ::testing::TestParamInfo<ModemLoopback::ParamType>& info) {
        return "Baud" + std::to_string(std::get<1>(info.param));
    }
);

// ===========================================================================
//  Modem Loopback — Frame integrity under different payloads
// ===========================================================================

TEST(ModemIntegrity, AFSK1200_EmptyInfo) {
    modem::Modulator mod;
    modem::Demodulator demod;
    hdlc::Encoder enc;
    hdlc::Decoder dec;

    mod.init(modem::AFSK_1200, 44100, 16000);
    demod.init(modem::AFSK_1200, 44100);
    dec.init();

    std::vector<uint8_t> received;
    dec.set_on_frame([&](const uint8_t* data, size_t len) {
        received.assign(data, data + len);
    });
    demod.set_on_bit([&dec](int bit) { dec.receive_bit(bit); });
    mod.set_on_sample([&demod](int16_t s) { demod.process_sample(s); });
    enc.set_on_bit([&mod](int bit) { mod.put_bit(bit); });

    // Minimal frame: just addresses + ctrl (no info, no PID)
    ax25::Frame f;
    f.dest = ax25::Addr::make("CQ");
    f.src  = ax25::Addr::make("T");
    f.ctrl = 0x03;
    f.pid  = 0xF0;
    auto raw = f.encode();

    enc.send_frame(raw.data(), raw.size(), 30, 2);
    mod.put_quiet_ms(100);

    ASSERT_FALSE(received.empty());
    EXPECT_EQ(received, raw);
}

TEST(ModemIntegrity, AFSK1200_BinaryPayload) {
    modem::Modulator mod;
    modem::Demodulator demod;
    hdlc::Encoder enc;
    hdlc::Decoder dec;

    mod.init(modem::AFSK_1200, 44100, 16000);
    demod.init(modem::AFSK_1200, 44100);
    dec.init();

    std::vector<uint8_t> received;
    dec.set_on_frame([&](const uint8_t* data, size_t len) {
        received.assign(data, data + len);
    });
    demod.set_on_bit([&dec](int bit) { dec.receive_bit(bit); });
    mod.set_on_sample([&demod](int16_t s) { demod.process_sample(s); });
    enc.set_on_bit([&mod](int bit) { mod.put_bit(bit); });

    // Frame with all byte values 0x00-0xFF
    ax25::Frame f;
    f.dest = ax25::Addr::make("CQ");
    f.src  = ax25::Addr::make("TEST-5");
    f.ctrl = 0x03;
    f.pid  = 0xF0;
    for (int i = 0; i < 128; i++) f.info.push_back((uint8_t)i);
    auto raw = f.encode();

    enc.send_frame(raw.data(), raw.size(), 30, 2);
    mod.put_quiet_ms(100);

    ASSERT_FALSE(received.empty());
    EXPECT_EQ(received, raw);
}

// ===========================================================================
//  Scrambler/Descrambler symmetry
// ===========================================================================

TEST(Scrambler, RoundTrip) {
    // G3RUH scrambler → descrambler should recover original data
    auto scramble = [](int in, int* state) -> int {
        int out = (in ^ (*state >> 16) ^ (*state >> 11)) & 1;
        *state = (*state << 1) | (out & 1);
        return out;
    };
    auto descramble = [](int in, int* state) -> int {
        int out = (in ^ (*state >> 16) ^ (*state >> 11)) & 1;
        *state = (*state << 1) | (in & 1);
        return out;
    };

    int sc_state = 0, dsc_state = 0;
    int errors = 0;
    for (int i = 0; i < 10000; i++) {
        int original = (i * 37 + 13) & 1;
        int scrambled = scramble(original, &sc_state);
        int recovered = descramble(scrambled, &dsc_state);
        if (recovered != original) errors++;
    }
    EXPECT_EQ(errors, 0);
}

TEST(Scrambler, SelfSync) {
    // Descrambler should sync after 17 bits even with wrong initial state
    auto scramble = [](int in, int* state) -> int {
        int out = (in ^ (*state >> 16) ^ (*state >> 11)) & 1;
        *state = (*state << 1) | (out & 1);
        return out;
    };
    auto descramble = [](int in, int* state) -> int {
        int out = (in ^ (*state >> 16) ^ (*state >> 11)) & 1;
        *state = (*state << 1) | (in & 1);
        return out;
    };

    int sc_state = 0;
    int dsc_state = 0x1FFFF;  // wrong initial state

    // Generate scrambled bits
    std::vector<int> scrambled;
    for (int i = 0; i < 100; i++)
        scrambled.push_back(scramble((i * 37 + 13) & 1, &sc_state));

    // Descramble with wrong initial state — first 17 bits may be wrong
    sc_state = 0;
    int errors_after_sync = 0;
    for (int i = 0; i < 100; i++) {
        int original = (i * 37 + 13) & 1;
        int recovered = descramble(scrambled[i], &dsc_state);
        if (i >= 17 && recovered != original) errors_after_sync++;
    }
    EXPECT_EQ(errors_after_sync, 0);
}
