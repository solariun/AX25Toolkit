#!/usr/bin/env python3
"""
ble_kiss_monitor.py — BLE KISS TNC scanner and AX.25 monitor

Requires:  pip install bleak

Modes:
  --scan               Discover nearby BLE devices
  --inspect  <ADDR>    List every service/characteristic of a device
  --device   <ADDR>    Monitor AX.25 traffic (requires --service/--write/--read)

Examples:
  python3 ble_kiss_monitor.py --scan --timeout 15
  python3 ble_kiss_monitor.py --inspect AA:BB:CC:DD:EE:FF
  python3 ble_kiss_monitor.py \\
      --device   AA:BB:CC:DD:EE:FF \\
      --service  00000001-ba2a-46c9-ae49-01b0961f68bb \\
      --write    00000003-ba2a-46c9-ae49-01b0961f68bb \\
      --read     00000002-ba2a-46c9-ae49-01b0961f68bb
"""

import asyncio
import sys
import argparse
import datetime
from typing import List, Tuple, Optional

try:
    from bleak import BleakScanner, BleakClient
    from bleak.backends.characteristic import BleakGATTCharacteristic
except ImportError:
    print("bleak not found.  Install with:  pip install bleak")
    sys.exit(1)

# ── KISS constants ────────────────────────────────────────────────────────────
FEND  = 0xC0
FESC  = 0xDB
TFEND = 0xDC
TFESC = 0xDD

KISS_CMD = {
    0:  "DATA", 1:  "TXDELAY", 2:  "P",    3: "SLOTTIME",
    4:  "TXTAIL", 5: "FULLDUPLEX", 6: "SETHW", 15: "RETURN",
}

# ── AX.25 frame type tables ───────────────────────────────────────────────────
U_FRAMES = {
    0x2F: "SABM",  0x3F: "SABM(P)",
    0x43: "DISC",  0x53: "DISC(P)",
    0x63: "UA",    0x73: "UA(F)",
    0x0F: "DM",    0x1F: "DM(F)",
    0x87: "FRMR",  0x97: "FRMR(F)",
    0x03: "UI",    0x13: "UI(P)",
}
S_TYPES = {0x01: "RR", 0x05: "RNR", 0x09: "REJ", 0x0D: "SREJ"}

PID_NAMES = {
    0xF0: "NoL3", 0xCF: "NET/ROM", 0xCC: "IP", 0xCD: "ARP",
    0x08: "ROSE", 0x01: "X25PLP",
}

# ── Address decoder ───────────────────────────────────────────────────────────
def decode_addr(data: bytes, offset: int) -> Tuple[str, bool]:
    """Return (callsign-ssid, end_of_address_flag) from 7 bytes at offset."""
    call = "".join(chr(data[offset + i] >> 1) for i in range(6)).rstrip()
    ssid_byte = data[offset + 6]
    ssid = (ssid_byte >> 1) & 0x0F
    end  = bool(ssid_byte & 0x01)
    label = f"{call}-{ssid}" if ssid else call
    return label, end

# ── AX.25 full decoder ────────────────────────────────────────────────────────
def decode_ax25(raw: bytes) -> dict:
    """
    Decode an AX.25 frame.
    Returns a dict with keys: dest, src, via, ctrl_hex, type, info, summary.
    """
    result = {"dest": "?", "src": "?", "via": [], "ctrl_hex": "",
              "type": "?", "info": b"", "summary": ""}

    if len(raw) < 15:
        result["type"]    = "TRUNCATED"
        result["summary"] = f"[frame too short: {len(raw)} bytes] {raw.hex()}"
        return result

    dest, _       = decode_addr(raw, 0)
    src,  end_src = decode_addr(raw, 7)
    result["dest"] = dest
    result["src"]  = src

    offset = 14
    via: List[str] = []
    while not end_src and offset + 7 <= len(raw):
        rep, end_src = decode_addr(raw, offset)
        via.append(rep)
        offset += 7
    result["via"] = via

    if offset >= len(raw):
        result["type"]    = "NO-CTRL"
        result["summary"] = f"{src} → {dest}  [no control byte]"
        return result

    ctrl = raw[offset]
    result["ctrl_hex"] = f"0x{ctrl:02X}"
    pf   = bool(ctrl & 0x10)

    if (ctrl & 0x01) == 0:                            # ── I-frame ──
        ns  = (ctrl >> 1) & 0x07
        nr  = (ctrl >> 5) & 0x07
        pid = raw[offset + 1] if offset + 1 < len(raw) else None
        info = raw[offset + 2:] if offset + 2 < len(raw) else b""
        result["info"] = info

        pid_str = f"PID={PID_NAMES.get(pid, f'0x{pid:02X}')}" if pid is not None else ""
        data_str = ""
        if info:
            try:
                data_str = f' "{info.decode("ascii", errors="replace")}"'
            except Exception:
                data_str = f" [{info.hex()}]"

        ftype = f"I(NS={ns},NR={nr}{'P' if pf else ''})"
        result["type"]    = "I"
        result["summary"] = f"{src} → {dest}  [{ftype}] {pid_str}{data_str}"

    elif (ctrl & 0x03) == 0x01:                       # ── S-frame ──
        nr    = (ctrl >> 5) & 0x07
        stype = S_TYPES.get(ctrl & 0x0F, f"S?{ctrl & 0x0F:X}")
        pf_s  = "P/F" if pf else ""
        result["type"]    = stype
        result["summary"] = f"{src} → {dest}  [{stype}(NR={nr}{pf_s})]"

    else:                                              # ── U-frame ──
        base  = ctrl & ~0x10
        ftype = U_FRAMES.get(ctrl, U_FRAMES.get(base, f"U?0x{ctrl:02X}"))
        result["type"]    = ftype.split("(")[0]
        via_str = f" via {','.join(via)}" if via else ""
        result["summary"] = f"{src} → {dest}{via_str}  [{ftype}]"

    return result

