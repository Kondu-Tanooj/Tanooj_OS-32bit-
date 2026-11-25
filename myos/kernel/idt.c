/*
 * idt.c — IDT + PIC (clean final version, interrupts confirmed working)
 */

#include "idt.h"
#include "terminal.h"
#include "process.h"
#include <stdint.h>

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" :: "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void io_wait(void) { outb(0x80, 0); }

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1
#define PIC_EOI   0x20

static void pic_remap(void) {
    outb(PIC1_CMD,  0x11); io_wait();
    outb(PIC2_CMD,  0x11); io_wait();
    outb(PIC1_DATA, 0x20); io_wait();
    outb(PIC2_DATA, 0x28); io_wait();
    outb(PIC1_DATA, 0x04); io_wait();
    outb(PIC2_DATA, 0x02); io_wait();
    outb(PIC1_DATA, 0x01); io_wait();
    outb(PIC2_DATA, 0x01); io_wait();
    outb(PIC1_DATA, 0x00); /* unmask all IRQs on PIC1 */
    outb(PIC2_DATA, 0x00); /* unmask all IRQs on PIC2 */
}

static idt_entry_t  idt[256];
static idt_ptr_t    idt_ptr;
static irq_handler_t irq_handlers[16];

extern void isr0(void);  extern void isr1(void);  extern void isr2(void);  extern void isr3(void);
extern void isr4(void);  extern void isr5(void);  extern void isr6(void);  extern void isr7(void);
extern void isr8(void);  extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void); extern void isr15(void);
extern void isr16(void); extern void isr17(void); extern void isr18(void); extern void isr19(void);
extern void isr20(void); extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void); extern void isr27(void);
extern void isr28(void); extern void isr29(void); extern void isr30(void); extern void isr31(void);
extern void irq0(void);  extern void irq1(void);  extern void irq2(void);  extern void irq3(void);
extern void irq4(void);  extern void irq5(void);  extern void irq6(void);  extern void irq7(void);
extern void irq8(void);  extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void); extern void irq15(void);

void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags) {
    idt[num].base_low  = base & 0xFFFF;
    idt[num].base_high = (base >> 16) & 0xFFFF;
    idt[num].selector  = sel;
    idt[num].zero      = 0;
    idt[num].flags     = flags;
}

void irq_install_handler(int irq, irq_handler_t h) {
    if (irq >= 0 && irq < 16) irq_handlers[irq] = h;
}

/* -----------------------------------------------------------------------
   Direct VGA helpers (no terminal dependency inside interrupt context)
   ----------------------------------------------------------------------- */
static volatile uint16_t * const VGA = (volatile uint16_t *)0xB8000;

static void vga_str(int row, int col, const char *s, uint8_t color) {
    for (int i = 0; s[i]; i++)
        VGA[row * 80 + col + i] = (uint16_t)(unsigned char)s[i] | ((uint16_t)color << 8);
}


/* -----------------------------------------------------------------------
   Exception handler
   ----------------------------------------------------------------------- */
static const char *exc_names[] = {
    "Divide by zero","Debug","NMI","Breakpoint","Overflow",
    "Bound range","Invalid opcode","No FPU","Double fault",
    "FPU overrun","Bad TSS","Segment not present","Stack fault",
    "General protection","Page fault","Reserved","x87 FP",
    "Alignment check","Machine check","SIMD FP"
};

void isr_handler(registers_t *regs) {
    /* White on red banner across row 0 */
    const char *hdr = "  *** TanoojOS KERNEL EXCEPTION ***                                             ";
    for (int i = 0; i < 80 && hdr[i]; i++)
        VGA[i] = (uint16_t)(unsigned char)hdr[i] | (0x4F << 8);

    /* Exception name on row 1 */
    if (regs->int_no < 20)
        vga_str(1, 2, exc_names[regs->int_no], 0x0C);

    /* Page fault extra info */
    if (regs->int_no == 14) {
        /* CR2 holds the faulting virtual address */
        uint32_t fault_addr;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(fault_addr));
        vga_str(2, 2, "Fault address: ", 0x0E);
        /* Print fault_addr as hex directly to VGA */
        static const char h[] = "0123456789ABCDEF";
        char buf[11]; buf[0]='0'; buf[1]='x'; buf[10]=0;
        for (int i=9;i>=2;i--){buf[i]=h[fault_addr&0xF];fault_addr>>=4;}
        for (int i=0;i<10;i++)
            VGA[2*80+17+i] = (uint16_t)(unsigned char)buf[i] | (0x0E<<8);

        /* Error code bits */
        vga_str(3, 2, (regs->err_code & 1) ? "Protection violation" : "Page not present", 0x07);
        vga_str(4, 2, (regs->err_code & 2) ? "Write fault" : "Read fault", 0x07);
        vga_str(5, 2, (regs->err_code & 4) ? "User mode" : "Kernel mode", 0x07);
    }

    vga_str(7, 2, "System halted. Restart with 'reboot' or Ctrl+Alt+Del.", 0x08);
    for (;;) __asm__ volatile ("cli; hlt");
}

/* -----------------------------------------------------------------------
   IRQ handler — clean, no debug output
   ----------------------------------------------------------------------- */
volatile uint32_t timer_ticks = 0;  /* non-static: shell reads this via extern */

