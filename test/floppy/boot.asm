; Boot sector of the floppy boot test: prints FLOPPY-BOOT, reads sector 2 (CHS 0/0/2) of drive A with
; INT 13h and prints the text it holds; FLOPPY-READ-ERR if the read fails.
bits 16
org 0x7c00
	xor ax, ax
	mov ds, ax
	mov es, ax
	mov ss, ax
	mov sp, 0x7c00
	mov si, banner
	call puts
	mov ax, 0x0201			; read 1 sector
	mov cx, 0x0002			; cylinder 0, sector 2
	xor dx, dx			; head 0, drive A
	mov bx, 0x0600
	int 0x13
	mov si, readerr
	jc .print
	mov si, 0x0600
.print:	call puts
.halt:	hlt
	jmp .halt
puts:	lodsb
	or al, al
	jz .done
	mov ah, 0x0e
	xor bx, bx
	int 0x10
	jmp puts
.done:	ret
banner	db 'FLOPPY-BOOT ', 0
readerr	db 'FLOPPY-READ-ERR', 0
	times 510 - ($ - $$) db 0
	dw 0xaa55
	db 'FLOPPY-READ-OK', 0
	times 1474560 - ($ - $$) db 0
