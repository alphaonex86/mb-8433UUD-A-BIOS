# Award 4.50PG (UUD960306I) reverse engineering - findings for newBIOS

Image: `BIOSORIGINAL.bin` (md5 e0ffa2aa98801fec9d1041df4a4b0272). Modules extracted with the local `7z`
(LZH support, CRC checked): `original.tmp` (lh5, 131072 B, flash 0x00000) and `ncr306.16k` (lh5, 16384 B,
flash 0x109D2). `original.tmp` maps to E0000-FFFFF: file offset 0x00000-0x0FFFF = E000:xxxx,
0x10000-0x1FFFF = F000:xxxx. All addresses below are segment:offset in that image unless "flash" is said.
Same work done on `uud960326s.bin` (= `lastbios.bin`, 03/26/96) and `UUD0520S.BIN` (4.51PG, 05/20/96).

Tags: **[V]** verified in the image (bytes/disassembly read), **[I]** inference, **[S]** checked in newBIOS /
coreboot source only (not run).

Scripts used (not included): `d16.py` (16-bit disassembler on original.tmp), `sl.py` (same, follows Award's
stackless `mov sp,ret / jmp` calls), `defs2.py <original.tmp>` (setup item tables with both default sets),
`items2.py`, `rec.py`, `xref.py`. Outputs: `defs_0306.txt`, `defs_0326.txt`, `defs_0520.txt`, `items_*.txt`.

---------------------------------------------------------------------------------------------------------

## 1. Prioritized list: what newBIOS lacks or does differently

### P1 - real-board risk / wrong behaviour

**1.1 "Auto Configuration" in Award is a table indexed by the bus clock; newBIOS Auto is one fixed, more
aggressive set.** [V]
- Pointer table F000:5A25 (24 words: 12 speed classes x {1 bank, 2 banks} L2), labels at F000:5A54
  (`16MHZ 20MHZ 25MHZ 33MHZ 40MHZ 50MHZ 50DX2 66DX2 80DX2 75DX4 100DX4 120DX4`, each `1B`/`2B`). Each entry
  points to 4 records `02 reg devfn mask 00 value 00` (F000:5B48-5CCF). 1B and 2B rows are identical.
  Values depend only on the bus clock:

  | Bus clock | HB51[7:4] DRAM R/W WS | HB50[5:4] L2 WS | IBC56[3:0] KBC / ISA clock | IBCA4[1:0] Host:PCI |
  |---|---|---|---|---|
  | 16-25 MHz (also 50DX2, 75DX4) | F = 0 WS / 0 WS | 11 = 2-1-1-1 | 0 = 7.16 MHz / PCICLK/3 | 01 = 1:1 |
  | 33 MHz (66DX2, 100DX4) | A = 1 WS / 1 WS | 01 = 3-1-1-1 | 1 = 7.16 MHz / PCICLK/4 | 01 = 1:1 |
  | 40 MHz (80DX2, 120DX4) | A = 1 WS / 1 WS | 00 = 3-2-2-2 | 0 = 7.16 MHz / PCICLK/3 | 10 = 1:2/3 |
  | 50 MHz | 0 = 3 WS / 3 WS | 00 = 3-2-2-2 | 0 | 00 = 1:1/2 |

  Option-list order used for decoding (F000:602B `3 WS,2 WS,1 WS,0 WS`; F000:60A9 `3-2-2-2,3-1-1-1,2-2-2-2,
  2-1-1-1`; F000:62C0 `1:1/2, 1:1, 1:2/3`) agrees with `award_map.txt` (e.g. 51 a6->e6 = 0 WS).
- Bus class: E000:3B86-3BC8 divides the measured CPU MHz by the multiplier flagged in [bp+3Fh]
  (bit5 = x4, bit7 = x3, bit6 = x2) and scans the 8 thresholds at E000:3BCA (66/60/50/40/33/25/20/16 MHz). [V]
  In 86Box with the Am5x86-133, the Award result is the 33 MHz row: the oracle baseline (51=a6, 50=d4,
  56=01, a4=81) matches it. [V]
