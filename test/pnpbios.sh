#!/bin/bash
# PnP BIOS from DOS (test/pnpbios/pnp.asm) with an RTL8019AS PnP card: the system device nodes, the ISA
# PnP configuration, the ESCD in the flash (the card's board and the system board); a board written by the
# program is read back, and after a reset the POST has its own ESCD back. Real mode and 16-bit protected
# mode entries (nodes, ESCD read through the ESCD selector).
. "$(dirname "$0")/lib.sh"
mkdir -p "$vm_dir"
nasm -f bin -o "$vm_dir/pnp.com" "$nb/test/pnpbios/pnp.asm"
dos_disk "$vm_dir/hdd.img" "$vm_dir/pnp.com"
vm_start pnpbios "$rom" "$(disk_cfg "$vm_dir/hdd.img")

[Network]
net_01_card = ne2kpnp
net_01_net_type = slirp
net_01_link = 0"
vm_end_of_post 60
vm_wait_text "PNP-TEST-DONE" 90
vm_expect "PNP-NODES-OK"
for id in PNP0000 PNP0200 PNP0100 PNP0B00 PNP0303 PNP0C04 PNP0800 PNP0A03 PNP0C02 PNP0700 PNP0600 PNP0501 \
          PNP0400 PNP0C01; do
  vm_expect " $id"
done
vm_expect "PNP ISA 01 020B"                        # one card, read port 20Bh
vm_expect "^ESCD [0-9A-F]{4} 02  01 RTL8019  00 @@@0000"   # the card (slot = CSN 1), the system board
vm_expect "ESCD-WRITE-OK"
vm_expect "PNP-PM-OK"
vm_reset
vm_end_of_post 60
vm_wait_text "PNP-TEST-DONE" 90
vm_expect "^ESCD [0-9A-F]{4} 02  01 RTL8019  00 @@@0000"
vm_expect "ESCD-WRITE-OK"
vm_expect "PNP-PM-OK"
vm_stop
