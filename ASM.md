# newBIOS: the parts of the ROM that are not C

This guide covers the parts of `out/newbios.rom` (128 KiB flash) that are not C:

- assembly: `.S` files, which are gas assembler run through the C preprocessor;
- linker scripts;
- build-time generated tables.

It also explains the CPU background you need to read them. All assembly files listed here carry comments.

The facts were checked against this build:

- `coreboot_486/build-newbios`, the release configuration `out/newbios.config`;
- `build/seabios/out-rel`, with its maps, objects and disassembly.

Where something is not certain, the text says so.

## 1. Reading x86 boot assembly (for C programmers)

- **Syntax (AT&T, as used by gas).**
  - Operand order is `op source, destination`: `movl %eax, %ebp` means `ebp = eax`.
  - `%` marks a register and `$` an immediate value. A suffix gives the size: `b` = 8, `w` = 16, `l` = 32 bits.
  - `(%esi)` is the memory at the address in ESI, and `4(%esp)` the memory at ESP+4.
  - `%cs:(%bx)` uses the CS segment instead of the default one.
  - `ljmp $sel, $off` is a far jump: it loads CS too.
  - `rep stosl` stores EAX to ECX dwords at EDI, and `rep lodsl` loads ECX dwords from ESI. Both count upwards, because DF = 0 after `cld`.
- **Real mode** is the CPU state after reset and the mode DOS runs in.
  - An address is `segment * 16 + offset`, with 16-bit offsets, so 1 MiB of address space.
  - Interrupts go through the interrupt vector table (IVT) at 0000:0000.
  - After reset, CS has a hidden base of 0xFFFF0000. Execution starts at 0xFFFFFFF0, the top of the flash.
- **Protected mode** is entered by setting CR0.PE.
  - A segment register holds a *selector*: an index into the GDT (global descriptor table). The descriptor gives base, limit and 16- or 32-bit size.
  - coreboot and SeaBIOS use *flat* segments: base 0, limit 4 GiB, so the offset is the physical address.
  - Changing CS requires a far jump.
  - Interrupts and exceptions go through the IDT. An IDT with limit 0 means that any exception ends in a triple fault (CPU shutdown).
  - Paging is never enabled.
- **CR0 cache bits on the 486.**
  - CD = 0, NW = 0: normal caching (write-through L1).
  - CD = 1, NW = 1: no new line fills and no write-through. Lines already in L1 still answer reads and writes.
  - Reset value: CD = NW = 1, with an empty cache.
  - `invd` drops all L1 lines without writing them back. `wbinvd` writes them back first.

## 2. Boot flow

| # | Code | CPU mode | Runs from | Stack |
|---|---|---|---|---|
| 1 | `cpu/486/reset16.S` | real mode | flash, 0xFFFFFFF0 | none |
| 2 | `cpu/486/entry16.S` | real mode, then 16-bit protected | flash | none |
| 3 | `cpu/486/entry32.S` | 32-bit flat (ROM GDT `arch/486/gdt_init.S`) | flash | none |
| 4 | `cpu/486/cache_as_ram.S`, then bootblock C | 32-bit flat, cache-as-RAM | flash | CAR |
| 5 | `arch/486/assembly_entry.S`, then romstage C | 32-bit flat, cache-as-RAM | flash (XIP) | CAR |
| 6 | `arch/486/exit_car.S` (postcar), then postcar C | leaves CAR, caches on | DRAM (cbmem) | postcar .bss |
| 7 | `arch/486/c_start.S`, then ramstage C (IDT from `arch/486/idt.S`) | 32-bit flat, own GDT | DRAM (cbmem) | ramstage .bss |
| 8 | SeaBIOS `entry_elf` (`romlayout.S`), then POST in C | 32-bit flat, SeaBIOS GDT | RAM 0xE82C0-0xFFFFF | 0:0x7000 |
| 9 | SeaBIOS runtime (`romlayout.S`, `entryfuncs.S`) | real mode, switching to 32-bit when needed | F segment (shadow RAM) | caller or ExtraStack |

