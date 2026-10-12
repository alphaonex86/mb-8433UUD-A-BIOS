# newBIOS — open replacement BIOS for the Biostar MB-8433UUD-A

Goal: the compatibility of the board's Award BIOS (v4.50PG UUD960306I on the chip):
DOS and Linux 6.18 run as under the original, the RTL8139C socket ROM boots over the network, an
Award-like setup utility (main menu, pages, values) configures the board, the POST shows CPU, memory
test and drives. Developed and tested in a patched 86Box before touching the real board.

Status 2026-10-12: all emulator tests pass (`test/run_all.sh`). Not yet run on the real board. POST codes
for a POST card: [POSTCODES.md](POSTCODES.md); everything that is not C: [ASM.md](ASM.md).

## Does it fit in the 128 KiB flash? Yes

`out/newbios.rom` (CBFS, sizes in bytes):

| Part | Size | Content |
|---|---|---|
| bootblock | 3,264 | reset vector, cache-as-RAM, CBFS lookup |
| romstage | 7,540 | UMC DRAM sizing, L2 cache, PCI quiesce, chipset setup values (XIP) |
| postcar | 8,732 | leaves cache-as-RAM, loads ramstage |
| ramstage | 27,141 | PCI enumeration/resources, IRQ routing, Auto chipset timings, coreboot tables (LZMA) |
| payload | 55,284 | SeaBIOS (USB 1/2 included) + setup utility + POST screen + memory test + ISA PnP (LZMA) |
| other | 436 | CMOS layout |
| **free** | **28,196** | |

One compression method, LZMA: SeaBIOS 38,992 vs 41,486 bytes with zstd, ramstage 36,109 vs 38,587, and the
zstd decoder alone is 31,839 bytes against 7,706 for LZMA. Bootblock and romstage run in place from the
flash and postcar stays uncompressed: the LZMA decoder would cost more in romstage than it saves. The
decoder in postcar and ramstage is compiled for size (`-Os` instead of coreboot's `-Ofast`: 5 KB less,
no measurable boot time difference in 86Box).
The flash ROM has no coreboot text console (about 24 KB of `printk` code and messages; the serial port
stays silent like under Award); it sends POST codes instead, and `DEBUG=1` builds a ROM with the full
logs.

## Why coreboot + SeaBIOS (and not Open Firmware)

- DOS needs the real-mode BIOS services (INT 10h/13h/15h/16h/1Ah, PnP/PCI option ROMs, BBS boot).
  SeaBIOS implements them; Open Firmware (openfirmware.info) is an IEEE 1275 Forth firmware whose x86
  build only "emulates enough legacy BIOS real mode INTs to boot Windows XP" on the OLPC; OpenBIOS (same
  site) must not be used as a real boot ROM. Neither knows the UMC chipset.
- coreboot_486 (pc2005cz) already initializes the UM8881F/UM8886BF (DRAM, L2, shadow, IDE) in auditable
  C on this exact board; seabios_486 is SeaBIOS 1.16.1 with that author's 486 patches.

## POST screen