# ── Stateful KISS decoder ─────────────────────────────────────────────────────
class KissDecoder:
    def __init__(self):
        self._buf:      bytearray = bytearray()
        self._in_frame: bool      = False
        self._escape:   bool      = False

    def feed(self, data: bytes) -> List[Tuple[int, int, bytes]]:
        """
        Feed raw bytes.  Yields (port, cmd_type, ax25_payload) for each
        complete KISS DATA frame decoded.
        """
        frames = []
        for b in data:
            if b == FEND:
                if self._in_frame and len(self._buf) > 1:
                    cmd  = self._buf[0]
                    port = (cmd >> 4) & 0x0F
                    typ  = cmd & 0x0F
                    frames.append((port, typ, bytes(self._buf[1:])))
                self._buf.clear()
                self._in_frame = True
                self._escape   = False
            elif not self._in_frame:
                pass
            elif b == FESC:
                self._escape = True
            elif self._escape:
                self._escape = False
                self._buf.append(FEND if b == TFEND else FESC if b == TFESC else b)
            else:
                self._buf.append(b)
        return frames

# ── Helpers ───────────────────────────────────────────────────────────────────
def ts() -> str:
    return datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]

def hr(ch: str = "─", n: int = 68) -> str:
    return ch * n

# ── SCAN mode ─────────────────────────────────────────────────────────────────
async def scan(timeout: float) -> None:
    print(f"Scanning for BLE devices ({timeout:.0f}s)…\n")
    found = await BleakScanner.discover(timeout=timeout, return_adv=True)

    if not found:
        print("No devices found.")
        return

    for addr, (dev, adv) in sorted(found.items(), key=lambda x: -(x[1][1].rssi or -999)):
        name = dev.name or "(no name)"
        print(hr())
        print(f"  Name   : {name}")
        print(f"  Address: {addr}")
        print(f"  RSSI   : {adv.rssi} dBm")
        if adv.service_uuids:
            print(f"  Services advertised:")
            for u in adv.service_uuids:
                print(f"    {u}")
        if adv.manufacturer_data:
            for cid, mdata in adv.manufacturer_data.items():
                print(f"  Manufacturer data  [0x{cid:04X}]: {mdata.hex()}")
        print()

    print(hr())
    print(f"Found {len(found)} device(s).")
    print(f"\nNext step → enumerate characteristics:")
    print(f"  python3 {sys.argv[0]} --inspect <ADDRESS>")

# ── INSPECT mode ──────────────────────────────────────────────────────────────
async def inspect(address: str) -> None:
    print(f"Connecting to {address} for service inspection…")
    async with BleakClient(address) as client:
        print(f"Connected.  MTU={client.mtu_size}\n")
        for svc in client.services:
            print(f"{'═'*68}")
            print(f"Service : {svc.uuid}")
            print(f"          {svc.description}")
            for ch in svc.characteristics:
                props = " | ".join(sorted(ch.properties))
                print(f"\n  Characteristic: {ch.uuid}")
                print(f"  Handle        : 0x{ch.handle:04X}")
                print(f"  Properties    : {props}")
                print(f"  Description   : {ch.description}")
                if "read" in ch.properties:
                    try:
                        val = await client.read_gatt_char(ch.uuid)
                        print(f"  Current value : {val.hex()}  {val!r}")
                    except Exception as e:
                        print(f"  Current value : (read error: {e})")
                for desc in ch.descriptors:
                    print(f"    Descriptor  : {desc.uuid}  {desc.description}")
        print(f"\n{'═'*68}")
        print("\nMonitor command:")
        print(f"  python3 {sys.argv[0]} \\")
        print(f"      --device   {address} \\")
        print(f"      --service  <SERVICE-UUID> \\")
        print(f"      --write    <WRITE-CHAR-UUID> \\")
        print(f"      --read     <NOTIFY-CHAR-UUID>")

