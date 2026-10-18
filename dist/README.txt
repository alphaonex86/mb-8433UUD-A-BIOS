mb-8433UUD for the Biostar MB-8433UUD-A
====================================

An open replacement BIOS for the Biostar MB-8433UUD-A (UMC UM8881F/UM8886BF chipset, 486 and Am5x86
CPUs): coreboot (coreboot_486) for the chipset, SeaBIOS for the PC BIOS services, and an Award-like
setup utility with diagnostics and benchmarks. Runs DOS and Linux.

Sources, build, tests: https://github.com/alphaonex86/mb-8433UUD-A-BIOS


Contents
--------
BIOS.bin       The BIOS, 128 KiB, for the board's original flash chip (Intel P28F001BX-T).
POSTCODES.txt  What each POST code means (port 80h POST card, and COM1).
README.txt     This file.


Flashing
--------
- Save your original BIOS first (read the chip twice and compare the dumps).
- Intel P28F001BX-T (original chip): it needs 12 V to program, so use an external programmer,
  e.g. minipro -p "P28F001BX-T@DIP32" -w BIOS.bin
- 5 V 2 Mbit chips (e.g. Winbond W49F002A, JP13 at 5 V): the socket keeps A17 high, so put the BIOS in
  both halves (cat BIOS.bin BIOS.bin > bios-2mbit.bin) for a programmer, or flash from the board
  under Linux with the adapted flashrom (see Tools).
- No warranty: a failed flash leaves the board unbootable until the chip is reprogrammed.


Boot time
---------
Measured on the real board (Am5x86-133, 32 MB, L2 off, USB stick), POST codes timed on COM1:
- Reset to the boot of the disk: 5.9 s (2.8 s of POST, then the boot prompt: 2 s by default,
  1-5 s in setup; 3 s here because a dead CMOS battery adds a warning to read).
- Linux on that board: 30 s from reset to the end of its init scripts.
What makes it fast: USB and IDE devices set up in parallel threads, a quick memory test by
default, the floppy motor started early in the POST, coreboot printk compiled out of the flash ROM.


Features
--------
Setup utility (DEL at the prompt), Award layout and keys:
- STANDARD CMOS, BIOS FEATURES, CHIPSET FEATURES (Auto configuration with the original BIOS's timing
  table per bus clock), POWER MANAGEMENT (APM), PNP/PCI CONFIGURATION, INTEGRATED PERIPHERALS,
  password, BIOS and setup defaults; menus HARDWARE INFORMATION, BENCHMARKS and HARDWARE TESTS.
- Boot devices: hard disk (IDE, CHS-only up to LBA48), USB disk, network (card boot ROM), floppy,
  CD-ROM. ESC at the prompt (or F8 in setup): boot menu.
- USB 1.1/2.0 keyboard and mass storage (UHCI, OHCI, EHCI PCI cards, hubs).
- ISA PnP cards configured at POST, PnP BIOS with ESCD, ISA option ROMs.

Benchmarks (BENCHMARKS, inspired by SpeedSys, VGA 640x480):
- CPU: integer (MIPS) and FPU (MFLOPS) bars against Intel and AMD 486-class CPUs.
- Memory speed: read, write and move over 37 block sizes from 4 KB to 2 MB (L1, L2 and DRAM steps).
- Hard disk: seek times (maximal, random, track to track), buffered and linear read, access time across
  the disk, for any IDE or USB disk; speed bars against the ATA PIO mode limits.

Diagnostics (the tests are in HARDWARE TESTS):
- MEMORY TEST: the default pass of memtest86+ 4.20 (tests 0-8), reimplemented in the BIOS, endless
  passes on the free RAM, errors with address, expected and read values and bits. F: bit fade test
  (each pattern left alone 5 minutes: weak cells, too slow a refresh).
