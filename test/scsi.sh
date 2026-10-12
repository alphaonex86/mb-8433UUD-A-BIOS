#!/bin/bash
# ISA option ROM: an Adaptec AHA-154xCF ISA SCSI card (its real BIOS v2.11 from the 86Box ROM set, at
# DC000, with RAM in its window) installs INT 13h and FreeDOS boots from the SCSI disk.
. "$(dirname "$0")/lib.sh"
scsi_dos_disk "$vm_dir/scsi.img"
vm_start scsi "$rom" "[Storage controllers]
scsicard_1 = aha154xcf

[Adaptec AHA-154xCF #1]
bios_addr = DC000

[Hard disks]
hdd_01_parameters = 63, 16, 65, 0, scsi
hdd_01_fn = $vm_dir/scsi.img
hdd_01_scsi_location = 0:00"
vm_wait_text "SCSI-DOS-OK" 90
vm_expect "BIOS Installed Successfully"
vm_expect "Drive C: \(80h\)"
vm_stop
