> Former firmware README. Paths are relative to `basestation_firmware/`.

# Lighthouse 1.0 base station firmware

Reverse engineering of the HTC Vive Lighthouse 1.0 base station (NXP LPC11U37,
stock firmware `lighthouse_tx_htc_2_0-436-2016-09-20`) and **lhbs**, a readable
reimplementation with stock feature parity. lhbs is tracked by SteamVR alone,
as optical slave of a stock master, and as master of a stock slave.

| Path | What |
|---|---|
| `ai/firmware/HARDWARE.md` | Hardware/firmware reference: MCU, pin map, peripherals, protocols (sync/OOTX, EEPROM parameters, Bluetooth power management), verification log |
| `src/`, `test/`, `Makefile` | The firmware rewrite. One module per stock source file; every function names the stock address it replaces |
| `fw/` (local only) | `lhfw`, a minimal bring-up firmware (display, LEDs, button, EEPROM snapshot) |
| `tools/` | `flash.sh` (flash/verify via the ROM USB drive), `lhcon.py` (USB console), `lhpower.py` (Bluetooth sleep/wake from Linux), `params_decode.py`, `ootx_decode.py`, `eeprom_extract.py`, `gen_params.py`, `lpcsum.py`, `lhpatch.py` |
| `re/` (local only) | Analysis scripts (headless Ghidra, Unicorn RAM-init dump), function names, parameter table |
| `patches/` (local only) | Byte patches for the stock image |

Not in git (see `.gitignore`): the stock firmware image, boot ROM dump, decompiled
stock code, and personal flash/EEPROM backups. Place the stock image at the repo
root as `lighthouse_tx_htc_2_0-436-2016-09-20.bin` for `gen_params.py`/`lhpatch.py`.

## Build and flash

```sh
make                               # -> build/firmware.bin (checksummed)
# hold the mode button while powering on -> "CRP DISABLD" USB drive appears
tools/flash.sh build/firmware.bin
# power-cycle; console on /dev/lhbs -> tools/lhcon.py status
```

USB console access (udev rule, creates `/dev/lhbs`):

```sh
echo 'SUBSYSTEM=="tty", ATTRS{idVendor}=="1fc9", ATTRS{idProduct}=="0083", TAG+="uaccess", SYMLINK+="lhbs"' | sudo tee /etc/udev/rules.d/70-lhbs.rules
```

Never copy firmware onto the ISP drive with a file manager: it corrupts flash
(see `ai/firmware/HARDWARE.md`); `flash.sh` writes in place.

## Safety

lhbs drives Class 1 lasers only while both rotors are locked, with several
independent interlocks. A stationary beam is not eye safe: keep the rotor
interlocks intact in any modification.
