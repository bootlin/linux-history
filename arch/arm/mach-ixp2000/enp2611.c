/*
 * arch/arm/mach-ixp2000/enp2611.c
 *
 * Radisys ENP-2611 support.
 *
 * Created 2004 by Lennert Buytenhek from the ixdp2x01 code.  The
 * original version carries the following notices:
 *
 * Original Author: Andrzej Mialwoski <andrzej.mialwoski@intel.com>
 * Maintainer: Deepak Saxena <dsaxena@plexity.net>
 *
 * Copyright (C) 2002-2003 Intel Corp.
 * Copyright (C) 2003-2004 MontaVista Software, Inc.
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 */

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/bitops.h>
#include <linux/pci.h>
#include <linux/ioport.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/serial.h>
#include <linux/tty.h>
#include <linux/serial_core.h>
#include <linux/device.h>

#include <asm/io.h>
#include <asm/irq.h>
#include <asm/pgtable.h>
#include <asm/page.h>
#include <asm/system.h>
#include <asm/hardware.h>
#include <asm/mach-types.h>

#include <asm/mach/pci.h>
#include <asm/mach/map.h>
#include <asm/mach/irq.h>
#include <asm/mach/time.h>
#include <asm/mach/arch.h>
#include <asm/mach/flash.h>

/*************************************************************************
 * ENP-2611 timer tick configuration
 *************************************************************************/
static void __init enp2611_timer_init(void)
{
	ixp2000_init_time(50 * 1000 * 1000);
}

static struct sys_timer enp2611_timer = {
	.init		= enp2611_timer_init,
	.offset		= ixp2000_gettimeoffset,
};


/*************************************************************************
 * ENP-2611 PCI
 *************************************************************************/
static int enp2611_pci_setup(int nr, struct pci_sys_data *sys)
{
	sys->mem_offset = 0xe0000000;
	ixp2000_pci_setup(nr, sys);
	return 1;
}

static void __init enp2611_pci_preinit(void)
{
	ixp2000_reg_write(IXP2000_PCI_ADDR_EXT, 0x00100000);
	ixp2000_pci_preinit();
}

static inline int enp2611_pci_valid_device(struct pci_bus *bus,
						unsigned int devfn)
{
	/* The 82559 ethernet controller appears at both PCI:1:0:0 and
	 * PCI:1:2:0, so let's pretend the second one isn't there.
	 */
	if (bus->number == 0x01 && devfn == 0x10)
		return 0;

	return 1;
}

static int enp2611_pci_read_config(struct pci_bus *bus, unsigned int devfn,
					int where, int size, u32 *value)
{
	if (enp2611_pci_valid_device(bus, devfn))
		return ixp2000_pci_read_config(bus, devfn, where, size, value);

	return PCIBIOS_DEVICE_NOT_FOUND;
}

static int enp2611_pci_write_config(struct pci_bus *bus, unsigned int devfn,
					int where, int size, u32 value)
{
	if (enp2611_pci_valid_device(bus, devfn))
		return ixp2000_pci_write_config(bus, devfn, where, size, value);

	return PCIBIOS_DEVICE_NOT_FOUND;
}

static struct pci_ops enp2611_pci_ops = {
	.read   = enp2611_pci_read_config,
	.write  = enp2611_pci_write_config
};

static struct pci_bus * __init enp2611_pci_scan_bus(int nr,
						struct pci_sys_data *sys)
{
	return pci_scan_bus(sys->busnr, &enp2611_pci_ops, sys);
}

static int __init enp2611_pci_map_irq(struct pci_dev *dev, u8 slot, u8 pin)
{
	int irq;

	if (dev->bus->number == 0x00 && PCI_SLOT(dev->devfn) == 0x01) {
		/* 21555 non-transparent bridge.  */
		irq = IRQ_IXP2000_PCIB;
	} else if (dev->bus->number == 0x01 && PCI_SLOT(dev->devfn) == 0x00) {
		/* 82559 ethernet.  */
		irq = IRQ_IXP2000_PCIA;
	} else {
		printk(KERN_INFO "enp2611_pci_map_irq for unknown device\n");
		irq = IRQ_IXP2000_PCI;
	}

	printk(KERN_INFO "Assigned IRQ %d to PCI:%d:%d:%d\n", irq,
		dev->bus->number, PCI_SLOT(dev->devfn), PCI_FUNC(dev->devfn));

	return irq;
}

struct hw_pci enp2611_pci __initdata = {
	.nr_controllers	= 1,
	.setup		= enp2611_pci_setup,
	.preinit	= enp2611_pci_preinit,
	.scan		= enp2611_pci_scan_bus,
	.map_irq	= enp2611_pci_map_irq,
};

int __init enp2611_pci_init(void)
{
	pci_common_init(&enp2611_pci);
	return 0;
}

subsys_initcall(enp2611_pci_init);


/*************************************************************************
 * ENP-2611 Machine Intialization
 *************************************************************************/
static struct flash_platform_data enp2611_flash_platform_data = {
	.map_name	= "cfi_probe",
	.width		= 1,
};

static struct ixp2000_flash_data enp2611_flash_data = {
	.platform_data	= &enp2611_flash_platform_data,
	.nr_banks	= 1
};

static struct resource enp2611_flash_resource = {
	.start		= 0xc4000000,
	.end		= 0xc4000000 + 0x00ffffff,
	.flags		= IORESOURCE_MEM,
};

static struct platform_device enp2611_flash = {
	.name		= "IXP2000-Flash",
	.id		= 0,
	.dev		= {
		.platform_data = &enp2611_flash_data,
	},
	.num_resources	= 1,
	.resource	= &enp2611_flash_resource,
};

static struct platform_device *enp2611_devices[] __initdata = {
	&enp2611_flash
};

static void __init enp2611_init_machine(void)
{
	platform_add_devices(enp2611_devices, ARRAY_SIZE(enp2611_devices));
}


#ifdef CONFIG_ARCH_ENP2611
MACHINE_START(ENP2611, "Radisys ENP-2611 PCI network processor board")
	MAINTAINER("Lennert Buytenhek <buytenh@wantstofly.org>")
	BOOT_MEM(0x00000000, IXP2000_UART_PHYS_BASE, IXP2000_UART_VIRT_BASE)
	BOOT_PARAMS(0x00000100)
	MAPIO(ixp2000_map_io)
	INITIRQ(ixp2000_init_irq)
	.timer		= &enp2611_timer,
	INIT_MACHINE(enp2611_init_machine)
MACHINE_END
#endif


