newBIOS for the Biostar MB-8433UUD-A (UMC UM8881F/UM8886BF, 486/5x86)
=====================================================================

Open replacement BIOS: coreboot (coreboot_486) + SeaBIOS, with an Award-like setup utility (DEL),
POST screen, memory test, IDE, floppy, USB 1.1/2.0 keyboard and disk boot, network boot (F8 boot
menu, RTL8139 boot ROM), ISA PnP, PnP BIOS, APM, L2 cache control and tests (CACHE TEST, L2 SRAM TEST).
Runs DOS and Linux. Sources, build and tests: https://github.com/alphaonex86/mb-8433UUD-A-BIOS

Files
-----
newbios.rom              128 KiB, for the original 1 Mbit chip (Intel P28F001BX-T) or any 1 Mbit flash.
                         Halt On default: All Errors (a POST error waits for F1, as the Award BIOS).
newbios-halt1.rom        Same, Halt On default: No Errors. POST errors print a WARNING and the boot
                         goes on: choose it with a dead CMOS battery or no keyboard.
newbios-2mbit.rom        256 KiB, for a 2 Mbit chip (e.g. Winbond W49F002A): the BIOS in both halves,
newbios-halt1-2mbit.rom  as the socket keeps A17 high.
nic-rtl8139-ipxe-64k.rom Optional: iPXE boot ROM (64 KiB) for the RTL8139 network card's ROM socket.
POSTCODES.txt            POST codes on port 80h (POST card) and COM1 (115200 8N1, one "XX" line each).
SHA256SUMS               Checksums of the ROMs.
LICENSE                  GPL-3.0 (scripts and documents; coreboot GPL-2.0, SeaBIOS LGPL-3.0, iPXE GPL-2.0).

Flashing
--------
- Intel P28F001BX-T (original chip): needs 12 V to program, so use an external programmer.
- W49F002A and other 5 V chips: from the board itself (JP13 at 5 V), under Linux as root with
  flashrom and the UMC chipset patch: "bios-update newbios-halt1.rom" (see the repository).
- Keep a copy of your original BIOS before flashing.

Notes
-----
- A bad L2 cache crashes the POST once it is enabled: the next POST leaves it off and says so,
  so the setup and the L2 tests stay reachable.
- Boot order default: Hard Disk, USB, Network, Floppy. ESC at the prompt (or F8 in the setup): boot menu.
- No warranty: flashing a BIOS can leave the board unbootable until the chip is reprogrammed.