- BOARD TEST: each IRQ line raised by its device on the board (timer, keyboard controller, serial
  ports, floppy controller, RTC and the slave PIC cascade, FPU error, IDE channels), the PIC masks,
  the PIT channels, the RTC crystal against the 14.318 MHz clock, the DMA controller's registers,
  a floppy read by DMA (F), the IDE cable's 16 data lines (WRITE/READ BUFFER), the whole video
  memory (VESA BIOS, else the 256K of the VGA planes), PCI error bits, UART scratch registers,
  CMOS RAM, ISA card ROMs and RAM in C8000-DFFFF, the ISA I/O ports that answer (I), the keyboard
  controller's line test. A hang there leaves its POST code (C8-CF).
- HDD SURFACE SCAN: every sector of an IDE hard disk read and checked by the drive (VERIFY), the
  bad ones listed; ESC stops it.
- CACHE TEST: the L2 cache with each write policy, timing and tag width, read speed in the L2.
- L2 SRAM TEST: each L2 chip tested in place; says which data chip is bad or missing, which tag RAM bit
  is stuck, and checks the dirty bit.
- L2 guard: an L2 that crashes the POST is left off at the next POST, which says why; the setting is
  kept, so a repaired L2 works again by itself.
- POST codes on port 80h and on COM1 (115200 8N1), unfinished POST reported at the next boot.

Information (HARDWARE INFORMATION):
- CPU, clock, L1/L2, memory, each SIMM socket (size, sides; "not used": a SIMM there was left out),
  chipset revisions.
- PCI device list (vendor, class, interrupt pin and IRQ), USB device list, ISA resources, option ROMs
  and IRQ 0-15 map, IDE drives (model, size, geometry, PIO mode).
- The CHIPSET FEATURES options as the chip has them, C8000-DFFFF shadow RAM or ISA by 16K, and the
  register dump (UM8881F, UM8886BF, IDE, UM8663BF super I/O).


Notes for this board
--------------------
- Halt On (STANDARD CMOS): with a dead CMOS battery or no keyboard, set it to No Errors so that the
  boot goes on after the warnings.
- A dead CMOS battery loads the defaults at each power-on (external cache on): with a bad L2 the first
  POST crashes once, the next one runs without it.


What we improved
----------------
coreboot (coreboot_486):
- Setup values applied to the chipset, external cache and PCI IRQs, with a fallback to safe chipset
  values after two unfinished POSTs (no CMOS clearing needed after bad timings).
- Auto configuration from the original BIOS's table, from the measured core clock and multiplier.
- PCI devices turned off before DRAM sizing (a warm reset sized a video framebuffer as RAM), both IDE
  channels, PIRQ router found on both UM8886 revisions, POST codes from the first instruction.
SeaBIOS:
- The setup utility, POST screen, memory test, benchmarks and diagnostics above.
- POST errors and Halt On, CMOS boot order, floppy types and swap, password.
- ATA: CHS-only drives, PIO mode per drive with the UM8886BF timings, block mode and LBA48.
- ISA option ROMs run in place, shadow write protection after POST, ISA PnP, PnP BIOS and ESCD.
- USB: an EHCI left running by a warm reboot is reset instead of skipped.

Tools
-----
- flashrom adapted for this chipset (UM8886BF/UM8881F flash write enable, the 1 Mbit socket's alias
  handled) so anyone can update the BIOS from Linux: bios-update BIOS.bin. The patch is in the
  repository (patches/buildroot/flashrom).
- 86Box adapted to emulate this board better: the UM8881F DRAM and L2 cache (with faulty chips),
  the Intel 28F001BX flash and its write enable, the RTL8139 boot ROM socket and EEPROM, UHCI fixes,
  a dead CMOS battery, a POST card log. Every release passes its test suite in this emulator first.


License
-------
coreboot GPL-2.0, SeaBIOS LGPL-3.0, the mb-8433UUD scripts and documents GPL-3.0. The full sources and the
patches are in the repository above.