void irq_handler(registers_t *regs) {
    /* EOI first — always */
    if (regs->int_no >= 40) outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);

    /* IRQ0: timer */
    if (regs->int_no == 32) {
        timer_ticks++;
        /* Draw status bar once */
        if (timer_ticks == 1) {
            for (int i = 0; i < 80; i++)
                VGA[24 * 80 + i] = ' ' | (0x70 << 8);
            vga_str(24,  1, "TanoojOS v0.1", 0x70);
            vga_str(24, 60, "PgUp/Dn = scroll", 0x70);
        }
        /* Run scheduler */
        schedule();
    }

    /* Dispatch to registered handler (keyboard etc.) */
    int irq = (int)regs->int_no - 32;
    if (irq >= 0 && irq < 16 && irq_handlers[irq])
        irq_handlers[irq](regs);
}

/* -----------------------------------------------------------------------
   idt_init
   ----------------------------------------------------------------------- */
void idt_init(void) {
    for (int i = 0; i < 256; i++) {
        idt[i].base_low=0; idt[i].selector=0;
        idt[i].zero=0; idt[i].flags=0; idt[i].base_high=0;
    }
    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base  = (uint32_t)&idt;

    pic_remap();

    idt_set_gate(0,  (uint32_t)isr0,  0x08, 0x8E);
    idt_set_gate(1,  (uint32_t)isr1,  0x08, 0x8E);
    idt_set_gate(2,  (uint32_t)isr2,  0x08, 0x8E);
    idt_set_gate(3,  (uint32_t)isr3,  0x08, 0x8E);
    idt_set_gate(4,  (uint32_t)isr4,  0x08, 0x8E);
    idt_set_gate(5,  (uint32_t)isr5,  0x08, 0x8E);
    idt_set_gate(6,  (uint32_t)isr6,  0x08, 0x8E);
    idt_set_gate(7,  (uint32_t)isr7,  0x08, 0x8E);
    idt_set_gate(8,  (uint32_t)isr8,  0x08, 0x8E);
    idt_set_gate(9,  (uint32_t)isr9,  0x08, 0x8E);
    idt_set_gate(10, (uint32_t)isr10, 0x08, 0x8E);
    idt_set_gate(11, (uint32_t)isr11, 0x08, 0x8E);
    idt_set_gate(12, (uint32_t)isr12, 0x08, 0x8E);
    idt_set_gate(13, (uint32_t)isr13, 0x08, 0x8E);
    idt_set_gate(14, (uint32_t)isr14, 0x08, 0x8E);
    idt_set_gate(15, (uint32_t)isr15, 0x08, 0x8E);
    idt_set_gate(16, (uint32_t)isr16, 0x08, 0x8E);
    idt_set_gate(17, (uint32_t)isr17, 0x08, 0x8E);
    idt_set_gate(18, (uint32_t)isr18, 0x08, 0x8E);
    idt_set_gate(19, (uint32_t)isr19, 0x08, 0x8E);
    idt_set_gate(20, (uint32_t)isr20, 0x08, 0x8E);
    idt_set_gate(21, (uint32_t)isr21, 0x08, 0x8E);
    idt_set_gate(22, (uint32_t)isr22, 0x08, 0x8E);
    idt_set_gate(23, (uint32_t)isr23, 0x08, 0x8E);
    idt_set_gate(24, (uint32_t)isr24, 0x08, 0x8E);
    idt_set_gate(25, (uint32_t)isr25, 0x08, 0x8E);
    idt_set_gate(26, (uint32_t)isr26, 0x08, 0x8E);
    idt_set_gate(27, (uint32_t)isr27, 0x08, 0x8E);
    idt_set_gate(28, (uint32_t)isr28, 0x08, 0x8E);
    idt_set_gate(29, (uint32_t)isr29, 0x08, 0x8E);
    idt_set_gate(30, (uint32_t)isr30, 0x08, 0x8E);
    idt_set_gate(31, (uint32_t)isr31, 0x08, 0x8E);
    idt_set_gate(32, (uint32_t)irq0,  0x08, 0x8E);
    idt_set_gate(33, (uint32_t)irq1,  0x08, 0x8E);
    idt_set_gate(34, (uint32_t)irq2,  0x08, 0x8E);
    idt_set_gate(35, (uint32_t)irq3,  0x08, 0x8E);
    idt_set_gate(36, (uint32_t)irq4,  0x08, 0x8E);
    idt_set_gate(37, (uint32_t)irq5,  0x08, 0x8E);
    idt_set_gate(38, (uint32_t)irq6,  0x08, 0x8E);
    idt_set_gate(39, (uint32_t)irq7,  0x08, 0x8E);
    idt_set_gate(40, (uint32_t)irq8,  0x08, 0x8E);
    idt_set_gate(41, (uint32_t)irq9,  0x08, 0x8E);
    idt_set_gate(42, (uint32_t)irq10, 0x08, 0x8E);
    idt_set_gate(43, (uint32_t)irq11, 0x08, 0x8E);
    idt_set_gate(44, (uint32_t)irq12, 0x08, 0x8E);
    idt_set_gate(45, (uint32_t)irq13, 0x08, 0x8E);
    idt_set_gate(46, (uint32_t)irq14, 0x08, 0x8E);
    idt_set_gate(47, (uint32_t)irq15, 0x08, 0x8E);

    __asm__ volatile ("lidt (%0)" :: "r"(&idt_ptr));
    __asm__ volatile ("sti");

}