Paths are relative to `coreboot_486/src` unless the path starts with `seabios`. Hand-offs between stages:

- **Bootblock to romstage.** The bootblock C code (`cpu/486/bootblock.c`, then `lib/bootblock.c`) finds romstage in CBFS and runs it in place. coreboot calls this "prog_run".
- **Romstage to postcar.** Romstage initializes the DRAM, then `arch/486/postcar_loader.c` loads postcar into cbmem, relocates it and calls `_start(cbmem_top)`.
- **Postcar to ramstage.** Postcar loads ramstage, which is LZMA-compressed, the same way.
- **Ramstage to SeaBIOS.** Ramstage loads the SeaBIOS ELF and jumps to its entry point, `entry_elf`.
- **Booting the OS.** SeaBIOS ends POST with INT 19h (`entry_19`), which loads the boot sector and enters it in real mode.

**CPU-only reset.** A CPU-only reset can come from a port 92h fast reset, a keyboard-controller reset or a triple fault. The chipset keeps its configuration, and the CPU restarts at step 1. `entry16.S` then checks the shadow state:

- Both conditions true: E/F shadow read is enabled (UM8881 registers 54h bit 0 and 55h bit 7) and CMOS byte 0Fh (shutdown status) is non-zero.
  - It jumps in real mode to F000:FFF0, which is the SeaBIOS `reset_vector` in shadow RAM.
  - From there the path is `entry_post`, then `entry_resume`, then `handle_resume()` (resume.c), which acts on the shutdown code.
  - `handle_resume()` clears CMOS 0Fh. A plain warm reboot ends in a port 92h reset, which takes the full coreboot path.
- Otherwise: the full coreboot boot.

## 3. coreboot assembly, file by file

Ten of the eleven files below end up in the ROM. `cpu/486/exit_car.S` is built but dropped by the linker. Not built for this board: `thread_switch.S`, `wakeup.S` and `walkcbfs.S` in `arch/486`.

- **`cpu/486/reset16.S`** (bootblock, section `.reset`)
  - The 16 bytes at 0xFFFFFFF0 hold one hand-encoded `jmp rel16` to `_start16bit`.
- **`cpu/486/entry16.S`** (bootblock, `.init._start`)
  - Saves the BIST result (EAX at reset) in EBP.
  - Keeps the CPU signature in DX at reset (family and model, bits 11-4) in CMOS 7Fh: the clock
    multiplier and name of CPUs without CPUID (coreboot ramstage, SeaBIOS).
  - Sends POST code 01h to port 80h.
  - Flushes the TLB (`mov cr3`) and L1 (`invd`).
  - Runs the soft-reset check of §2.
  - Loads an empty IDT (`nullidt`) and the ROM GDT. It uses `lgdtl` with the 16-bit offsets `gdtptr_offset`/`nullidt_offset` computed by `bootblock.ld`.
  - Sets CR0.PE with CD = NW = 1, then far-jumps to `bootblock_protected_mode_entry` with selector 0x08.
- **`cpu/486/entry32.S`** (bootblock, `.init`)
  - Loads DS/ES/SS with the flat data selector 0x10 and the null selector into FS/GS.
  - Jumps to `bootblock_pre_c_entry`.
- **`cpu/486/cache_as_ram.S`** (bootblock, `.init`)
  - Sets up cache-as-RAM (§4).
  - Points ESP at `_ecar_stack`, aligned.
  - Pushes the arguments of `bootblock_c_entry_bist(uint64_t base_timestamp, uint32_t bist)`. The timestamp is 0, because the 486 has no TSC.
  - Calls it. It never returns.
- **`arch/486/gdt_init.S`** (bootblock, romstage, postcar)
  - The ROM GDT: null, 0x08 flat 32-bit code, 0x10 flat 32-bit data, 0x18 64-bit code (unused).
  - `gdt_init()` reloads GDTR from the copy linked into the current stage. The bootblock does not call it: the linker drops it there.
