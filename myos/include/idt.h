#ifndef IDT_H
#define IDT_H

#include <stdint.h>

/*
 * IDT — Interrupt Descriptor Table
 *
 * The CPU uses the IDT to find the handler for every interrupt/exception.
 * 256 possible entries:
 *   0–31   : CPU exceptions (divide by zero, page fault, etc.)
 *   32–47  : Hardware IRQs  (remapped from 0–15 to avoid conflicts)
 *   48–255 : Available for software interrupts (syscalls etc.)
 *
 * Each entry is 8 bytes.
 */
typedef struct {
    uint16_t base_low;   /* Handler address bits 0–15   */
    uint16_t selector;   /* Kernel code segment selector */
    uint8_t  zero;       /* Always 0                     */
    uint8_t  flags;      /* Gate type + privilege flags   */
    uint16_t base_high;  /* Handler address bits 16–31   */
} __attribute__((packed)) idt_entry_t;

/* Loaded into CPU via lidt instruction */
typedef struct {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed)) idt_ptr_t;

/*
 * Registers snapshot — must match the EXACT stack layout built by isr.asm:
 *
 *  Stack grows downward. After the stub runs, ESP points to:
 *    [ESP+ 0]  ds        <- push eax (ds) AFTER pusha, so it sits at TOP
 *    [ESP+ 4]  edi       \ 
 *    [ESP+ 8]  esi        |
 *    [ESP+12]  ebp        |  from pusha (pusha pushes eax first,
 *    [ESP+16]  esp_orig   |   edi last, so edi is at lowest addr)
 *    [ESP+20]  ebx        |
 *    [ESP+24]  edx        |
 *    [ESP+28]  ecx        |
 *    [ESP+32]  eax       /
 *    [ESP+36]  int_no    <- pushed by ISR macro
 *    [ESP+40]  err_code  <- pushed by ISR macro (or CPU for some exceptions)
 *    [ESP+44]  eip       \ 
 *    [ESP+48]  cs         | pushed automatically by CPU on interrupt
 *    [ESP+52]  eflags    /
 *
 * The struct MUST start with ds, otherwise int_no reads the wrong field.
 */
typedef struct {
    uint32_t ds;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t int_no;
    uint32_t err_code;
    uint32_t eip, cs, eflags, useresp, ss;
} __attribute__((packed)) registers_t;

void idt_init(void);
void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags);

/* IRQ handler registration */
typedef void (*irq_handler_t)(registers_t *regs);
void irq_install_handler(int irq, irq_handler_t handler);

#endif
