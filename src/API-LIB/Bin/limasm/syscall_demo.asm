; syscall_demo.asm - int 0x80 syscall output test
; build: limasm /user/home/syscall.asm /user/home/syscall.exc
; run:   run /user/home/syscall.exc
[bits 32]
global _start
_start:
    mov eax, 3              ; SYS_IOPRINTLN
    mov ebx, msg1
    int 0x80

    mov eax, 2              ; SYS_IOPRINT
    mov ebx, msg2
    int 0x80

    mov esi, 48             ; '0'
.loop:
    mov eax, 1              ; SYS_PUTC
    mov ebx, esi
    int 0x80
    inc esi
    cmp esi, 58             ; '9' + 1
    jb .loop

    mov eax, 1              ; newline
    mov ebx, 10
    int 0x80

    mov eax, 3              ; SYS_IOPRINTLN
    mov ebx, msg3
    int 0x80

    mov eax, 17             ; SYS_EXIT
    mov ebx, 0
    int 0x80

msg1: db "=== int 0x80 syscall demo ===", 0
msg2: db "putc 0-9: ", 0
msg3: db "ioprintln / ioprint / putc / exit: OK", 0
