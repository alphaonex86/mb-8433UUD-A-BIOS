/* Forced into every SeaBIOS file compiled on the host: the privileged x86 helpers become calls into the
   device models (seams.c), and real-mode pointers below 64 KiB (IVT, BDA) map to a host buffer. */
#ifndef DEVFUZZ_SEAM_H
#define DEVFUZZ_SEAM_H

/* x86.h is read with its inline asm and MMIO helpers renamed out of the way, then they are declared as
   functions. */
#define inb x86_asm_inb
#define inw x86_asm_inw
#define inl x86_asm_inl
#define outb x86_asm_outb
#define outw x86_asm_outw
#define outl x86_asm_outl
#define insb x86_asm_insb
#define insw x86_asm_insw
#define insl x86_asm_insl
#define outsb x86_asm_outsb
#define outsw x86_asm_outsw
#define outsl x86_asm_outsl
#define irq_disable x86_asm_irq_disable
#define irq_enable x86_asm_irq_enable
#define save_flags x86_asm_save_flags
#define restore_flags x86_asm_restore_flags
#define cpu_relax x86_asm_cpu_relax
#define hlt x86_asm_hlt
#define wbinvd x86_asm_wbinvd
#define __cpuid x86_asm___cpuid
#define cr0_read x86_asm_cr0_read
#define cr0_write x86_asm_cr0_write
#define cr0_mask x86_asm_cr0_mask
#define cr0_vm86_read x86_asm_cr0_vm86_read
#define rdmsr x86_asm_rdmsr
#define wrmsr x86_asm_wrmsr
#define rdtscll x86_asm_rdtscll
#define getesp x86_asm_getesp
#define sgdt x86_asm_sgdt
#define lgdt x86_asm_lgdt
#define get_a20 x86_asm_get_a20
#define set_a20 x86_asm_set_a20
#define writel x86_asm_writel
#define writew x86_asm_writew
#define writeb x86_asm_writeb
#define readq x86_asm_readq
#define readl x86_asm_readl
#define readw x86_asm_readw
#define readb x86_asm_readb
#include "x86.h"
#include "farptr.h"
#undef inb
#undef inw
#undef inl
#undef outb
#undef outw
#undef outl
#undef insb
#undef insw
#undef insl
#undef outsb
#undef outsw
#undef outsl
#undef irq_disable
#undef irq_enable
#undef save_flags
#undef restore_flags
#undef cpu_relax
#undef hlt
#undef wbinvd
#undef __cpuid
#undef cr0_read
#undef cr0_write
#undef cr0_mask
#undef cr0_vm86_read
#undef rdmsr
#undef wrmsr
#undef rdtscll
#undef getesp
#undef sgdt
#undef lgdt
#undef get_a20
#undef set_a20
#undef writel
#undef writew
#undef writeb
#undef readq
#undef readl
#undef readw
#undef readb

void outb(u8 value, u16 port);
void outw(u16 value, u16 port);
void outl(u32 value, u16 port);
u8 inb(u16 port);
u16 inw(u16 port);
u32 inl(u16 port);
void insb(u16 port, u8 *data, u32 count);
void insw(u16 port, u16 *data, u32 count);
void insl(u16 port, u32 *data, u32 count);
void outsb(u16 port, u8 *data, u32 count);
void outsw(u16 port, u16 *data, u32 count);
void outsl(u16 port, u32 *data, u32 count);
void irq_disable(void);
void irq_enable(void);
u32 save_flags(void);
void restore_flags(u32 flags);
void cpu_relax(void);
void hlt(void);
void wbinvd(void);
void __cpuid(u32 index, u32 *eax, u32 *ebx, u32 *ecx, u32 *edx);
u32 cr0_read(void);
void cr0_write(u32 cr0);
void cr0_mask(u32 off, u32 on);
u16 cr0_vm86_read(void);
u64 rdmsr(u32 index);
void wrmsr(u32 index, u64 val);
u64 rdtscll(void);
u32 getesp(void);
void sgdt(struct descloc_s *desc);
void lgdt(struct descloc_s *desc);
u8 get_a20(void);
u8 set_a20(u8 cond);
void writel(void *addr, u32 val);
void writew(void *addr, u16 val);
void writeb(void *addr, u8 val);
u64 readq(const void *addr);
u32 readl(const void *addr);
u16 readw(const void *addr);
u8 readb(const void *addr);

/* The 16-bit runtime code (INT 13h disk access) runs here in flat mode too. */
#undef ASSERT16
#define ASSERT16() do { } while (0)

void *seam_flatptr(u32 seg, u32 off);
#undef MAKE_FLATPTR
#define MAKE_FLATPTR(seg, off) seam_flatptr((u32)(seg), (u32)(off))
#include "biosvar.h"                            /* its inline helpers see the new MAKE_FLATPTR */

#endif
