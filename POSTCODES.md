# POST codes (port 80h)

newBIOS writes a code to I/O port 80h when each step of the power-on self test **starts**, for an
ISA or PCI POST card. If the board hangs, the card keeps showing the step that did not finish. Every
code is unique. Both ROMs (`out/newbios.rom` and `out/newbios-debug.rom`) send the same codes; the
debug ROM also logs the steps in text on COM1 (115200 8N1).

Both also send each code from coreboot's C stages and SeaBIOS on COM1 (115200 8N1) as one line `XX`:
on a PC with a null-modem cable, `tools/post_time.py /dev/ttyUSB0` prints each with its time (the
board has no clock to time its POST itself).

In 86Box the POST card is on in `run_86box.sh` (codes in the status bar) and in the tests
(`BOX86_POSTLOG`). `test/post.sh` checks that a boot sends exactly the "Normal boot" codes below, in
this order, so this table is the reference.

## Normal boot

| Code | Stage | Step that starts | If it stops here |
|---|---|---|---|
| `01` | reset vector | CPU runs the BIOS at FFFFFFF0h (16-bit real mode) | (no code at all: no CPU clock/power, flash not read or badly programmed) |
| `03` | bootblock asm | IDT and GDT loaded | CPU cannot run code from the flash |
| `04` | bootblock asm | protected mode on (CR0.PE), far jump to 32-bit code | wrong CPU type or GDT |
| `10` | bootblock asm | 32-bit protected mode, segment registers set | |
| `21` | cache-as-RAM | temporary UM8881F DRAM setup so that 1A0000h can be cached | |
| `22` | cache-as-RAM | L1 cache on, reading the cache-as-RAM range into it | CPU internal cache does not work |
| `23` | cache-as-RAM | CR0.CD = NW = 1: the L1 lines become 8 KiB of RAM | |
| `26` | cache-as-RAM | temporary DRAM setup removed | |
| `27` | cache-as-RAM | cache-as-RAM zeroed | |
| `28` | cache-as-RAM | stack in cache-as-RAM, calling the C code | |
| `14` | bootblock | C code running | stack in cache-as-RAM does not work |
| `15` | bootblock | CPU self test (BIST) passed | (stays at `14`: BIST failed) |
| `16` | bootblock | UM8881F: ROM decode, shadow RAM and caches off | |
| `17` | bootblock | UM8886BF early setup | |
| `18` | bootblock | CPU early setup | |
| `19` | bootblock | done, romstage looked up in the flash (CBFS) | flash content damaged |
| `30` | romstage | UM8881F register defaults | |
| `31` | romstage | PCI devices turned off before DRAM sizing | |
| `32` | romstage | DRAM banks sized (UM8881F 52h/53h/61h) | SIMMs: none, bad, unsupported or badly seated |
| `33` | romstage | L2 cache sized and enabled | L2 SRAM or tag RAM, cache jumpers |
| `35` | romstage | UM8886BF early init | |
| `37` | romstage | chipset values from setup (CMOS) | too fast manual timings: after 2 unfinished POSTs the defaults are used |
| `38` | romstage | ROM segment E copied to shadow RAM, C-F shadow RAM on | |
| `3A` | romstage | CBMEM at the top of RAM | RAM at the top of memory (last bank) |
| `3B` | romstage | done, postcar loaded | |
| `3C` | postcar | running, still on the cache-as-RAM stack | |
| `3D` | postcar | cache-as-RAM left, L1 cache normal; then ramstage is loaded (LZMA) | first use of DRAM as stack and code |
| `13` | ramstage | C start code | |
| `6E` | ramstage | before the main function | |
| `39` | ramstage | main function, console | |
| `6F` | ramstage | hardware main | |
| `70` | ramstage | boot state: before devices | |
| `71` | ramstage | chips initialized (UM8881F, UM8886BF) | |
| `72` | ramstage | devices enumerated | |
| `24` | ramstage | PCI bus scan | a PCI card blocking configuration cycles |
| `25` | ramstage | PCI bus scan done | |
| `73` | ramstage | resources (BARs, I/O, memory) allocated | |
| `74` | ramstage | devices enabled | |
| `75` | ramstage | devices initialized (IDE, PCI IRQ routing) | |
| `76` | ramstage | after devices | |
| `77` | ramstage | resume check (no S3 on this board) | |
| `79` | ramstage | coreboot tables written (memory map for SeaBIOS) | |
| `9A` | ramstage | PCI IRQ routing table ($PIR) | |
| `7A` | ramstage | SeaBIOS loaded from the flash (LZMA) | |
| `7B` | ramstage | jumping to SeaBIOS | |
| `F8` | ramstage | SeaBIOS entry point called | |
| `B0` | SeaBIOS | POST entry, memory map from coreboot | |
| `B1` | SeaBIOS | interrupt controller, timer, keyboard controller, PCI, BIOS tables | |
| `B2` | SeaBIOS | video card option ROM | video card or its ROM |
| `B3` | SeaBIOS | POST screen (text mode 80x25) | |
| `B4` | SeaBIOS | keyboard, USB, floppy and IDE drives detected | a drive that does not answer |
| `D0` | SeaBIOS | USB setup starts (its threads run on beside the next steps: `D1`-`D6`) | a USB controller |
| `B5` | SeaBIOS | POST memory test | (the screen shows the memory count) |
| `C1` | SeaBIOS | L2 init: L1 off, WBINVD (the POST screen's cache line shows each step) | |
| `C2` | SeaBIOS | L2 init: write-through fill of the scratch range | L2 SRAM, tag RAM or their timing |
| `C3` | SeaBIOS | L2 init: tag checks | |
| `C4` | SeaBIOS | L2 init: write-back fill | |
| `C5` | SeaBIOS | L2 init: caches back on (also after a failed check: no `C4` then) | |
| `B6` | SeaBIOS | ISA PnP cards configured, then option ROMs: ISA cards (C8000-DFFFF), network boot ROM | a PnP card or an option ROM |
| `B7` | SeaBIOS | system configuration box | |
| `B8` | SeaBIOS | POST errors (Halt On: `Press F1 to continue`), then `Press DEL to enter SETUP, ESC for boot menu` | (waits for F1/DEL, the keys or the boot delay) |
| `BB` | SeaBIOS | boot preparation: setup values, tables, BIOS write-protected | |
| `BC` | SeaBIOS | booting a device (INT 19h); one per device tried | boot sector or operating system |

## Other codes

| Code | When |
|---|---|
| `02` | CPU-only reset (CMOS 0Fh not 0, e.g. a program returning from protected mode): jump to SeaBIOS, which resumes the program |
| `B9` | setup utility (DEL) |
| `BA` | MEMORY TEST in setup |
| `C1`-`C7` | CACHE TEST in setup: its L2 steps (`C6` speed test, `C7` L2 off) |
| `D1` / `D2` | an EHCI (USB 2.0) controller starts / ends checking its ports |
| `D3` / `D4` | a UHCI (USB 1.1) controller starts / ends checking its ports |
| `D5` | a USB device configured (keyboard, disk, hub...) |
| `D6` | a USB or SCSI disk answered "ready" (TEST UNIT READY) |
| `BD` | no bootable device: the message is on screen, retry after 60 s |
| `E0` | coreboot: a stage or SeaBIOS was not found or not loaded (flash content) |
| `EE` | coreboot: code that must not return did return |

A POST that does not reach `BB` (hang, reset, power off) is reported by the next one: "The previous
POST did not finish, its last POST code: XX" (CMOS 3Fh keeps the last SeaBIOS code).

Other fatal coreboot errors (bad CPU self test, missing stage) halt and leave the last code shown.

After `BC` the operating system may write port 80h too (Linux uses it as an I/O delay), so other
codes appear once it runs.
