#ifndef __ASM_ARM_IRQ_H
#define __ASM_ARM_IRQ_H

#include <asm/arch/irqs.h>

#ifndef irq_cannonicalize
#define irq_cannonicalize(i)	(i)
#endif

#ifndef NR_IRQS
#define NR_IRQS	128
#endif

/*
 * Use this value to indicate lack of interrupt
 * capability
 */
#ifndef NO_IRQ
#define NO_IRQ	((unsigned int)(-1))
#endif

struct irqaction;

#define disable_irq_nosync(i) disable_irq(i)

extern void disable_irq(unsigned int);
extern void enable_irq(unsigned int);

#define __IRQT_FALEDGE	(1 << 0)
#define __IRQT_RISEDGE	(1 << 1)
#define __IRQT_LOWLVL	(1 << 2)
#define __IRQT_HIGHLVL	(1 << 3)

#define IRQT_NOEDGE	(0)
#define IRQT_RISING	(__IRQT_RISEDGE)
#define IRQT_FALLING	(__IRQT_FALEDGE)
#define IRQT_BOTHEDGE	(__IRQT_RISEDGE|__IRQT_FALEDGE)
#define IRQT_LOW	(__IRQT_LOWLVL)
#define IRQT_HIGH	(__IRQT_HIGHLVL)

int set_irq_type(unsigned int irq, unsigned int type);

int setup_irq(unsigned int, struct irqaction *);

#endif