- **`arch/486/id.S`** (bootblock, `.id`)
  - Version, vendor and part strings, their negative offsets from 4 GiB, and the ROM size.
  - It sits just below the reset vector, where flashrom looks for it.
- **`arch/486/assembly_entry.S`** (romstage `_start`)
  - Calls `gdt_init()`.
  - Resets ESP to the top of the CAR stack.
  - Zeroes the stage .bss in CAR.
  - Aligns the stack, then calls `car_stage_entry()` (`arch/486/romstage.c`).
- **`arch/486/exit_car.S`** (postcar `_start`)
  - Calls `gdt_init()` and saves the cbmem_top argument.
  - Zeroes DRAM 0x200000-0x2FFFFF, still in CAR mode.
  - `invd`: the CAR contents are dropped.
  - Clears CD/NW, so the caches are on, then `invd` again.
  - Reads 2 MiB (see §7).
  - Sets UM8881 register 50h bit 6, the L2 write-back mode.
  - Switches to its own stack and calls `main()` (`arch/486/postcar.c`).
  - Sends POST codes 3Ch and 3Dh.
- **`cpu/486/exit_car.S`** (postcar)
  - `chipset_teardown_car()`. Its only call, in `arch/486/exit_car.S`, is commented out.
  - `postcar.map` lists it under "Discarded input sections", so it is **not in the ROM**.
- **`arch/486/c_start.S`** (ramstage `_start`)
  - Loads the ramstage GDT (in .data): 0x10 code, 0x18 data, 16-bit entries 0x28/0x30/0x38/0x40, and one per-CPU data segment.
  - Far-jumps to reload CS, then sets the data segments.
  - Saves `cbmem_top`.
  - Fills the 8 KiB stack with 0xDEADBEEF.
  - Calls `main()` (`lib/hardwaremain.c`).
- **`arch/486/idt.S`** (ramstage)
  - `vec0`-`vec19` push a dummy error code where the CPU pushes none, then the vector number.
  - `int_hand` builds `struct eregs` and calls `x86_exception()` (`exception.c`). If the call returns, it restores the registers and executes `iret`.
  - `exception_init()`, called from `main()`, builds the 20-gate IDT in C.

The ROM GDT uses selectors 0x08 for code and 0x10 for data. The ramstage GDT uses 0x10 for code and 0x18 for data, so the same number means different segments in different stages.

### POST codes

`include/cpu/486/post_code.h` makes `post_code()` in assembly write the code to port 80h, like the C
`post_code()`; the values are named in `include/cpu/486/newbios_post.h`. The reset vector, real mode,
protected mode and cache-as-RAM steps send 01h, 03h, 04h, 10h, 21h-28h, postcar 3Ch/3Dh, ramstage
13h/6Eh. [POSTCODES.md](POSTCODES.md) lists them in boot order with what a hang at each one means.

## 4. Cache-as-RAM on the 486

Before romstage has initialized the DRAM there is no RAM, but C needs a stack and writable data. `cache_as_ram.S` turns the L1 cache into 8 KiB of RAM:

1. **Fake DRAM.** UM8881 registers 52h = 77h and 53h = 0Fh declare the largest DRAM banks.
   - The 486 caches a line only when the chipset asserts KEN#, which it does for DRAM addresses.
   - After this, `CONFIG_DCACHE_RAM_BASE` = 0x1A0000 is "DRAM". Its content does not matter.
2. **Fill.** `invd`, then CD = NW = 0, then `rep lodsl` over 0x1A0000-0x1A1FFF: each miss fills a 16-byte line.
3. **Freeze.** CD = 1, NW = 1.
   - Lines can no longer be replaced, and writes that hit them stay in the cache.
   - Code keeps being fetched uncached from flash.
4. **Clean up.** Registers 52h/53h go back to 0, so there are no DRAM banks again. Then the region is zeroed with `rep stosl`.

`car.ld` defines the layout (this build, see `bootblock.map`):

