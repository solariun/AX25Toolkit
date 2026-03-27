# Changelog

## 2026-03-27 — Fix install script service chaining + wget install

**Intent:** Review and fix `install_linux_bbs.sh` — services must chain properly (BLE bridge → kissattach → linpac/BBS), stop correctly when dependencies stop, and never auto-activate after install. Add one-line wget install from GitHub.

**Changes:**
- `resources/install_linux_bbs.sh`:
  - Removed auto-enable of BLE bridge service (was `systemctl enable` when BLE_DEVICE set)
  - All 4 services now created **disabled** — manual activation only
  - Added `BindsTo=` chaining: kissattach → BLE bridge, BBS/linpac → kissattach
  - When BLE bridge stops, entire chain stops automatically (cascade)
  - BLE bridge: added `Requires=bluetooth.target` (was `Wants=`), added `ExecStopPost` to clean up PTY
  - kissattach: changed `Type=forking` → `Type=oneshot` + `RemainAfterExit=yes`, added `ExecStop` to tear down ax0 interface
  - BBS/linpac: replaced `Wants=` with `BindsTo=` for hard dependency
  - Updated "Next steps" summary with BLE pairing instructions and correct enable order
- `README.md`:
  - Added one-line wget install: `wget -qO- ... | sudo bash`
  - Added download-and-edit install variant
  - Added service dependency table (shows chain)
  - Added "Post-install: BLE pairing and service activation" section
  - Documented that LE bonding is required before service activation

**Tests:** 180/180 pass (no code changes, script-only)
