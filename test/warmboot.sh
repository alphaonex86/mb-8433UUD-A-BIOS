#!/bin/bash
# Ctrl+Alt+Del (warm boot: port 92h resets the CPU, not the chipset) on the operator's setup: 2 Mbit flash with
# iPXE, RTL8139C 10ec:8039, DOS disk, the Am5x86 at 3 x 40 MHz. DOS boots, Ctrl+Alt+Del at its prompt, the
# POST must reach DOS again; twice, then once from the setup utility. Then with the card left receiving by a boot
# ROM (iPXE booted from the network: a script opens net0 and stays at its shell) and LAN broadcasts arriving every
# ms (86Box BOX86_NET_NOISE_MS): the card keeps writing them into RAM by DMA, the POST must still finish.
. "$(dirname "$0")/lib.sh"
dos_disk "$vm_dir/hdd.img"
mkdir -p "$vm_dir/tftp"
printf '#!ipxe\nifopen net0\necho NIC-OPEN\nshell\n' > "$vm_dir/tftp/boot.ipxe"
export BOX86_FLASH=w49f002a BOX86_NET_NOISE_MS=1 BOX86_SLIRP_TFTP=$vm_dir/tftp BOX86_SLIRP_BOOTFILE=boot.ipxe
vm_cpu_family=am5x86 vm_cpu_speed=120000000 vm_cpu_multi=3
vm_start warmboot "${rom%.rom}-2mbit.rom" "$(disk_cfg "$vm_dir/hdd.img")
$(printf '[Network]\nnet_01_card = rtl8139c+\nnet_01_net_type = slirp\nnet_01_link = 0\n
[Realtek RTL8139C+ #1]\nbios = 0\ndevice_id = 8039\ncplus = 0\n')"
vm_end_of_post
vm_wait_text "DOS-TEST-OK" 90
for n in 1 2; do
  vm_cmd combo 1d 38 53                             # Ctrl+Alt+Del
  vm_wait_text "Press DEL to enter SETUP" 60 || fail "warm boot $n: no POST prompt"
  vm_wait_text "DOS-TEST-OK" 90
done
vm_cmd combo 1d 38 53
vm_wait_text "Press DEL to enter SETUP" 60
vm_setup
vm_cmd combo 1d 38 53                               # in the setup utility
vm_wait_text "Press DEL to enter SETUP" 60
vm_setup
vm_menu 1                                           # BIOS FEATURES SETUP
vm_wait_text "1st Boot Device" 5
vm_key $K_DOWN $K_DOWN $K_PGDN $K_PGDN $K_PGDN      # 1st boot device: Floppy -> Network
vm_expect "1st Boot Device +: Network"
vm_save
for n in 1 2 3; do
  vm_wait_text "NIC-OPEN" 90 || fail "network boot $n: iPXE did not open the card"
  sleep 2                                           # frames keep arriving
  vm_cmd combo 1d 38 53
  vm_wait_text "Press DEL to enter SETUP" 60 || fail "warm boot $n with the card receiving: no POST prompt"
done
vm_wait_text "NIC-OPEN" 90
vm_stop