| Region | Addresses |
|---|---|
| stack | 0x1A0000-0x1A1000, grows down from `_ecar_stack` |
| pre-RAM cbmem console | 0x1A1000-0x1A1C00 |
| timestamps | 0x1A1C00-0x1A1E00 |
| EHCI debug info | 80 bytes, at 0x1A1E00 |
| per-stage .bss | after the EHCI info |
| end of CAR | 0x1A2000 |

Romstage reuses the same layout. Postcar leaves CAR (§3).

Assumptions and limits, not verified on the real board:

- **Code fetches must not be cached.** Between the fill and CR0.CD = 1 (a `post_code` and three instructions), the code fetched from flash must not be cacheable. Otherwise it could evict a CAR line on a CPU whose L1 is exactly 8 KiB (486DX/DX2). With a 16 KiB L1 (DX4, Am5x86) only half of the ways hold CAR, so this risk does not exist. The 86Box model works, but it does not prove the KEN# behaviour of the real UM8881 for 0xFFFxxxxx.
- **No overflow detection.** A stack overflow below 0x1A0000, or any access outside CAR, silently reads or writes nothing.
- **No cache flush instructions in CAR stages.** `wbinvd`/`invd` must not be used in bootblock or romstage C code, because they would drop the stack.

## 5. SeaBIOS assembly

**`seabios/src/romlayout.S`.** All SeaBIOS assembly. It is assembled as 16-bit code and linked into segment F000, so its symbols are offsets in that segment. It contains:

- **Mode switches.**
  - `transition32` goes from real mode to 32-bit flat. It masks NMI, enables A20 through port 92h, loads the SeaBIOS IDT/GDT (C data in `misc.c`), sets CR0.PE (keeping CR0.CD/NW, which upstream SeaBIOS clears: see §7), then far-jumps.
  - `transition16`/`transition16big` go back. They first load 16-bit descriptors: 64 KiB limits, or 4 GiB limits for "big real mode".
- **External calls.**
  - `__farcall16`: C calls 16-bit code, such as option ROMs and interrupt handlers, with a `struct bregs` register image.
  - `irq_trampoline_0xNN`: `int N; lretw`, used by `call16_int()`.
- **BIOS interfaces.**
  - `entry_pmm`: POST Memory Manager for option ROMs.
  - `entry_pnp_*`: PnP BIOS.
  - `entry_apm16/32`: APM.
  - `entry_pcibios16/32` and `entry_bios32`: PCI BIOS and the BIOS32 directory.
  - `entry_1589`: INT 15h AH=89h.
  - `entry_elf`: the entry from coreboot. It replaces coreboot's GDT/IDT/stack, then jumps to `handle_post()`.
  - `entry_resume`: restart after POST.
- **Interrupt entry.**
  - Each `entry_XX` pushes its C handler address. Hardware IRQs go to `irqentry_extrastack`, which runs on SeaBIOS's own stack in low memory; software interrupts go to `irqentry_arg`, which runs on the caller's stack.
  - The handler receives `struct bregs *`. The struct is the register image plus the interrupt frame, and the handler returns results by changing it.
  - INT 18h/19h (`ENTRY_INTO32`) restart on a fresh stack in 32-bit mode.
- **Fixed addresses.** The IBM PC/AT entry points at F000:E05B (POST), E3FE (INT 13h), E6F2 (INT 19h), E739, E82E, E987, EC59, EF57, EFD2, F045/F065 (INT 10h, only an `iret`: the VGA option ROM provides it), F841, F84D, F859, FE6E, FEA5, FF53, FF54 and FFF0 (reset vector).

**`seabios/src/entryfuncs.S`.** Macros, included by `romlayout.S`:

- `PUSHBREGS`/`POPBREGS` and their variants;
- `ENTRY`, `ENTRY_ARG`, `ENTRY_ARG_ST`, `ENTRY_ARG_ESP`: call a C function with the conditions gcc `-m16` code needs (DS = SS, DF = 0, ESP high half 0, `calll`, first argument in EAX);
- `ENTRY_INTO32`;
- `DECLFUNC`: one section per function.

Not in this ROM, because `layoutrom.py` drops them as unreferenced in this configuration:

