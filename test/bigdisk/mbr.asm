; Boot sector for the large disk test: INT 13h extended read of LBA 30000000h (beyond LBA28, needs
; LBA48) to 0:7E00, then run it; prints LBA48-FAIL if the read fails.
bits 16
org 7c00h
        xor ax, ax
        mov ds, ax
        mov es, ax
        mov ss, ax
        mov sp, 7c00h
        mov si, dap
        mov ah, 42h
        int 13h
        jc fail
        jmp 0:7e00h
fail:   mov si, msg
print:  lodsb
        or al, al
        jz $
        mov ah, 0eh
        mov bx, 7
        int 10h
        jmp print
dap:    db 16, 0
        dw 1, 7e00h, 0
        dq 30000000h
msg:    db "LBA48-FAIL", 0
        times 510 - ($ - $$) db 0
        dw 0aa55h
