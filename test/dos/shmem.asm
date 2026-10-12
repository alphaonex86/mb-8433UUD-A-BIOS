; DOS test: enable the 16 KiB shared RAM of a WD8013 ISA card (I/O 280h) at D000:0, write and read it.
; The BIOS must leave that block on the ISA bus for reads and writes, as the original did.
org 100h
        mov dx, 280h
        mov al, 40h | ((0D0000h >> 13) & 3Fh)   ; MSR: RAM enable + address bits A13-A18
        out dx, al
        mov ax, 0D000h
        mov es, ax
        mov word [es:0], 5AA5h
        mov word [es:3FFEh], 0A55Ah
        mov dx, fail
        cmp word [es:0], 5AA5h
        jne print
        cmp word [es:3FFEh], 0A55Ah
        jne print
        mov dx, ok
print:  mov ah, 9
        int 21h
        ret
ok:     db 'SHMEM-OK', 13, 10, '$'
fail:   db 'SHMEM-FAIL', 13, 10, '$'