- newBIOS Auto = coreboot romstage (`um8881/romstage.c`): HB51=E4 (DRAM **read 0 WS**, write 1 WS),
  HB50[5:4]=11 (**L2 2-1-1-1**), IBC56=01, IBCA4=81 (1:1), whatever the bus clock. [S]
  Differences at 33 MHz: DRAM read 0 WS vs 1 WS, L2 2-1-1-1 vs 3-1-1-1 (coreboot itself comments "TODO
  TEST one freezes benches" on HB51). At 40/50 MHz bus newBIOS keeps PCI at 1:1 (40/50 MHz PCI). [S]
- The unfinished-POST fallback returns to these same coreboot values, so it does not protect against them. [S/I]
- Why it helps: matching Award's Auto (measured bus clock -> row) removes the main real-board timing risk.
  The coreboot author's real-board lspci under Award (`um8881/reg_init.h`) shows HB51=A4/A5 (1 WS read)
  and HB50 = FB or DB (2-1-1-1 or 3-1-1-1), so 1 WS read is what Award ran on real hardware. [V source]

**1.2 EDO mode is on in newBIOS Auto; Award default is off.** [V/S]
- Award: "EDO DRAM install option" (CMOS 38h bit5) default Disabled in both default sets; F000:78EF writes
  HB61[7:6] = C0 only when the option is Enabled, else 00. No EDO auto-detection found (the "EDO DRAM
  installed !" string at F000:7B72 is only printed when the option is set, E000:3D9D -> F000:7B87). [V]
- coreboot romstage writes HB61=C4 (EDO mode bits 7:6 = 11) and newBIOS Auto keeps it. [S]
- Risk [I]: EDO read timing on FPM SIMMs. Real board SIMM type unknown. Also HB61 bit3 (Award "EDO DRAM
  Read Speed", 0 = 4-2-2-2, 1 = 3-1-1-1): C4 = 4-2-2-2 although the coreboot comment says 3-1-1-1.

**1.3 System/Video BIOS cacheable are never set by newBIOS.** [V Award / S newBIOS]
- Award Setup defaults: System BIOS Cacheable = Enabled, Video BIOS Cacheable = Enabled (HB55 bits 5, 0).
  F000:7B10: if C0000 is not shadowed (HB54 bit1 = 0) Video BIOS cacheable is cleared; if HB55 bit5 is set,
  HB58=0F and HB59=00 (the oracle baseline shows 58=0f 59=00; reset values ff/0f). [V]
- coreboot `northbridge.c:273` (live code in `mp_init_cpus`, ramstage) does
  `pci_update_config8(HB55, ~0x3f, 0)` after romstage applied the setup values: HB55 bits 5 and 0 end up 0
  whatever the setup says; HB58/59 stay FF/0F. Nothing later sets them (SeaBIOS shadow.c only touches bit 6).
  `test/chipset.sh` does not check HB55. [S, not run]
- Why it helps: Award runs the F-segment BIOS and the shadowed VGA BIOS from L1/L2; newBIOS does not
  (slower INT 10h/BIOS calls under DOS), and the two setup items have no effect.

**1.4 PCI IRQ pool: newBIOS can route a PCI INT line to IRQ 15 (secondary IDE).** [V Award / S coreboot]
- Award: "1st..4th Available IRQ" (CMOS 52h-53h nibbles) defaults 11, 10, 9, 12 in both sets [V];
  routing bitmap in the B10E table = 0x1EB8 (IRQ 3,4,5,7,9,10,11,12) [V]; never 14/15. Real board under
  Award (coreboot author's lspci): IBC43=9A, IBC44=CC (INTA 9, INTB 10, INTC/INTD 12). [V source]
- coreboot `mb8433/irq_tables.c` IRQ_MASK 0x8E00 (9,10,11,15); `pirq_route_irqs` gives every link an IRQ
  even with no device, so link 4 (INTD of slot 1, INTC of slot 2, INTB of slot 3) is IRQ 15 while the
  secondary IDE channel (enabled by default) uses IRQ 15. [S]
- Fix idea [I]: same pool as Award (11,10,9,12) or drop 15 from the mask.
- Related [V]: Award disables the chipset's PS/2-mouse IRQ12 function when no mouse is found (F000:7B48:
  BDA 410h bit2 = 0 -> IBC40 bit4 = 0, IBC45 bit0 = 0), which is what makes 12 usable for PCI.
  coreboot sets both bits unconditionally. [S]

### P2 - parity features the user can see

**2.1 POST error reporting, "Halt On" and the F1 prompt.** [V]
- Message groups (E000:33C4, format: status byte offset in the POST data area, then mask + string pointer
  pairs; displayed by E000:338E):
  - [bp+15Ah] bits 80/40/20/10/08: `Hard disk(s) fail (80)`..`(08)`; bit 01: `Keyboard is locked out - Unlock the key`
  - [bp+159h] bits 80/40: `Floppy disk(s) fail (80)`/`(40)`
  - [bp+0Eh] bit 02: `Keyboard error or no keyboard present`; bit 04: `Display switch is set incorrectly.`;
    bit 40: `CMOS checksum error - Defaults loaded`; bit 80: `CMOS battery failed`
  - [bp+158h] bit 01: `Memory test fail`
  - [bp+3Fh] bit 04: `Override enabled - Defaults loaded` (trigger not found, see 4)
- Then `Press F1 to continue, DEL to enter SETUP` and E000:3340 waits for F1 (3B00h) or DEL (5300h).
- "Halt On" (CMOS 3Eh bits 2:0): 0 All Errors (default), 1 No Errors, 2 All But Keyboard, 4 All But
  Diskette, 6 All But Disk/Key (3, 5 invalid).
- newBIOS only prints `CMOS settings invalid - defaults loaded` (setup.c). Cheap and useful: CMOS battery
  failed (RTC reg D bit7), keyboard missing, drive failures. Keep the default non-halting for a headless
  netboot box [I].

**2.2 ISA PnP card configuration at POST, PnP BIOS runtime, ESCD.** [V]
- ISA PnP isolation/configuration code E000:5393-55F7 (ports 279h, A79h); strings
  `Initializing Plug and Play Cards...`, `Card-00`, `Updating ESCD ... Success/Fail`; option
  "PnP BIOS Auto-Config" (CMOS 50h bit4, default Enabled).
- PnP BIOS header `$PnP` F000:C530, real-mode entry F000:C566, functions 00, 01, 02, 03-05, 09, 0A, 0B
  (06-08 unsupported), 40 (ISA config structure), 41-43 (ESCD info/read/write). ESCD lives in the flash
  4 KiB parameter block (flash 0x1D000).
- newBIOS: SeaBIOS PnP BIOS answers only function 60h (`pnpbios.c`). [S]
- Impact [I]: DOS drivers for ISA PnP cards (sound, NE2000 PnP) need the card configured by the BIOS (or
  a DOS PnP tool). The real board's ESCD lists no ISA PnP card (see 3.6), so this only matters if one is
  added. Win9x/Linux configure ISA PnP themselves.

**2.3 Two default sets: LOAD BIOS DEFAULTS (fail-safe) vs LOAD SETUP DEFAULTS.** [V]
Fields: record +17 = BIOS default (in register-mask units), +19 = Setup default (CMOS-mask units).
Items whose two defaults differ (0306):

| Item | BIOS (fail-safe) | SETUP |
|---|---|---|
| Quick Power On Self Test | Disabled | Enabled |
| Gate A20 Option | Normal | Fast |
| L1 Cache Update Scheme | Wr-Through | Wr-Back |
| System BIOS Cacheable | Disabled | Enabled |
| Video BIOS Cacheable | Disabled | Enabled |
| Slow Refresh (1/4 Freq) | Disabled | Enabled |
| PCI Posted Memory Write | Disabled | Enabled |
| Onboard Serial Port 2 | COM2/2F8 | COM4/2E8 |
| Onboard Parallel Mode | ECP/EPP | SPP |
| Drive A | None | 1.44M, 3.5 in. |
| Power Management | User Define | Disable |
| VESA Slave / IDE / VGA Access Check | Enabled | Disabled |
| I/O Region Access Check | Disabled | 1F0h-1FFh |

newBIOS has only Setup defaults. Main menu records (F000:3A67) contain both LOAD BIOS DEFAULTS and LOAD
SETUP DEFAULTS at the same position (row 14, col 5): only one is shown (which one: not determined).

**2.4 Serial port 2 default = COM4/2E8 in this version.** [V]
- Setup default of "Onboard Serial Port 2" (CMOS 7Ah word, mask 7000h) is value 4 = COM4/2E8; the oracle
  baseline agrees (chip-9 shows COM4/2E8 with no SIO change) and the real board's ESCD records a serial
  port at 2E8h IRQ3 (see 3.6). newBIOS default is 2F8. Award offers COM1-COM4 on both ports
  (newBIOS: 2 choices per port). The 05/20/96 BIOS changed this default to Disabled.

**2.5 IDE block mode (READ/WRITE MULTIPLE).** [V]
- INT 13h disk entry F000:A404, function table F000:A50C. At F000:A98E, if the per-drive multiple count
  (BDA 40:AD-40:B0) > 1, command 20h becomes C4h and 30h becomes C5h (`shr 4; add C2h`).
- "IDE HDD Block Mode" (CMOS 11h bit6) default Enabled in both sets. newBIOS: not implemented (README).
  Benefit [I]: fewer DRQ handshakes in DOS disk I/O; small for SeaBIOS's polled PIO.

**2.6 Hard disk type table and User type.** [V]
- 47-entry fixed disk table at F000:0326 (16-byte AT format): 1: 306/4/17, 2: 615/4/17, 3: 615/6/17,
  4: 940/8/17, 5: 940/6/17, 6: 615/4/17, 7: 462/8/17, 8: 733/5/17, 9: 900/15/17, 10: 820/3/17,
  11: 855/5/17, 12: 855/7/17, 13: 306/8/17, 14: 733/7/17, 15: reserved, 16: 612/4/17, 17: 977/5/17,
  18: 977/7/17, 19: 1024/7/17, 20: 733/5/17, 21: 733/7/17, 22: 733/5/17, 23: 306/4/17, 24: 977/5/17,
  25: 1024/9/17, 26: 1224/7/17, 27: 1224/11/17, 28: 1224/15/17, 29: 1024/8/17, 30: 1024/11/17,
  31: 918/11/17, 32: 925/9/17, 33: 1024/10/17, 34: 1024/12/17, 35: 1024/13/17, 36: 1024/14/17,
  37: 1024/2/17, 38: 1024/16/17, 39: 918/15/17, 40: 820/6/17, 41: 1024/5/17, 42: 1024/5/26,
  43: 809/6/17, 44: 809/6/26, 45: 776/8/33, 46: 684/16/38, 47: User (zeros).
- Standard CMOS shows CYLS HEAD PRECOMP LANDZ SECTOR MODE; IDE auto-detect offers NORMAL/LBA/LARGE with the
  note "Some OSes (like SCO-UNIX) must use NORMAL for installation". newBIOS has Auto/None only.
  Useful only for non-IDENTIFY controllers (MFM/RLL/ESDI on ISA) or forced geometry [I].

### P3 - low priority, cosmetic or informative

- **3.1 Labels/layout** [V]: main menu (F000:3A67) left: STANDARD CMOS SETUP, BIOS FEATURES SETUP, CHIPSET
  FEATURES SETUP, POWER MANAGEMENT SETUP, **PCI/GREEN FUNCTION SETUP**, LOAD BIOS/SETUP DEFAULTS; right:
  **PASSWORD SETTING**, IDE HDD AUTO DETECTION, SAVE & EXIT SETUP, EXIT WITHOUT SAVING. HDD LOW LEVEL FORMAT
  strings exist but have no menu record. FDC/serial/parallel are on the CHIPSET page. Status-line help
  texts: "Time, Date, Hard Disk Type...", "Virus Protection, Boot Sequence...", "AT clock, DRAM
  timings...", "Sleep timer, Suspend timer...", "IRQ Settings, Latency Timers...", "Change/Set/Disable
  Password", "Auto-Configure HDD: Sector, Cylinder, Head...", "Save Data to CMOS & Exit SETUP",
  "Abandon all Datas". Standard CMOS also has Video (CMOS 14h bits 5:4), Halt On, Daylight Saving.
- **3.2 CPU name** [V]: table E000:4120 indexed by CPU type/2; the Am5x86 (type 52h) prints
  `Am5x86-P75` (newBIOS: `Am5x86-WB`). Also `Enhanced Am486DX2/DX4`, `Cx5x86`, `U486..`, `TI486..`, `P24T/D`.
  At POST code 0Dh (E000:2205) an Am5x86 gets its L1 enabled early via CMOS 3Dh (F000:E4BD).
- **3.3 BIOS identity** [V]: F000:FFF5 `03/06/96`, model FCh; INT 15h C0 table F000:E6F5 = `08 00 FC 01 00
  74 00 00 00 00` (submodel 01, feature1 74h). newBIOS: SeaBIOS `06/23/99`, submodel 00 [S]. Old OS/driver
  date checks could differ [I].
- **3.4 INT 15h** [V] (F000:D4E0): 24xx (A20), 4F hook (Ctrl+Alt+Gray-/Gray+ speed switch, F000:D5AE ->
  9428), 53 (APM, see 3.5), 80-91 table, 87, 88, 89, C0, C1, C2 (PS/2 mouse), E820. No E801. No INT 13h
  extensions (no AH=41h): newBIOS is a superset.
  E820 template F000:E5C0 (5 x 20 B): 0-9FC00 usable; 9FC00 len 400h reserved (1 KiB EBDA);
  F0000 len 10000h reserved (E0000 not listed); 100000 len patched at POST usable; FFFF0000 len 10000h
  reserved.
- **3.5 APM** [V]: version 1.1 (AX=0101h, CX=7: 16/32-bit PM, CPU idle slows clock), functions 00-0Fh,
  real data in shadow F000:DFC0. newBIOS: APM 1.2 [S].
- **3.6 Real-board ESCD (flash 0x1D000, 351 B, ver 2.1, 3 boards)** [V]:
  - slot 0 (system board): PIC IRQ2 20-21/A0-A1; DMA4 00-0F,81-83,87,89-8B,8F,C0-DF; timer IRQ0 40-43;
    RTC IRQ8 70-71; KBC IRQ1 60/64; 61; FPU IRQ13 F0-FF; FDC IRQ6 DMA2 3F2,3F4-3F5,3F7;
    serial IRQ4 3F8-3FF; **serial IRQ3 2E8-2EF**; parallel IRQ7 378-37B.
  - slot 16: onboard PCI 00:12.0 (1060:886A) and 00:12.1 (1060:673A), IRQ14+15, 1F0-1FF, 3F6, 170-17F, 376.
  - slot 17: **PCI devfn 18h = 00:03.0, 1023:9440 (Trident TGUI9440)**, ports 3B0-3BB, 3C0-3DF, no IRQ.
  So the first PCI slot is device 3 (as coreboot/86Box), the real VGA is a TGUI9440, and no ISA PnP card
  was present when it was written.
- **3.7 PCI BIOS** [V]: 2.10, mechanism 1 (F000:B810: AL=01,02,03,06,08-0F); BIOS32 `_32_` F000:B800.
  No `$PIR`. B10E copies a static 3-entry table (F000:C1C0, length F000:C1F0 = 30h): devices **1Bh, 1Ch,
  1Dh** (slots 1-3), links 1234/2341/3412, bitmap 1EB8h. The device numbers are wrong for this board
  (ESCD shows the slot device at 3), so Award's B10E data is not a reference; the rotation matches
  coreboot's `irq_tables.c`. B10F accepts pins 0Ah-0Dh.
- **3.8 Fixed chipset table ("CT", F000:5967, 27 records `02 reg devfn mask 00 value 00`), identical in
  0306/0326/0520** [V]: HB 51&08=00, 54&FE=00, 56=FF, 57=0F, 5A=0C, 5B=00, 5D&60=20, 5E=00, 5F=FF,
  60&3F=00, 61&0F=04; IBC 04&0F=0F, 40=01, 41=42, 42=08, 46&AF=80, 47&4F=40, 50=00, 57&38=38, 71&01=01,
  90&02=02, 92&1F=1F, A0&FC=00, A1=80, A4&88=80, A8=28. Compared with coreboot: consistent (later Award
  steps change 40/46/47/50/57 to the values coreboot uses). Only new register: HB5Bh=00, which 86Box and
  the real chip report as 2Ch (read-only) - nothing to do.
- **3.9 Early reset init** (F000:F600, stackless) [V]: ELCR 4D0/4D1 = 0; IBC A0=0; HB54=HB55=0;
  IBC56 &= 9F; IBC47 &= F0; IBC57 |= 30; if HB rev (08h) = 4: HB62 |= 01; Cyrix CCR C1h &= F9 (ports
  22h/23h). ELCR bit set per level PCI IRQ (F000:1E00). coreboot does the same where relevant [S].
- **3.10 IDE function** [V]: F000:771C: 00:12.1 cmd = 01, reg41 = 8D | 40 (secondary, from "IDE Second
  Channel Control"), reg42 = 33 (UM8886BF detected by IBC rev 0Dh, F000:F771). SET FEATURES 03h with
  08h+mode per drive (E000:789C). coreboot/newBIOS match [S].
- **3.11 "L1 Cache Update Scheme" only acts on Cyrix CPUs** [V]: F000:7BD3 runs only for CPU types
  Cx486S/S2/DX/DX2/Cx5x86: CCR2 bit2 cleared, CR0.NW set, CCR2 |= 06, HB61 |= 02. For AMD, write-back is
  the board jumper. This explains the oracle's "no register change".
- **3.12 Hidden items (present in tables, not on screen) with their fields** [V]: ISA Bus Clock (IBC56
  bits1:0: PCICLK/3, PCICLK/4, /2; default /4), I/O Recovery Time (IBC57 bits1:0: 2/4/8/12 BCLK, default
  2), Keyboard Emulation (IBC57 bit3, default Disabled), EDO DRAM Read Speed (HB61 bit3), Alt Bit in Tag
  SRAM (HB51 bit0: 7+1 / 8+0, default 7+1), L2 Cache Update Scheme (Wr-Back), Boot Up System Speed,
  PCI/VGA Palette Snoop (CMOS 1Bh bit7), Delay Before HDD Detection (CMOS 36h bit7), ECP Mode Use DMA.
- **3.13 Power management** (documented as not implemented in newBIOS) [V]: timers/monitors map to IBC70
  bits3:0 (doze), IBC80 bits3:0 (inactive), activity bits IBC70[6:5], IBC71[7:1], IBC72[0]; Video Off uses
  VBE 4F10h DPMS (signature `SMPD`) or a Tseng-specific path (searches "Tseng" in the video BIOS,
  E000:7B58); HDD standby = ATA IDLE (E3h) with count = minutes x 12, primary channel only (F000:B042).
- **3.14 KBC** [V]: AAh self-test, 60h <- 45h, AEh, A7h; PS/2 mouse probe AD/D4/FF; C0h (key lock);
  vendor command C9h sent when F000:FFED = 1 or 5; D1h output port. NMI texts: `RAM PARITY ERROR - CHECKING
  FOR SEGMENT...`, `I/O CHANNEL CHECK`, `PRESS F1 TO DISABLE NMI, F2 TO REBOOT`; boot failure:
  `DISK BOOT FAILURE, INSERT SYSTEM DISK AND PRESS ENTER`.
- **3.15 Later versions** [V]: 03/26 and 05/20 add CPU names `Am5x86-P75+`, `Am5x86-P90`,
  `PENTIUM OverDrive`, clocks 100DX2/150DX4, a `PNP CONFIGURATION SETUP` page ("System IRQ, DMA
  Assignment", legacy ISA reservation), `OS Select For DRAM > 64MB` (OS2/Non-OS2), IR port options.
  05/20 changes Setup defaults: System/Video BIOS Cacheable, Slow Refresh and Serial Port 2 -> Disabled.

---------------------------------------------------------------------------------------------------------

## 2. Setup item record format (for future work) [V]

25-byte records after each page title: `+0 label ptr, +2 devfn<<8|reg (FFFF = none), +4 register mask,
+6 CMOS index, +7 CMOS mask (16-bit, word access e.g. 7Ah-7Bh), +9 option-list ptr (NUL-separated,
01h prefix = hidden option), +B ?, +D max value, +F col, +10 row, +11 BIOS default (register units),
+13 Setup default (CMOS units), +15 help ptr, +17 flags`. Page starts: BIOS F000:5386, CHIPSET F000:5CD2,
PM F000:66BA/6831, PCI F000:6F13, STD F000:4862. Full decoded lists in `defs_0306.txt`.

## 3. Could not determine

- Whether Award reports APM with the Setup defaults (Power Management = Disable): APM function 00 needs bit0
  of FDFC:0007; its setter was not found. Check: boot Linux under BIOSORIGINAL in 86Box, look for
  `apm: BIOS version 1.1`.
- What sets "Override enabled - Defaults loaded" ([bp+3Fh] bit2): not found in original.tmp or the boot
  block (the E000:220C compare is the Am5x86 CPU type, not a key).
- Which of LOAD BIOS / LOAD SETUP DEFAULTS is visible (same screen position); the oracle screens can tell.
- The ISA PnP resource-allocation policy and the full ESCD memory entries of the VGA (BAR addresses).
- How Award picks the row for bus clocks between the thresholds on real hardware (only the 86Box Am5x86-133
  case is confirmed: 33 MHz row).
- The real board's SIMM type (EDO or FPM), which decides how much 1.2 matters.
- Effect of the 1.3 issue measured on hardware (not run; static source evidence only).
