; =============================================================================
; boot.asm — Multiboot entry point
; This is the FIRST code that runs. GRUB loads us, checks the magic header,
; then jumps here. Our only job: set up a stack, then call kernel_main().
; =============================================================================

; --- Multiboot 1 header constants ---
; GRUB looks for this magic signature in the first 8 KiB of the kernel binary.
MBALIGN   equ 1 << 0           ; Align loaded modules on page boundaries
MEMINFO   equ 1 << 1           ; Provide memory map to kernel
FLAGS     equ MBALIGN | MEMINFO
MAGIC     equ 0x1BADB002       ; Magic number GRUB searches for
CHECKSUM  equ -(MAGIC + FLAGS) ; Must sum to zero with MAGIC+FLAGS

; The .multiboot section must appear very early in the binary.
section .multiboot
align 4
    dd MAGIC
    dd FLAGS
    dd CHECKSUM

; --- Kernel stack ---
; C needs a stack before it can run. We reserve 16 KiB in BSS (uninitialised
; data) — the CPU stack grows downward, so we point ESP at the top.
section .bss
align 16
stack_bottom:
    resb 16384      ; 16 KiB
stack_top:

; --- Entry point ---
section .text
global _start

_start:
    ; Set up the stack pointer
    mov esp, stack_top

    ; GRUB puts useful info in eax (magic) and ebx (multiboot info pointer).
    ; Pass them to kernel_main so the kernel can read the memory map etc.
    push ebx        ; multiboot info pointer (arg 2)
    push eax        ; multiboot magic      (arg 1)

    ; Call our C kernel — this is where your OS lives from here on
    extern kernel_main
    call kernel_main

    ; If kernel_main ever returns (it shouldn't), halt the CPU.
    cli             ; Disable interrupts
.hang:
    hlt             ; Halt until next interrupt (which won't come)
    jmp .hang       ; Loop just in case
