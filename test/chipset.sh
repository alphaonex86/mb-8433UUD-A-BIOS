#!/bin/bash
# Chipset, PnP/PCI and integrated peripherals: setup values program the same register fields as the
# original BIOS does for them (test/oracle/award_map.txt); PIO mode per drive; unfinished POSTs make
# coreboot fall back to the default chipset values; the internal cache stays disabled up to DOS.
. "$(dirname "$0")/lib.sh"
dos_disk "$vm_dir/hdd.img"
vm_start chipset "$rom" "$(disk_cfg "$vm_dir/hdd.img")"
vm_end_of_post 60
vm_setup

vm_menu 2                                         # CHIPSET FEATURES SETUP
vm_wait_text "Auto Configuration" 5
vm_key $K_PGDN                                    # auto configuration off
vm_key $K_DOWN $K_PGDN                            # DRAM read 1 WS -> 0 WS
vm_key $K_DOWN $K_PGUP                            # DRAM write 1 WS -> 2 WS
vm_key $K_DOWN $K_PGDN $K_PGDN                    # L2 3-1-1-1 -> 2-1-1-1
vm_key $K_DOWN $K_PGDN                            # KBC clock -> PCICLKI/4
for _ in $(seq 9); do vm_key $K_DOWN; done        # slow refresh
vm_key $K_PGDN                                    # -> disabled
vm_expect "DRAM Read Wait States +: 0 WS"
vm_expect "DRAM Write Wait States +: 2 WS"
vm_expect "L2 Cache Wait States +: 2-1-1-1"
vm_expect "Slow Refresh \(1/4 Freq\) +: Disabled"
vm_key $K_ESC
vm_menu 1                                         # BIOS FEATURES: internal and external cache off
vm_key $K_PGDN $K_DOWN $K_PGDN
vm_expect "CPU Internal Cache +: Disabled"
vm_expect "External Cache +: Disabled"
vm_key $K_ESC
vm_menu 4                                         # PNP/PCI
for _ in $(seq 7); do vm_key $K_PGDN; done        # INTA# Auto -> 11
for _ in $(seq 4); do vm_key $K_DOWN; done        # primary master PIO
vm_key $K_PGDN $K_PGDN $K_PGDN                    # Auto -> Mode 2
vm_expect "PCI INTA# IRQ +: 11"
vm_expect "IDE Primary Master PIO +: Mode 2"
vm_key $K_ESC
vm_menu 5                                         # INTEGRATED PERIPHERALS
for _ in $(seq 4); do vm_key $K_DOWN; done        # serial port 2
vm_key $K_PGUP $K_PGUP                            # COM2 COM4/2E8 -> disabled
vm_key $K_DOWN $K_PGDN                            # parallel 378 -> 278
vm_key $K_DOWN $K_PGDN $K_PGDN                    # SPP -> ECP
vm_expect "Onboard Serial Port 2 +: Disabled"
vm_expect "Onboard Parallel Port +: 278H"
vm_expect "Onboard Parallel Mode +: ECP"
vm_save

check_chipset() {  # manual values applied (1) or default ones (0)
  local hb50 hb51 hb5d ibc56
  hb51=$(vm_pci_byte 00:10.0 51) hb50=$(vm_pci_byte 00:10.0 50)
  hb5d=$(vm_pci_byte 00:10.0 5d) ibc56=$(vm_pci_byte 00:12.0 56)
  if [ "$1" = 1 ]; then
    expect_bits "$hb51" 0xf0 0xd0 "DRAM read 0 WS, write 2 WS (HB51)"
    expect_bits "$hb50" 0x30 0x30 "L2 2-1-1-1 (HB50)"
    expect_bits "$hb5d" 0x10 0x00 "slow refresh off (HB5D)"
    expect_bits "$ibc56" 0x0c 0x04 "KBC clock PCICLKI/4 (IBC56)"
  else
    expect_bits "$hb51" 0xf0 0xa0 "Auto DRAM timing 1 WS / 1 WS (HB51)"
  fi
  expect_bits "$hb50" 0x80 0x00 "external cache disabled (HB50)"
  expect_bits "$(vm_pci_byte 00:10.0 55)" 0x21 0x21 "System and Video BIOS cacheable (HB55, as the original)"
  expect_bits "$(vm_pci_byte 00:10.0 58)" 0xff 0x0f "HB58 with System BIOS cacheable (as the original)"
  expect_bits "$(vm_pci_byte 00:10.0 59)" 0xff 0x00 "HB59 with System BIOS cacheable (as the original)"
}

vm_end_of_post 60
vm_expect "Serial Port\(s\) +: 3F8 +$"
vm_expect "Parallel Port\(s\) : 278"
vm_expect "Pri. Master Disk : 31MB, PIO 2"
check_chipset 1
expect_bits "$(vm_pci_byte 00:12.0 43)" 0xf0 0xb0 "PIRQ A -> IRQ 11 (IBC43 bits 7-4, as the original BIOS)"
ide43=$(vm_pci_byte 00:12.1 43) ide44=$(vm_pci_byte 00:12.1 44) ide46=$(vm_pci_byte 00:12.1 46)
expect_bits "$ide43" 0xc0 0x40 "PIO 2 timing A (IDE43)"
expect_bits "$ide44" 0xf0 0x40 "PIO 2 timing B (IDE44)"
expect_bits "$ide46" 0xf0 0x50 "PIO 2 timing C (IDE46)"
c0=$(vm_sio c0) c1=$(vm_sio c1)
expect_bits "$c0" 0x0f 0x0b "COM2 off (SIO C0)"
expect_bits "$c1" 0xca 0xc2 "LPT 278 ECP, COM1 3F8 (SIO C1)"

# Two unfinished POSTs: coreboot ignores the manual chipset values once, POST completion re-arms them.
vm_cmd out 70 7e
vm_cmd out 71 02
vm_cmd reset
vm_end_of_post 60
check_chipset 0
vm_expect "Chipset setup values not used after 2 unfinished POSTs"
vm_wait_text "DOS-TEST-OK" 60                     # POST completed
expect_bits "$(vm_cr0)" 0x40000000 0x40000000 "L1 cache still disabled in DOS (CR0.CD)"
vm_cmd reset
vm_end_of_post 60
check_chipset 1
vm_stop
