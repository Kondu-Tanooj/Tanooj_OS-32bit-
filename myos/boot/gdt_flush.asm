; =============================================================================
; gdt_flush.asm
;
; Loads our new GDT into the CPU and reloads all segment registers.
; This MUST be done in assembly — C cannot directly set CS (code segment).
;
; Called from gdt.c as: gdt_flush(uint32_t gdt_ptr_addr)
; The argument arrives on the stack at [esp+4].
; =============================================================================

global gdt_flush

gdt_flush:
    mov eax, [esp+4]    ; Get the address of our gdt_ptr_t struct
    lgdt [eax]          ; Tell the CPU about our new GDT

    ; Reload data segment registers to use our new GDT entry 2 (0x10)
    ; 0x10 = index 2, TI=0 (GDT), RPL=0 (ring 0)
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Reload CS (code segment) using a far jump to GDT entry 1 (0x08)
    ; 0x08 = index 1, TI=0 (GDT), RPL=0 (ring 0)
    ; A far jump is the only way to reload CS in protected mode.
    jmp 0x08:.flush_cs

.flush_cs:
    ret