- `entry_smi` and `entry_smp` (QEMU only);
- `entry_csm` and `__csm_return` (`CONFIG_CSM` off);
- `entry_sercon` (`CONFIG_SERCON` off);
- `irqentry_arg_extrastack` (`CONFIG_ENTRY_EXTRASTACK` off).

## 6. Other non-C parts

### Linker scripts, and why they cannot be C

A linker script decides where code and data end up. C cannot express that: the reset vector must sit at 0xFFFFFFF0, CAR at a fixed RAM address, and the PC/AT entry points at fixed F000 offsets.

- **`arch/486/memlayout.ld`**
  - The top-level script of every stage. It is run through cpp once per stage.
  - The `ROMSTAGE`/`RAMSTAGE`/`POSTCAR`/`REGION` macros come from `include/memlayout.h`. They include `lib/program.ld` (`.text`, `.data`, `.bss`, `.init` first).
  - `arch/486/include/arch/header.ld` sets `ENTRY(_start)`.
- **`arch/486/bootblock.ld`**
  - Lays out the top of the flash, from the top down:
    - CBFS header pointer at 0xFFFFFFFC;
    - `.reset` at 0xFFFFFFF0;
    - `.id`;
    - `.init` (entry16, GDT, CAR, entry32);
    - bootblock C, starting at 0xFFFFF200 in this build.
  - Also defines `gdtptr_offset` and `nullidt_offset`.
- **`arch/486/car.ld`**
  - The CAR layout of §4, shared by bootblock and romstage.
  - Also an `.illegal_globals` check: initialized data is impossible in stages that run from flash.
- **SeaBIOS `out-rel/romlayout16.lds`, `romlayout32seg.lds`, `romlayout32flat.lds`**
  - Generated by `scripts/layoutrom.py` from the objdump of the three compiled parts.
  - The script keeps only reachable sections and honours the `.fixedaddr.*` sections.
  - It packs everything else into the free space of segment F000.
  - It places the POST-only code so that it can be relocated (`CONFIG_RELOCATE_INIT`).

### Assembly inside C (not commented line by line)

coreboot:

- `include/cpu/486/cr.h` (CR0-CR4) and `cache.h` (`invd`, `wbinvd`, enable/disable cache);
- port I/O helpers (`inb`/`outb`);
- `exception.c` (`lidt`), `cpu.c` (`mov %gs` for `cpu_info`), `breakpoint.c` (debug registers).

SeaBIOS:

- `src/x86.h`: port I/O, CR0, `cpuid`, `wbinvd`, `lgdt`/`sgdt`, `cli`/`sti`.
- `src/farptr.h`: segment-override accessors (`GET_FARVAR`, `SET_SEG`) for 16-bit code, which reaches memory outside DS through ES/FS/GS.
- `src/stacks.c`: `call32()`, `call16()`, stack switching to and from the ExtraStack, thread stack switches. These enter `transition32`/`transition16` with hand-written asm because they change CPU mode and stack mid-function.
- `src/resume.c`: returns to the caller via 40h:0067h for shutdown codes 5, 0Ah, 0Bh, 0Ch.

SeaBIOS C is compiled three times, each pass as a whole program:

- 16-bit (`-m16`, real-mode handlers);
- 32-bit segmented (APM/PCI BIOS);
- 32-bit flat (POST, boot).

### Build-time generators

coreboot:

- `util/genbuild_h` writes `build.h` (version string for `id.S`).
- `sconfig` turns `devicetree.cb` into `static.c`.
- `nvramtool` turns the mainboard `cmos.layout` into `cmos_layout.bin` in CBFS. `CONFIG_USE_OPTION_TABLE` is off, so coreboot itself does not read it at run time.
- `rmodtool` makes postcar and ramstage relocatable.
- `cbfstool` assembles the ROM:
  - bootblock at the top;
  - stages and payload as CBFS files;
  - the default FMAP, because `CONFIG_FMDFILE` is empty.

SeaBIOS:

