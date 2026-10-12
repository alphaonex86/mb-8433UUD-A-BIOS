#!/bin/bash
# ISA Plug and Play at POST as the original BIOS (PnP BIOS Auto-Config): a Realtek RTL8019AS PnP card is
# isolated, gets free I/O and IRQ and answers there; with the option off it stays inactive.
. "$(dirname "$0")/lib.sh"
mkdir -p "$vm_dir"
vm_start isapnp "$rom" "[Network]
net_01_card = ne2kpnp
net_01_net_type = slirp
net_01_link = 0"

isa_page() {  # setup -> HARDWARE INFORMATION -> ISA page, then back to the main menu
  vm_setup
  vm_menu 6
  vm_wait_text "CPU +:" 5
  vm_key $K_PGDN $K_PGDN
  vm_wait_text "IRQ 15" 5
}
port() {  # I/O port (hex) -> value read (hex)
  rm -f "$vm/in.txt"
  vm_cmd in "$1" "$vm/in.txt"
  for _ in $(seq 50); do [ -s "$vm/in.txt" ] && break; sleep 0.1; done
  awk '{print $2}' "$vm/in.txt"
}

vm_end_of_post 60
isa_page
vm_expect "ISA PnP RTL8019: 220/5 Realtek Plug & Play"
vm_expect "IRQ 5 : ISA PnP card"
[ "$(port 220)" != ff ] || fail "the card does not answer at 220h"
vm_key $K_ESC
vm_menu 4                                         # PNP/PCI CONFIGURATION
vm_wait_text "PnP BIOS Auto-Config +: Enabled" 5
for _ in $(seq 9); do vm_key $K_DOWN; done
vm_key $K_PGDN
vm_expect "PnP BIOS Auto-Config +: Disabled"
vm_save

vm_end_of_post 60
[ "$(port 220)" = ff ] || fail "the card answers at 220h with PnP auto-config off"
isa_page
grep -q "ISA PnP" "$vm/screen.txt" && fail "a PnP card was configured with PnP auto-config off"
vm_stop
