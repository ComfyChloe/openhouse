lhbs firmware for 1.0 base stations
===================================

lhbs is replacement firmware for Lighthouse 1.0 base stations. It keeps everything
the original does (channels A/b/c, cable sync) and adds a multi-station mode for up
to four base stations in one room.

Before you start
  - You need the base station, its power cable and a micro-USB cable.
  - Do one base station at a time.
  - Your current firmware is saved before anything is written, in the backups
    folder, named after the serial you type in.

Installing
  Windows:  double-click Install-Firmware.bat
  Linux:    run ./install-firmware.sh in a terminal

The script tells you what to do. In short:
  1. Unplug the base station's power.
  2. Hold the button on the back, plug in the micro-USB cable and then the power.
     Let go of the button. A small drive called "CRP DISABLD" shows up on the PC.
  3. Type the serial from the sticker (or press Enter) and confirm.
  4. When it asks, unplug and plug it back in the same way, holding the button.
     It reads the firmware back to make sure it was written right.
  5. Unplug it and plug the power back in normally. Done.

Don't copy firmware.bin onto the drive yourself or drag it in a file manager.
That replaces the file, and the base station's bootloader can end up writing it to
the wrong place. The scripts write it in place, which is safe.

Going back to your old firmware
  Windows:  Install-Firmware.bat -Restore backups\LHB-XXXXXXXX-....bin
  Linux:    ./install-firmware.sh --restore backups/LHB-XXXXXXXX-....bin

If something goes wrong
  The base station's bootloader is in ROM and can't be overwritten, so it can
  always be brought back: hold the button while plugging it in, and restore your
  backup.

Linux needs udisks2 (for mounting the drive without root), which most desktops
already have.
