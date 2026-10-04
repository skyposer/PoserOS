[BITS 32]
    mov byte [0xB8000], 'S'
    mov byte [0xB8001], 0x07
    jmp $
