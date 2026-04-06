# AX25Toolkit — Spec

## kiss_modem — TX Queue & Audio Redesign

### Problem

1. Long RX packet → ACK never transmitted.
   - `wait_drain()` held `tx_mtx_` while bulk-copying samples into ring, blocking the
     CoreAudio real-time render callback.  macOS kills/resets a unit whose RT thread
     blocks too long → silent audio, PTT pulses but nothing plays.
   - `kp.txdelay` / `kp.txtail` (set by KISS clients over PTY or TCP) could override
     the modem's own timing, allowing remote peers to corrupt RF timing.

2. TX queue had no P/F-bit awareness.
   - Batching all queued frames into one PTT burst is wrong when a frame carries P=1
     (poll) — the remote must respond before the next burst.

3. No post-PTT cooldown — back-to-back PTT cycles with no guard time.

---

### Design

#### TX timing authority

`cfg.txdelay` and `cfg.txtail` (set via `--txdelay` / `--txtail` CLI flags) are the sole
timing authority.  KISS parameter commands (`TxDelay`, `TxTail`) from clients are
accepted and stored in `kp` for protocol compliance but **never used for TX timing**.

Minimum preamble: `max(cfg.txdelay × baud / 800, 15)` flags.

#### TX queue

`std::deque<std::vector<uint8_t>>` — unbounded FIFO, bounded only by RAM.
Frames are never dropped; clients (PTY or TCP) may enqueue at any time.

**S-frame supersession**: when a new supervisory frame (RR, REJ, RNR) is enqueued,
the queue is scanned for an existing S-frame with the same address block (same
connection + direction).  If found, the old S-frame is replaced in-place — it never
transmits.  This prevents redundant RR/REJ frames from consuming separate PTT cycles.

Detection: compare address bytes up to the control byte offset; both frames must have
`(ctrl & 0x03) == 0x01` (S-frame bit pattern).  I-frames and U-frames are never
superseded.  Debug level 2 logs `[QUEUE] supersede S-frame`.

#### CSMA/CA — DWAIT + p-persistence + exponential backoff

Three-layer collision avoidance for half-duplex channels:

**DWAIT (post-RX holdoff)**: after the last received frame, don't start CSMA for
`dwait` ms.  This prevents keying up in the gap between I-frames in a window burst.
Without DWAIT, a 20ms inter-frame gap looks like "channel clear" and triggers TX,
colliding with the next frame in the burst.

Default: 1500ms at 1200 baud, 500ms at 9600 baud.  Override with `--dwait N`.

**P-persistence carrier sense** (Dire Wolf algorithm):

```
DWAIT       wait until now - last_rx_ts >= dwait
DCD_WAIT    poll demod.dcd() every 10ms until channel clear
SETTLE      20ms squelch tail guard
            if DCD returns during settle → restart DCD_WAIT
CSMA        loop:
              sleep(slottime × 10ms)       — default 100ms
              if DCD active → collision_count++, restart DWAIT
              if rand()&0xFF <= eff_persist → break (transmit)
              else → wait another slot
```

**Exponential backoff**: each DCD collision during a CSMA slot halves the effective
persist (`eff_persist = persist >> min(collision_count, 3)`).  This makes the station
progressively less aggressive on a busy channel, reducing collision probability.

| collision_count | eff_persist (base=63) | Tx prob/slot |
|---:|---:|---:|
| 0 | 63 | 25% |
| 1 | 31 | 12% |
| 2 | 15 | 6% |
| 3+ | 7 | 3% |

`collision_count` resets to 0 on successful transmit.

Parameters from CLI (`--persist N`, `--slottime N`, `--dwait N`), never from KISS clients.
Default: persist=63, slottime=10 (100ms), dwait=auto (1500ms@1200, 500ms@9600).

Full-duplex mode (`kp.fullduplex`) bypasses CSMA entirely.

#### TX thread state machine — one frame per PTT cycle

No batching.  Each frame gets its own DWAIT + CSMA → txdelay → data → txtail → PTT cycle.
This ensures proper turnaround time between frames and allows the remote to respond.

```
WAIT_WORK   cv.wait until queue non-empty
DWAIT       wait until now - last_rx_ts >= dwait ms
DCD_WAIT    poll demod.dcd() every 10ms until channel clear
SETTLE      20ms fixed (squelch tail), recheck DCD
CSMA        p-persistence slots (slottime × 10ms each)
            DCD during slot → collision_count++, restart DWAIT
DEQUEUE     pop ONE frame, log depth
MODULATE    txdelay preamble + single frame + txtail silence
TX          PTT ON → write → wait_drain → PTT OFF
COOLDOWN    50ms hard guard — no re-key
            → queue non-empty? → DCD_WAIT (cv.wait predicate skips block)
            → queue empty?     → WAIT_WORK
```

Debug level 2 logs enqueue (`[QUEUE] +1 frame ... depth=N`) and
dequeue (`[QUEUE] dequeue ... remaining=N`) for queue visibility.

#### ax25lib turnaround delay

`Router::route()` sets `tx_next_ = now + txdelay×10ms` on every received frame.
`drain_tx()` holds queued responses until `tx_next_` elapses, then flushes **all**
pending frames at once (no inter-frame pacing).  The TNC/modem handles RF timing
(txdelay preamble, CSMA slots) for each frame it transmits.

Previous behavior added a second `txdelay` pause between consecutive frames in
`drain_tx()`.  This was removed — the modem's per-frame CSMA + preamble provides
sufficient spacing, and the extra pacing caused excessive turnaround time.

#### Minimal AX.25 awareness (queue supersession only)