- `scripts/gen-offsets.sh` writes `asm-offsets.h`: the `BREGS_*` offsets used by the assembly, taken from the compiled `asm-offsets.c`.
- `scripts/buildversion.py` writes the version string.
- `scripts/checkrom.py` checks the size and sets the BIOS checksum byte.
- `scripts/ldnoexec.py` re-marks the linked 16-bit and 32-bit segmented parts as relocatable objects, so that they can be linked again into the final image.

### Configuration and scripts (not firmware code)

- Kconfig files (`src/**/Kconfig`, SeaBIOS `src/Kconfig`) and `Makefile.inc` lists.
- `configs/mb8433_author.config` and `newBIOS/seabios.config`, combined by `newBIOS/build.sh`. Note that `build.sh` re-exports `patches/*.patch` from the work trees after each build, so the comments in these files become part of the patches.
- `newBIOS/cmos.h` (the CMOS map shared by SeaBIOS and coreboot) is C.

## 7. Oddities found while commenting

- **The L1 cache option has no effect after boot.** SeaBIOS `transition32` (`romlayout.S`) clears CR0.CD/NW on every switch to 32-bit mode. The setup option "CPU Internal Cache: Disabled" sets CD in `setup_finish()` (`setup.c`), but `startBoot()` then raises INT 19h, and `entry_19` goes through `transition32`. As a result, L1 is enabled again before the OS starts. **Fixed:** `transition32` keeps CD/NW, and `test/chipset.sh` checks CR0.CD in DOS.
- **Wrong register in the postcar read loop.** `arch/486/exit_car.S` sets EDI = 0x200000, then reads 2 MiB with `rep lodsl`, which reads from ESI. ESI is not initialized, so the 2 MiB read start at whatever ESI held on entry. The intended range (0x200000-0x3FFFFF) is not what is read, and an unlucky ESI near the top of the 4 GiB range would fault. With no IDT at that point, a fault means a triple fault. **Fixed:** ESI = 0x200000.
- **Always-built debug output on COM1 in the release ROM.**
  - `entry16.S` sends the raw CS selector bytes.
  - `arch/486/exit_car.S` sends 44 raw bytes: `{`/CR pairs, EIP, ESP, the return address, `_cbmem_top_ptr`, CR0 twice, `34567`, `X` 0C 0B 0A.
  - The `post_code` characters of §3 come on top, because `CONFIG_POST_IO=y`.
  - All of this goes to COM1 even though the release ROM disables the coreboot serial console.
  - **Fixed:** the POST codes go to port 80h, and the raw dumps are removed; the release ROM sends nothing to COM1, the debug ROM only text logs.
- **Dead code and data in the ROM.**
  - `chipset_teardown_car` (`cpu/486/exit_car.S`) is built but discarded.
  - The unused `__gdtptr` (4 bytes) and `pagetables_name` (11 bytes) are in the bootblock.
  - The `CBFS_FILE_*` defines in `cache_as_ram.S` are unused.
- **Inconsistent upstream GDT entries.** In the `c_start.S` GDT, `arch/ram_segs.h` names 0x38 code and 0x40 data, but the descriptors and their comments say the opposite. Entry 0x38 also has limit[15:0] = 0, so its limit is 0xF0000FFF rather than 4 GiB. These entries are upstream and only used for ACPI S3 resume, which is not built here.
- **CPU-only reset leaves L1 disabled.** On the CPU-only reset path, SeaBIOS resumes the program with the CPU's reset value of CR0 (CD = NW = 1), so L1 stays disabled for it. Shutdown codes 05h/0Ah/0Bh/0Ch return through 40h:0067h without passing `transition32`. This only matters for software that leaves protected mode by resetting the CPU. **Fixed:** `handle_resume()` (`resume.c`) sets CR0.CD/NW from the setup option first.
- **Port assumptions.** `entry16.S` reads port 70h to keep the NMI mask bit, and SeaBIOS uses port 92h for A20 and reset. Neither is checked against the UM8886 documentation here. The 86Box tests pass, but the real board is not tested yet.