Text mode 80x25 like the original (one resolution in the whole BIOS), no logo. Board name and release
(build) date, SeaBIOS version and build time at the bottom left, `<CPU> CPU at
<MHz>MHz` (CPUID name, clock measured), `External Cache : nnnK installed`, `Memory Test : nnnnnK OK` (every
free 4 KiB block written/flushed/verified with two patterns, non-destructively; Quick POST tests one dword
per KiB; ESC skips, DEL requests setup), IDE drives detected in parallel (one thread per channel), each
shown as it goes (`Detecting HDD Primary Master   ... <model>` or `None`), then an
Award-style system configuration box (CPU, FPU, clock, memory, L2 size, floppies, serial/parallel ports,
each IDE drive with size, PIO mode and model), the original BIOS's POST errors (`Floppy disk(s) fail
(80)`, `Hard disk(s) fail (80)`, `Keyboard error or no keyboard present`, `CMOS checksum error - Defaults
loaded`, `CMOS battery failed`, `Memory test fail at <4 KiB block>`) and, as Halt On decides,
`Press F1 to continue, DEL to enter SETUP`; then `Press DEL to enter SETUP, ESC for boot menu`. ISA PnP
cards are listed (`Initializing Plug and Play Cards...`, `Card-01: ...`) before the box.

## Setup utility (DEL)

Award layout: main menu in two columns, pages with values, Esc/arrows/PgUp/PgDn/+/-, F5 old values,
F7 defaults, F10 save & reboot (Y/N), Esc in the main menu quits without saving.

| Page | Options | Applied by |
|---|---|---|
| STANDARD CMOS SETUP | date, time; per IDE drive TYPE (Auto, None, the AT types 1-46, User with typed cylinders/heads/sectors: a fixed type is the INT 13h geometry) and MODE (Auto/Normal/LBA/Large) with size and CHS; floppy A/B; Halt On (All Errors, No Errors, All But Keyboard/Diskette/Disk-Key); memory sizes | SeaBIOS (RTC, ATA, INT 13h translation, FDC) |
| BIOS FEATURES SETUP | CPU internal cache, external cache, 1st-4th boot device, setup prompt delay, swap floppy, NumLock, typematic rate/delay, security option, Quick POST, IDE HDD block mode (READ/WRITE MULTIPLE), C8000-DFFFF shadow (6 blocks) | SeaBIOS (CR0, keyboard, boot order, ISA ROM copy); coreboot (L2 enable) |
| CHIPSET FEATURES SETUP | Auto configuration (the original BIOS's table: DRAM/L2 wait states, ISA and PCI clocks per bus clock 25/33/40/50 MHz, from the measured core clock and the multiplier of the CPU; System/Video BIOS cacheable stay settable); DRAM read/write wait states, L2 wait states, KBC clock, EDO, early cache write, system/video BIOS cacheable, host-to-PCI post write and burst, PCI bus park, PCI posted write, slow refresh, host/PCI clock, burst copy-back, preempt PCI master, IBC DEVSEL# | coreboot romstage |
| POWER MANAGEMENT SETUP | APM BIOS, HDD standby timer | SeaBIOS |
| PNP/PCI CONFIGURATION | PCI INTA#-INTD# IRQ (Auto or 3-15), IDE PIO mode per drive (Auto, 0-4), IDE 32-bit transfer, PnP BIOS Auto-Config (ISA PnP cards get free I/O, IRQ and DMA at POST; off: left inactive for the OS) | coreboot (PIRQ), SeaBIOS (UM8886BF timings, SET FEATURES) |
| INTEGRATED PERIPHERALS | onboard IDE primary/secondary, FDC, serial 1/2 (COM1-4 or off), parallel (378/278 or off) and mode (SPP/EPP/ECP) | SeaBIOS (UM8886BF, UM8663BF) |
| HARDWARE INFORMATION | CPU (CPUID vendor/family/model/stepping/features, Socket 3 names incl. Cyrix DIR0), clock, L1/L2, DRAM, chipset revisions; PCI devices with vendor, class, INT pin and IRQ; ports, ISA option ROMs and IRQ 0-15 map; IDE drives (model, size, CHS, translation, PIO used/maximum); chipset register dump (UM8881F, UM8886BF, IDE, UM8663BF); memory read speed line chart (SysSpeed style) over 37 block sizes 4K-2M, measured when first shown, drawn with redefined characters in 8-dot cells on that page only (same 31.5 kHz / 70 Hz signal); L1, L2 and DRAM show as steps on the real board, flat in 86Box, which has no cache timing | read-only |
| MEMORY TEST | endless passes (pass counter, pass and test progress bars) of the default pass of memtest86+ 4.20 (tests 0-8: address walking ones and own address, moving inversions ones & zeros, 8-bit, random and 32-bit patterns, block move, random number sequence, modulo 20), reimplemented on the RAM above 1 MiB that POST does not use; CPU, L2, memory size and the POST read speeds in the header; pass count, time, errors with address, expected/read value, bits and count; ESC back | setup |
| LOAD BIOS DEFAULTS, LOAD SETUP DEFAULTS (also F6/F7 on the pages) | the original BIOS's fail-safe and Setup values, except Standard CMOS | |
| SUPERVISOR PASSWORD, IDE HDD AUTO DETECTION, SAVE & EXIT, EXIT WITHOUT SAVING | as in Award | |

The chipset and super I/O register fields are the ones the original BIOS writes for the same values:
`test/oracle/` changes each item of the original setup in 86Box, saves, reboots and dumps the PCI config
space and the super I/O registers (`award_map.txt`). Chipset Auto configuration (default) keeps the
coreboot_486 author's values; manual values start from the Award manual defaults. If two POSTs in a row
do not complete, coreboot ignores the manual chipset values until a POST completes (recovery from bad
timings without clearing the CMOS).

IDE: PIO mode Auto takes the best mode IDENTIFY reports (words 51/53/64), so old PIO 0-2 drives get slow
timings; the drive gets SET FEATURES and the UM8886BF timing registers get the coreboot_486 values for that
mode (the original BIOS leaves them untouched in 86Box). CHS-only drives (no LBA), LBA28 and LBA48 drives
(> 128 GiB, e.g. a 1 TB SSD on an IDE adapter; LBA48 and its block mode added to the 86Box fork) work.

CMOS: AT standard bytes 10h-2Dh (floppy types, equipment, memory sizes, checksum) plus the newBIOS block
from 40h (one nibble per setting, password hash at 78h, signature 7Bh, checksum 7Ch, unfinished-POST
counter 7Eh), defined in `cmos.h` and shared by SeaBIOS and coreboot. A cleared or Award CMOS loads
defaults.

Not implemented from the Award pages (no effect possible or not needed): virus warning, boot up floppy
seek, gate A20 option, memory parity check, L1 cache update scheme (the original BIOS changes no register
for it), video BIOS shadow (the PCI VGA ROM is always shadowed), video type, power management
timers and wake events (need SMM), slot INT# mapping, PCI IRQ edge/level, assign IRQ for VGA, PCI IDE IRQ
mapping. Not done from the original BIOS: the PnP BIOS runtime functions 00h-0Bh and 40h-43h and the ESCD
(they would write the flash chip at run time; Win9x and Linux find and configure the devices themselves).

## Compatibility with the original BIOS

| Feature | Award | newBIOS | Test |
|---|---|---|---|
| CPU 486/Am5x86 | yes | yes | all |
| DRAM sizing (4 SIMM banks, 4-128 MB SIMMs), L2 128K-1M, 1/2 banks | yes | coreboot_486 | `ram` (same results as the original BIOS on the fork's model) |
| DOS: INT 10/13/15/16/1A, XMS | yes | SeaBIOS | `dos` |
| IDE primary + secondary, PIO 0-4 per drive, CHS-only and LBA drives | yes | yes | `dos`, `setup`, `chipset`, `bigdisk` |
| Floppy A/B, swap | yes | yes | `setup` (detection) |
| PCI option ROMs, VGA (incl. class 0301) | yes | yes | all |
| ISA option ROMs C8000-DFFFF, RAM in their window | yes | yes, run in place | `scsi` (Adaptec AHA-154xCF) |
| ISA shared memory C8000-DFFFF | yes | yes | `dos` (WD8013) |
| NIC boot ROM (RTL8139C socket), PnP/BEV | yes | yes | `pxe` (operator's Etherboot 5.4.4) |
| Network boot built into the BIOS | no | yes: Etherboot 5.4.4 for RTL8139/RTL8169 cards without a boot ROM | `pxebios` |
| Drive A default 1.44M, 3.5 in. (Setup defaults) | yes | yes | `setup`, `errors` |
| Linux boot: memory map, APM, PCI IRQ | yes | yes (E820, APM 1.2) | `pxe` (Linux 6.18.55) |
| Chipset/super I/O setup values | yes | same register fields | `chipset` |
| Password (setup / system) | yes | yes | `password` |
| Built-in NCR 53C810 SCSI BIOS | yes | no (operator has no SCSI) | — |
| USB keyboard and boot (PCI card: UHCI, OHCI, EHCI; hub, mass storage) | no | yes (SeaBIOS) | none: 86Box has no USB devices |
| POST errors, Halt On, F1 prompt; fail-safe defaults | yes | yes | `errors` |
| Hard disk types 1-46 and User | yes | yes | `hddtype` |
| IDE HDD block mode | yes | yes (also LBA48) | `setup`, `dos`, `bigdisk` |
| Auto chipset timings per bus clock | yes | same table | `busclock` (5 CPUs, same values as the original) |
| ISA PnP cards configured at POST | yes | yes (no ESCD, no PnP BIOS runtime) | `isapnp` |
| POST codes on port 80h | yes (Award codes) | own codes, [POSTCODES.md](POSTCODES.md) | `post` |

Conventional memory is 636 KiB instead of 639/640 KiB, chosen by the operator: the UMC shadow
write-protect is global, so SeaBIOS keeps its runtime data below 640 KiB to let C0000-FFFFF be
write-protected after POST like under Award.

## Try it in 86Box

`./run_86box.sh [rom]` rebuilds newBIOS if a source changed (without `rom`) and opens the Qt 86Box fork on a persistent VM in
`vm/` (CMOS, disk and settings kept between runs, flash reloaded from the ROM, RTC not synced to the host).
Hardware: 84xxuuda, Am5x86-133, one 32 MB SIMM and 256 KB L2
(`[UMC HB4(8881F)]` in `vm/86box.cfg`), TGUI9440, FreeDOS disk
on IDE, RTL8139C with the operator's Etherboot ROM (SLiRP DHCP/TFTP: `boot.pxe` → Linux 6.18) and
RTL8169S with the iPXE ROM (DHCP, then `bzImage` → Linux 6.18): set "1st Boot Device = Network" in setup,
ESC at the prompt picks the card. POST card on (codes in the status bar). Drive A 3.5" 1.44M, no drive B;
floppy and hard disk sounds (samples of the 86Box ROM set). The memory read speed chart is
the last page (6/6) of HARDWARE INFORMATION. `BIOSORIGINAL.bin` (a dump of your board's chip) as argument boots
the original BIOS.

## Build

- `./build.sh` → `out/newbios.rom`, the same ROM for the flash and for 86Box. `DEBUG=1` adds
  `out/newbios-debug.rom` (romstage/ramstage logs at coreboot level 7 and SeaBIOS level 1 on COM1; no room
  for the built-in network boot ROM).
- Built-in network boot: `tools/build_etherboot.sh` builds Etherboot 5.4.4 with the rtl8139 and r8169
  drivers in one PCI ROM (`patches/etherboot.patch`, coreboot's cross compiler); `build.sh` adds it to the
  CBFS as `pxe.rom.lzma` (22.6 KB) with `etc/pxe-pci-ids` (the drivers' PCI IDs). SeaBIOS runs it once,
  for the first supported card without its own boot ROM; Etherboot tries each supported card in turn.
- Sources: `fetch_sources.sh` (run by the builds) downloads the pinned trees and tarballs into `sources/`:
  the 486 coreboot and SeaBIOS of their author, [pc2005cz/coreboot_486](https://github.com/pc2005cz/coreboot_486)
  and [pc2005cz/seabios_486](https://github.com/pc2005cz/seabios_486), the Etherboot 5.4.4 tarball; for the tests 86Box 6.0, iPXE,
  FreeDOS 1.3 and Linux 6.18. `tools/build_toolchain.sh` builds coreboot's i386 cross compiler once (it
  downloads GCC and binutils). `sources/coreboot_486` is patched in place (`patches/coreboot.patch`),
  `build/seabios` is a clone of `sources/seabios_486` + `patches/seabios.patch`; SeaBIOS options in
  `seabios.config`, coreboot options in `configs/mb8433_author.config` + `build.sh`, CMOS layout `cmos.h`.
  The patches are the source of truth: missing trees are recreated from them and they are re-exported
  after each build.
- Host: a Linux with gcc, make, git, curl, python3, unzip, nasm (tests), cmake and Qt6 (86Box GUI).

## Tests (86Box fork)

`test/run_all.sh [test...]` builds what is missing and runs the tests in parallel, one 86Box each,
headless, driven through the fork's test FIFO (keys in; text screen, PCI config space and I/O ports
out) and COM1:

| Test | Checks |
|---|---|
| `setup` | defaults on cleared CMOS (drive A 1.44M), all menus, date/floppy/boot order edited and kept after F10, hardware information pages, secondary IDE disk |
| `dos` | POST screen (board, release date, "Detecting HDD" lines, version); FreeDOS 1.3 from IDE (CHS-only drive), HimemX, 636 KiB + 31 MiB XMS, WD8013 shared RAM at D0000; L1 cache on, Auto chipset timings as the original BIOS, no PCI interrupt on IRQ 14/15 |
| `scsi` | Adaptec AHA-154xCF ISA BIOS (real ROM from the 86Box set, RAM in its window), DOS from SCSI |
| `pxe` | DOS disk + the operator's NIC ROM: default order boots DOS; "Network" first → Etherboot → iPXE NBP → Linux 6.18.55 (e820, APM, 8139too, sda) |
| `pxebios` | RTL8139C+ without cable and RTL8169S, no socket ROMs: one network entry (built-in Etherboot), which falls back from the RTL8139 to the RTL8169 → iPXE NBP → Linux 6.18.55 |
| `pxe8169` | no disk, PCI RTL8169S with an iPXE ROM in its socket: "Network" first → iPXE DHCP + TFTP → Linux 6.18.55 with `ip=dhcp` (r8169, link up at 1 Gbps, DHCP lease) |
| `chipset` | manual chipset values, external cache off, PCI INTA# IRQ, PIO mode 2, serial/parallel settings program the original BIOS's register fields; fallback after unfinished POSTs (warning on screen); System/Video BIOS cacheable as the original; internal cache off still off in DOS |
| `password` | password asked before boot (System) and for setup, wrong one refused |
| `bigdisk` | 1 TB LBA48 disk: size, LBA translation, INT 13h extended read beyond 128 GiB, then 64 sectors written and read back there in block mode (READ/WRITE MULTIPLE EXT), checked in the image |
| `post` | POST codes of a boot, setup and its memory test equal the order in POSTCODES.md |
| `errors` | cleared CMOS halts on "CMOS checksum error" until F1/DEL; drive A set up but absent: "Floppy disk(s) fail (80)", no halt with Halt On "All, But Diskette"; LOAD BIOS DEFAULTS keeps Standard CMOS |
| `hddtype` | User type typed in setup is the INT 13h geometry (FreeDOS boots, translation TYPE); type 2 without a drive: "Hard disk(s) fail (80)" |
| `busclock` | Am486DX2-50/80, Am486DX4-120, i486DX-50, Am5x86-133: clock shown and Auto chipset fields equal to the original BIOS's (`test/oracle/busclock.sh`) |
| `isapnp` | RTL8019AS PnP card isolated and given 220h/IRQ 5 (answers there); inactive with PnP BIOS Auto-Config off |
| `memtest` | MEMORY TEST on a 4 MB SIMM: a full pass without errors; with a faulty bit in the fork, POST reports the block and the test the address, values and bit |
| `ram` | 9 SIMM/L2 configurations (one bank to four, double sided, MA11 12-row DRAMs, region swap, bank 1 alone, no L2 to 1 MB in 1 or 2 banks): memory and cache on the POST screen, memory test, chipset fields |

Inputs: `sources/` (`fetch_sources.sh`); the 86Box ROM set ([86Box/roms](https://github.com/86Box/roms) in
`~/.local/share/86Box/roms`: VGA BIOS, Adaptec SCSI BIOS); a boot ROM for the RTL8139 socket in
`test/roms/etherboot-5.4.4-rtl8139.rom` (an Etherboot 5.4.4 rtl8139 ROM of at most 32 KiB, not included);
the PXE NBP and the RTL8169 boot ROM (`test/ipxe/build.sh`); the test kernel (`test/linux/build.sh`, whose busybox and glibc
come from the target directory of the coreboot_486 author's
[buildroot_486](https://github.com/pc2005cz/buildroot_486) build in `sources/buildroot_486/output`).
`test/oracle/sweep.sh`, `sweep2.sh` and `diff.py` regenerate `award_map.txt` from `BIOSORIGINAL.bin` (a
dump of the board's original BIOS, not included); `award_findings.md` is what reading that image taught; `test/oracle/ram.sh <config>` prints
what the original BIOS and newBIOS find on one SIMM/L2 configuration, as a `test/ram.sh` line.

86Box fork (`build_86box.sh`, `patches/86box.patch` on 86Box 6.0): test control FIFO
(`$BOX86_TESTCTL`: `key`, `text`, `pci`, `in`, `out`, `cr0`, `shot` (PNG screenshot), `reset`, `quit`); POST card
log (`$BOX86_POSTLOG`); RTL8139 boot ROM socket
(`roms/network/rtl8139c/rtl8139c.rom`) and MMIO BAR mapped whatever the order of BAR/command writes; PCI
RTL8169S gigabit NIC `rtl8169` with its own boot ROM socket (`roms/network/rtl8169/rtl8169.rom`), a variant
of the RTL8139C+ code (same descriptor rings; chip XID 008, TxPoll 38h, RxMaxSize, PHYAR to a PHY with the
RTL8211 ID, always linked at 1 Gbps full duplex; no jumbo frames, TBI, high-priority queue, Wake-on-LAN); SLiRP
TFTP (`$BOX86_SLIRP_TFTP`, `$BOX86_SLIRP_BOOTFILE`); 386+ CPU-only reset fetches from the ROM (CS base
FFFF0000); UM8886BF multi-function bit; IDE LBA48 (with READ/WRITE MULTIPLE EXT). UM8881 DRAM/L2 model (`chipset/umc_hb4.c`, device
options `bank0`-`bank3` SIMM type, `l2_cache` KB, `l2_banks`, `stuck_addr`/`stuck_bit` faulty RAM bit
read as 0; all banks empty keeps the stock flat RAM):
CPU addresses decoded to SIMM cells as registers 52h/53h/61h program them, MA mapping from the UM8881F
documentation, L2 SRAM answering forced hits (5Ah bit 6), and the CPU's internal cache as RAM (lines read
after INVD kept while CR0.CD and NW are set), so the unmodified coreboot raminit runs in 86Box. On 18
configurations the original BIOS gets the same sizes and register fields as newBIOS 13 times; it rejects a
64 KB L2, sees a 64 MB 12/11 double-sided SIMM as 32 MB and does not start with bank 0 empty or with a
12/10 double-sided and an 11/11 single-sided SIMM in one pair. `GUI=1` also builds the Qt GUI used by
`run_86box.sh`.

## Changes to the upstream trees

SeaBIOS (`patches/seabios.patch`): setup utility, POST screen and memory test (`src/setup.c`); CMOS boot
order, prompt delay, password; floppy types and swap; ATA: CHS-only drives, PIO mode per drive with
UM8886BF timings, drive type None, 32-bit transfer switch, standby timer; translation from setup; APM
switch; XGA/pre-2.0 VGA class accepted as display; AT keyboards that NAK the scan set command; ISA option
ROMs run in place (UMC shadow blocks routed to ISA, writes to the bus while they run) or copied when
shadowed in setup, unused C8000-DFFFF blocks back on the ISA bus; runtime data below 640 KiB with the
shadow write-protected after POST; port 92h reset with a 0→1 edge; POST codes B0h-BDh; the temporary low
memory zone limited to conventional RAM (this port's E0000 EBDA minimum let it reach C8000-DFFFF, which
goes back to the ISA bus after the option ROMs); CR0.CD/NW kept across mode switches and set from setup
after a CPU-only reset (the "CPU Internal Cache" option); USB 1/2; POST errors with Halt On (floppy
recalibrate check, PS/2 keyboard flag); IDE block mode (SET MULTIPLE MODE, READ/WRITE MULTIPLE and their EXT forms); the port's
trace prints moved from debug level 1 to 3; fixed
drive types as INT 13h geometry; ISA PnP configuration at POST; CPU name and clock without CPUID; drive
detection lines at POST; built-in network boot ROM from the CBFS (`optionroms.c`, CBFS read enabled);
version string without host name, release date (`scripts/buildversion.py`).

coreboot (`patches/coreboot.patch`): setup values for chipset, external cache and PCI IRQs
(`mainboard/biostar/mb8433/newbios.c`) with the unfinished-POST fallback; PCI devices turned off before
DRAM sizing (after a warm reset a VGA framebuffer BAR was sized as RAM); both IDE channels; PIRQ router
found at 00:12.0 (the IBC reports 886a or 888a); POST codes on port 80h (`include/cpu/486/newbios_post.h`, also from
the assembly, which sent ASCII characters to COM1); no `printk` in romstage/ramstage without a serial
console (`console/Makefile.inc`, `console/console.h`), `printk` above the build log level compiled out;
the raw COM1 dumps and test prints of the port removed; Auto Configuration = the original BIOS's table per bus clock, set in ramstage from the measured core
clock and the multiplier (CPUID, else the reset signature that `cpu/486/entry16.S` keeps in CMOS 7Fh),
romstage starting with its 33 MHz row (DRAM 1 WS, L2 3-1-1-1, no burst copy-back, no EDO: the previous
values froze benchmarks); `DECOMPRESS_OFAST` settable (`lib/Kconfig`), off; System/Video BIOS cacheable kept (with 58h/59h as
the original); PCI interrupts on IRQ 9-11 (15 is the secondary IDE); postcar read loop from ESI.

## Before flashing the real board

- Chip: Intel P28F001BX-T. Back up the board's original BIOS first and verify the dump (e.g. with a T48:
  `minipro -p "P28F001BX-T@DIP32" -r BIOSORIGINAL.bin`, read twice and compare). Program and restore
  (minipro erases and verifies by default): `minipro -p "P28F001BX-T@DIP32" -w out/newbios.rom` /
  `... -w BIOSORIGINAL.bin`.
- Only testable on the board: DRAM/L2 sizing on real SIMMs (the emulated model agrees with the original
  BIOS but is built from the chipset documentation), UMC shadow bits used for ISA ROMs, port 92h reset,
  IDE timings, PCI IRQ routing, manual chipset values (keep Auto configuration first).
- `out/newbios.rom` keeps COM1 silent and sends POST codes ([POSTCODES.md](POSTCODES.md)) for a POST card;
  `DEBUG=1` builds `out/newbios-debug.rom`, which also logs romstage, ramstage and SeaBIOS on COM1 (115200 8N1).

## License

The scripts, tests and documents of this repository: GPL-3.0 (`LICENSE`). The patches in `patches/`
modify, and keep the licenses of, the projects they apply to: coreboot (GPL-2.0), SeaBIOS (LGPL-3.0) and
86Box (GPL-2.0-or-later). No BIOS image, firmware blob or ROM is distributed here.