# ── MONITOR mode ──────────────────────────────────────────────────────────────
async def monitor(address: str, service_uuid: str,
                  write_uuid: str, read_uuid: str) -> None:
    decoder   = KissDecoder()
    rx_frames = 0

    def on_notify(char: BleakGATTCharacteristic, data: bytearray) -> None:
        nonlocal rx_frames
        raw   = bytes(data)
        now   = ts()
        print(f"\n{hr('─')}")
        print(f"[{now}]  ← BLE RX  {len(raw):3d} bytes  raw: {raw.hex()}")

        frames = decoder.feed(raw)
        if not frames:
            print("  (no complete KISS frame yet — buffering)")
            return

        for port, typ, payload in frames:
            rx_frames += 1
            cmd_name = KISS_CMD.get(typ, f"cmd{typ}")
            print(f"  KISS  port={port}  type={typ}({cmd_name})")
            print(f"  AX25  payload ({len(payload)}b): {payload.hex()}")

            if typ == 0 and payload:                        # DATA frame
                ax = decode_ax25(payload)
                print(f"  ┌─ {ax['summary']}")
                print(f"  │  ctrl={ax['ctrl_hex']}  type={ax['type']}")
                if ax["via"]:
                    print(f"  │  via: {', '.join(ax['via'])}")
                if ax["info"]:
                    try:
                        txt = ax["info"].decode("ascii", errors="replace")
                    except Exception:
                        txt = ax["info"].hex()
                    print(f"  └─ info ({len(ax['info'])}b): {txt!r}")
                else:
                    print(f"  └─")

    print(hr("═"))
    print(f"  BLE KISS/AX.25 Monitor")
    print(hr("═"))
    print(f"  Device  : {address}")
    print(f"  Service : {service_uuid}")
    print(f"  Read    : {read_uuid}  (notify)")
    print(f"  Write   : {write_uuid}")
    print(hr("═"))
    print(f"  Connecting…")

    async with BleakClient(address) as client:
        print(f"  Connected.  MTU={client.mtu_size}")
        print(f"  Waiting for AX.25 frames.  Ctrl-C to stop.\n")

        await client.start_notify(read_uuid, on_notify)
        try:
            while True:
                await asyncio.sleep(1)
        except (asyncio.CancelledError, KeyboardInterrupt):
            pass
        finally:
            await client.stop_notify(read_uuid)

    print(f"\n{hr()}")
    print(f"  Session ended.  Frames received: {rx_frames}")
    print(hr())

# ── Entry point ───────────────────────────────────────────────────────────────
def main() -> None:
    p = argparse.ArgumentParser(
        prog="ble_kiss_monitor.py",
        description="BLE KISS TNC scanner and AX.25 monitor",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )

    mode = p.add_mutually_exclusive_group(required=True)
    mode.add_argument("--scan",    action="store_true",
                      help="Scan for nearby BLE devices")
    mode.add_argument("--inspect", metavar="ADDRESS",
                      help="Enumerate all services/characteristics of a device")
    mode.add_argument("--device",  metavar="ADDRESS",
                      help="Device address for monitor mode")

    p.add_argument("--timeout", type=float, default=10.0,
                   help="Scan timeout in seconds (default: 10)")
    p.add_argument("--service", metavar="UUID",
                   help="GATT service UUID (monitor mode)")
    p.add_argument("--write",   metavar="UUID",
                   help="Write characteristic UUID (monitor mode)")
    p.add_argument("--read",    metavar="UUID",
                   help="Notify/read characteristic UUID (monitor mode)")

    args = p.parse_args()

    if args.device and not (args.service and args.write and args.read):
        p.error("--device requires --service, --write, and --read")

    try:
        if args.scan:
            asyncio.run(scan(args.timeout))
        elif args.inspect:
            asyncio.run(inspect(args.inspect))
        else:
            asyncio.run(monitor(args.device, args.service, args.write, args.read))
    except KeyboardInterrupt:
        print("\nInterrupted.")

if __name__ == "__main__":
    main()
