#!/bin/bash
# Ctrl+Alt+Del (warm boot: port 92h resets the CPU, not the chipset) on the operator's setup: 2 Mbit flash (A17 high),
# plain RTL8139C with Etherboot in its socket ROM, DOS disk, the Am5x86 at 3 x 40 MHz. DOS boots, Ctrl+Alt+Del at its
# prompt, the POST must reach DOS again; twice, then once from the setup utility. Then with the card left receiving by
# a boot program (Etherboot loads iPXE, whose script opens net0 and stays at its shell) and LAN broadcasts arriving
# every ms (86Box BOX86_NET_NOISE_MS): the card keeps writing them into RAM by DMA, the POST must still finish.
. "$(dirname "$0")/lib.sh"
dos_disk "$vm_dir/hdd.img"
net_boot_dir $'#!ipxe\nifopen net0\necho NIC-OPEN\nshell\n'
export BOX86_FLASH=w49f002a BOX86_NET_NOISE_MS=1
vm_cpu_family=am5x86 vm_cpu_speed=120000000 vm_cpu_multi=3
vm_start warmboot "${rom%.rom}-2mbit.rom" "$(disk_cfg "$vm_dir/hdd.img")
$(nic_cfg "$etherboot")"
vm_end_of_post
vm_wait_text "DOS-TEST-OK" 90
for n in 1 2; do
  vm_cad
  vm_wait_text "DOS-TEST-OK" 90
done
vm_cad
vm_setup
vm_cad                                              # in the setup utility
vm_setup
vm_menu 1                                           # BIOS FEATURES SETUP
vm_wait_text "1st Boot Device" 5
vm_net_first
vm_save
for n in 1 2 3; do
  vm_wait_text "NIC-OPEN" 90 || fail "network boot $n: iPXE did not open the card"
  sleep 2                                           # frames keep arriving
  vm_cad
done
vm_wait_text "NIC-OPEN" 90
vm_stop
