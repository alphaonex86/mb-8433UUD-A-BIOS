; Sector at LBA 30000000h of the large disk test: prints LBA48-OK.
bits 16
org 7e00h
        mov si, msg
print:  lodsb
        or al, al
        jz $
        mov ah, 0eh
        mov bx, 7
        int 10h
        jmp print
msg:    db "LBA48-OK", 0
        times 512 - ($ - $$) db 0