The modem is KISS-level.  It does not manage AX.25 state (no T1, no window, no
poll tracking).  The only AX.25 inspection is for queue supersession: the control
byte is checked to identify S-frames, and address bytes are compared to match
connections.  Upper-layer protocol correctness is the application's responsibility.

#### AX.25 monitor

`--monitor` decodes and displays frames using only the same ax25lib subset as
bt_kiss_bridge: `ax25::Frame::decode()`, `frame.format()`, `ax25::kiss::encode()`,
`ax25::kiss::Decoder`, and `hex_dump()` from ax25dump.hpp.
No Connection, Router, or connected-mode AX.25 features are used.

#### Half-duplex RX mute (self-echo suppression)

Standard TNC behavior: the demodulator continues running during TX (to maintain DCD
state), but any frame decoded by HDLC while `tx_active` is true is dropped.
After PTT OFF, the HDLC decoder is reset (`init()`) to flush any partial frame
assembled from the echo.

Debug level 1 logs dropped frames with AX.25 decode (`[RX] dropped echo: ...`).
Debug level 2 adds a hex dump.  This helps distinguish genuine self-echo from
real incoming frames arriving during TX (collision detection).

This prevents the host AX.25 stack from seeing its own transmitted frames as received
frames, which would corrupt the connection state machine (duplicate ACKs, spurious
REJs, retransmission storms).

Platform-independent: the `tx_active` flag is in `kiss_modem.cpp`, applies to both
CoreAudio (macOS) and ALSA (Linux).

#### Audio — lock-free TX ring (macOS)

The CoreAudio render callback (`tx_callback`) is a real-time thread.  Holding a mutex
inside it risks priority inversion and CoreAudio unit reset.

Replaced `std::mutex tx_mtx_` + plain-int `tx_wr_/tx_rd_/tx_avail_` with an SPSC
lock-free ring using `std::atomic<int> tx_wr_` / `tx_rd_`.  Single producer
(`wait_drain`), single consumer (`tx_callback`) — no mutex required.

`wait_drain` drain strategy: 1ms-poll loop checking ring occupancy, hard timeout at
`(samples / out_rate × 1000) + 200ms`.  Stops as soon as ring empties.

---

#### Debug levels (`--debug N`)

All debug and monitor output is timestamped (`HH:MM:SS.mmm`).

| Level | Tags | Content |
|-------|------|---------|
| 1 | `[TX]`, `[RX]` | PTT ON/OFF, frame size, samples, duration; dropped echo decode |
| 2 | `[DCD]`, `[QUEUE]`, `[PTY]` | DCD wait/clear, queue depth/supersede, PTY/TCP raw hex; echo hex dump |
| 3 | `[HDLC]` | FCS failures (got vs expected), abort events |

`--monitor` is independent of `--debug` and always prints decoded AX.25 frames.

---

---

## ax25lib — ACK Coalescing & Turnaround Optimization

### Problem

Real-world testing with G2UGK-1 revealed excessive retransmissions and slow turnaround:

1. **Redundant RR frames**: `Connection::handle_frame()` sent a separate RR for every
   in-sequence I-frame.  When the remote sent I Ns=0 and I Ns=1 back-to-back, ax25lib
   generated RR Nr=1 then RR Nr=2 — each consuming a full PTT cycle (~1.2s).

2. **RR + REJ pair**: out-of-sequence I-frames triggered `tx_rr()` then a separate REJ
   frame.  Two frames for one error condition, wasting a PTT cycle.

3. **drain_tx() inter-frame pacing**: after sending one frame, `drain_tx()` added a
   `txdelay×10ms` pause before the next.  This was on top of the turnaround delay in
   `route()` and the modem's own txdelay preamble — triple-counting turnaround time.

Combined effect: ~20 unnecessary PTT cycles in a typical session, each ~1.2s of air time
where the station was transmitting instead of listening.

### Design

#### T2 delayed ACK

New timer `T2` (default 200ms, configurable via `Config::t2_ms`).

When an in-sequence I-frame arrives with P=0:
- Set `ack_pending_ = true`, start/restart T2.
- When T2 fires: send one RR with the latest `vr_` (coalesces all pending ACKs).
- When an I-frame is sent (`tx_iframe`): the piggybacked NR clears `ack_pending_`.

P=1 (poll) bypasses T2 — respond immediately with F=1.

T2 is cleared on connect/disconnect/SABM reset.

#### REJ without preceding RR

Out-of-sequence I-frames now send only REJ (with P/F from the received frame).
The previous `tx_rr()` call before REJ was redundant — REJ already carries NR.

#### drain_tx() — no inter-frame pacing

`drain_tx()` now flushes all frames past the turnaround deadline at once.
The TNC/modem handles RF timing (txdelay preamble, CSMA) per frame.

### Files changed

| File | Change |
|------|--------|
| `kiss_modem/kiss_modem.cpp` | TX queue → deque; TX thread state machine; timing always from cfg; `--debug N` (1-3) with timestamps; S-frame queue supersession; echo frame decode in debug |
| `kiss_modem/audio_coreaudio.cpp` | Lock-free TX ring; remove tx_mtx_; wait_drain drain loop |
| `kiss_modem/hdlc.h` | `set_debug(bool)` → `set_debug(int)` for level support |
| `kiss_modem/hdlc.cpp` | Debug prints gated at level 3 |
| `lib/ax25lib.hpp` | `Config::t2_ms`; `Connection`: `ack_pending_`, `t2_run_`, `t2_exp_`, `start_t2()`, `stop_t2()` |
| `lib/ax25lib.cpp` | T2 delayed ACK in `handle_frame()` + `tick()`; REJ without RR; `drain_tx()` no inter-frame pacing; `tx_iframe()` clears pending ACK |
