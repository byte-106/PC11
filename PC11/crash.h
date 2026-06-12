/* crash.h - minimal CPU exception handling for PC11.
 *
 * Installs a tiny IDT so CPU faults (page fault, GP, divide-by-zero, etc.)
 * are caught instead of triple-faulting/rebooting. On a fault we paint a
 * crash screen with the exception number + error code, wait ~5s, then reboot.
 *
 * The caller must provide pc11_panic(int vec, unsigned long err) which draws
 * the screen and reboots.
 */
#ifndef PC11_CRASH_H
#define PC11_CRASH_H

/* provided by wm.c */
void pc11_panic(int vec, unsigned long long err);

/* 64-bit IDT gate descriptor */
typedef struct __attribute__((packed)) {
    UINT16 off_lo;
    UINT16 sel;
    UINT8  ist;
    UINT8  type_attr;
    UINT16 off_mid;
    UINT32 off_hi;
    UINT32 zero;
} IdtGate;

typedef struct __attribute__((packed)) {
    UINT16 limit;
    UINT64 base;
} IdtPtr;

/* We need enough entries to cover the firmware's hardware-interrupt vectors
 * too (timer is usually 0x20+). We copy the firmware's whole IDT, then
 * overlay ONLY the CPU-exception vectors (0..19) with our handlers, so
 * normal interrupts keep working while faults reach our crash screen. */
#define IDT_ENTRIES 256
static IdtGate g_idt[IDT_ENTRIES];

/* read current code segment selector */
static inline UINT16 read_cs(void){
    UINT16 cs; __asm__ __volatile__("mov %%cs,%0":"=r"(cs)); return cs;
}

/* Common assembly entry: each vector pushes its number (and a dummy error
 * code for vectors that don't supply one), then jumps here. The C handler
 * receives (vec, err) and never returns. */
extern void isr_common(void);

/* Macro to make an ISR stub. Vectors 8,10-14,17 push an error code; the
 * rest don't, so we push a 0 to keep the stack layout uniform. */
#define ISR_NOERR(n) \
    __attribute__((naked)) static void isr##n(void){ \
        __asm__ __volatile__("pushq $0\n\tpushq $" #n "\n\tjmp isr_common"); }
#define ISR_ERR(n) \
    __attribute__((naked)) static void isr##n(void){ \
        __asm__ __volatile__("pushq $" #n "\n\tjmp isr_common"); }

ISR_NOERR(0)  ISR_NOERR(1)  ISR_NOERR(2)  ISR_NOERR(3)
ISR_NOERR(4)  ISR_NOERR(5)  ISR_NOERR(6)  ISR_NOERR(7)
ISR_ERR(8)    ISR_NOERR(9)  ISR_ERR(10)   ISR_ERR(11)
ISR_ERR(12)   ISR_ERR(13)   ISR_ERR(14)   ISR_NOERR(15)
ISR_NOERR(16) ISR_ERR(17)   ISR_NOERR(18) ISR_NOERR(19)

/* isr_common: stack on entry (top first): vec, err, [CPU frame...].
 * We pull vec & err from the stack and call pc11_panic. Naked + manual. */
__attribute__((naked,used)) static void isr_common_impl(void){
    __asm__ __volatile__(
        "popq %rsi\n\t"        /* rsi = vec  */
        "popq %rdx\n\t"        /* rdx = err  */
        "movq %rsi,%rcx\n\t"   /* ms_abi: 1st arg in rcx */
        "movq %rdx,%r8\n\t"    /* but pc11_panic is plain; use System V? */
        "movl %esi,%edi\n\t"   /* SysV: 1st arg edi = vec */
        "movq %rdx,%rsi\n\t"   /* SysV: 2nd arg rsi = err */
        "andq $-16,%rsp\n\t"   /* align stack */
        "call pc11_panic\n\t"
        "hlt\n\t");
}
/* alias label isr_common -> isr_common_impl */
__asm__(".set isr_common, isr_common_impl");

static void idt_set(int n, void (*handler)(void)){
    UINT64 a=(UINT64)handler;
    g_idt[n].off_lo  = a & 0xFFFF;
    g_idt[n].sel     = read_cs();
    g_idt[n].ist     = 0;
    g_idt[n].type_attr = 0x8E;          /* present, ring0, interrupt gate */
    g_idt[n].off_mid = (a>>16)&0xFFFF;
    g_idt[n].off_hi  = (a>>32)&0xFFFFFFFF;
    g_idt[n].zero    = 0;
}

/* simple memcpy */
static void crash_memcpy(void *d, const void *s, unsigned long n){
    unsigned char *dd=d; const unsigned char *ss=s;
    for(unsigned long i=0;i<n;i++) dd[i]=ss[i];
}

static void crash_init(void){
    /* 1) read the firmware's current IDT */
    IdtPtr cur;
    __asm__ __volatile__("sidt %0":"=m"(cur));

    /* 2) copy the firmware's IDT into ours so hardware interrupts (timer,
     *    etc.) keep working exactly as before. */
    unsigned long entries = (cur.limit + 1) / sizeof(IdtGate);
    if(entries > IDT_ENTRIES) entries = IDT_ENTRIES;
    if(cur.base) crash_memcpy(g_idt, (void*)cur.base, entries*sizeof(IdtGate));

    /* 3) overlay ONLY the CPU-exception vectors 0..19 with our handlers. */
    void (*stubs[20])(void) = {
        isr0,isr1,isr2,isr3,isr4,isr5,isr6,isr7,isr8,isr9,
        isr10,isr11,isr12,isr13,isr14,isr15,isr16,isr17,isr18,isr19
    };
    for(int i=0;i<20;i++) idt_set(i, stubs[i]);

    /* 4) load our IDT (same size as the firmware's so its IRQ gates remain) */
    IdtPtr p; p.limit = cur.limit; p.base=(UINT64)g_idt;
    if(p.limit < (UINT16)(20*sizeof(IdtGate)-1)) p.limit = sizeof(g_idt)-1;
    __asm__ __volatile__("lidt %0"::"m"(p));
}

#endif
