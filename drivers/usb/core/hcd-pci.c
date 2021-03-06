/*
 * (C) Copyright David Brownell 2000-2002
 * 
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/config.h>

#ifdef CONFIG_USB_DEBUG
	#define DEBUG
#else
	#undef DEBUG
#endif

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <asm/io.h>
#include <asm/irq.h>
#include <linux/usb.h>
#include "hcd.h"


/* PCI-based HCs are normal, but custom bus glue should be ok */


/*-------------------------------------------------------------------------*/

/* configure so an HC device and id are always provided */
/* always called with process context; sleeping is OK */

/**
 * usb_hcd_pci_probe - initialize PCI-based HCDs
 * @dev: USB Host Controller being probed
 * @id: pci hotplug id connecting controller to HCD framework
 * Context: !in_interrupt()
 *
 * Allocates basic PCI resources for this USB host controller, and
 * then invokes the start() method for the HCD associated with it
 * through the hotplug entry's driver_data.
 *
 * Store this function in the HCD's struct pci_driver as probe().
 */
int usb_hcd_pci_probe (struct pci_dev *dev, const struct pci_device_id *id)
{
	struct hc_driver	*driver;
	struct usb_hcd		*hcd;
	int			retval;

	if (usb_disabled())
		return -ENODEV;

	if (!id || !(driver = (struct hc_driver *) id->driver_data))
		return -EINVAL;

	if (pci_enable_device (dev) < 0)
		return -ENODEV;
	dev->current_state = 0;
	dev->dev.power.power_state = 0;
	
        if (!dev->irq) {
        	dev_err (&dev->dev,
			"Found HC with no IRQ.  Check BIOS/PCI %s setup!\n",
			pci_name(dev));
   	        retval = -ENODEV;
		goto err1;
        }

	hcd = usb_create_hcd (driver, &dev->dev, pci_name(dev));
	if (!hcd) {
		retval = -ENOMEM;
		goto err1;
	}

	if (driver->flags & HCD_MEMORY) {	// EHCI, OHCI
		hcd->rsrc_start = pci_resource_start (dev, 0);
		hcd->rsrc_len = pci_resource_len (dev, 0);
		if (!request_mem_region (hcd->rsrc_start, hcd->rsrc_len,
				driver->description)) {
			dev_dbg (&dev->dev, "controller already in use\n");
			retval = -EBUSY;
			goto err2;
		}
		hcd->regs = ioremap_nocache (hcd->rsrc_start, hcd->rsrc_len);
		if (hcd->regs == NULL) {
			dev_dbg (&dev->dev, "error mapping memory\n");
			retval = -EFAULT;
			goto err3;
		}

	} else { 				// UHCI
		int	region;

		for (region = 0; region < PCI_ROM_RESOURCE; region++) {
			if (!(pci_resource_flags (dev, region) &
					IORESOURCE_IO))
				continue;

			hcd->rsrc_start = pci_resource_start (dev, region);
			hcd->rsrc_len = pci_resource_len (dev, region);
			if (request_region (hcd->rsrc_start, hcd->rsrc_len,
					driver->description))
				break;
		}
		if (region == PCI_ROM_RESOURCE) {
			dev_dbg (&dev->dev, "no i/o regions available\n");
			retval = -EBUSY;
			goto err1;
		}
	}

#ifdef CONFIG_PCI_NAMES
	hcd->product_desc = dev->pretty_name;
#endif

	pci_set_master (dev);

	retval = usb_add_hcd (hcd, dev->irq, SA_SHIRQ);
	if (retval != 0)
		goto err4;
	return retval;

 err4:
	if (driver->flags & HCD_MEMORY) {
		iounmap (hcd->regs);
 err3:
		release_mem_region (hcd->rsrc_start, hcd->rsrc_len);
	} else
		release_region (hcd->rsrc_start, hcd->rsrc_len);
 err2:
	usb_put_hcd (hcd);
 err1:
	pci_disable_device (dev);
	dev_err (&dev->dev, "init %s fail, %d\n", pci_name(dev), retval);
	return retval;
} 
EXPORT_SYMBOL (usb_hcd_pci_probe);


/* may be called without controller electrically present */
/* may be called with controller, bus, and devices active */

/**
 * usb_hcd_pci_remove - shutdown processing for PCI-based HCDs
 * @dev: USB Host Controller being removed
 * Context: !in_interrupt()
 *
 * Reverses the effect of usb_hcd_pci_probe(), first invoking
 * the HCD's stop() method.  It is always called from a thread
 * context, normally "rmmod", "apmd", or something similar.
 *
 * Store this function in the HCD's struct pci_driver as remove().
 */
void usb_hcd_pci_remove (struct pci_dev *dev)
{
	struct usb_hcd		*hcd;

	hcd = pci_get_drvdata(dev);
	if (!hcd)
		return;

	usb_remove_hcd (hcd);
	if (hcd->driver->flags & HCD_MEMORY) {
		iounmap (hcd->regs);
		release_mem_region (hcd->rsrc_start, hcd->rsrc_len);
	} else {
		release_region (hcd->rsrc_start, hcd->rsrc_len);
	}
	usb_put_hcd (hcd);
	pci_disable_device(dev);
}
EXPORT_SYMBOL (usb_hcd_pci_remove);


#ifdef	CONFIG_PM

static char __attribute_used__ *pci_state(u32 state)
{
	switch (state) {
	case 0:		return "D0";
	case 1:		return "D1";
	case 2:		return "D2";
	case 3:		return "D3hot";
	case 4:		return "D3cold";
	}
	return NULL;
}

/**
 * usb_hcd_pci_suspend - power management suspend of a PCI-based HCD
 * @dev: USB Host Controller being suspended
 * @state: state that the controller is going into
 *
 * Store this function in the HCD's struct pci_driver as suspend().
 */
int usb_hcd_pci_suspend (struct pci_dev *dev, u32 state)
{
	struct usb_hcd		*hcd;
	int			retval = 0;
	int			has_pci_pm;

	hcd = pci_get_drvdata(dev);

	/* even when the PCI layer rejects some of the PCI calls
	 * below, HCs can try global suspend and reduce DMA traffic.
	 * PM-sensitive HCDs may already have done this.
	 */
	has_pci_pm = pci_find_capability(dev, PCI_CAP_ID_PM);
	if (state > 4)
		state = 4;

	switch (hcd->state) {

	/* entry if root hub wasn't yet suspended ... from sysfs,
	 * without autosuspend, or if USB_SUSPEND isn't configured.
	 */
	case USB_STATE_RUNNING:
		hcd->state = USB_STATE_QUIESCING;
		retval = hcd->driver->suspend (hcd, state);
		if (retval) {
			dev_dbg (hcd->self.controller, 
					"suspend fail, retval %d\n",
					retval);
			break;
		}
		hcd->state = HCD_STATE_SUSPENDED;
		/* FALLTHROUGH */

	/* entry with CONFIG_USB_SUSPEND, or hcds that autosuspend: the
	 * controller and/or root hub will already have been suspended,
	 * but it won't be ready for a PCI resume call.
	 *
	 * FIXME only CONFIG_USB_SUSPEND guarantees hub_suspend() will
	 * have been called, otherwise root hub timers still run ...
	 */
	case HCD_STATE_SUSPENDED:
		if (state <= dev->current_state)
			break;

		/* no DMA or IRQs except in D0 */
		if (!dev->current_state) {
			pci_save_state (dev);
			pci_disable_device (dev);
			free_irq (hcd->irq, hcd);
		}

		if (!has_pci_pm) {
			dev_dbg (hcd->self.controller, "--> PCI D0/legacy\n");
			break;
		}

		/* POLICY: ignore D1/D2/D3hot differences;
		 * we know D3hot will always work.
		 */
		retval = pci_set_power_state (dev, state);
		if (retval < 0 && state < 3) {
			retval = pci_set_power_state (dev, 3);
			if (retval == 0)
				state = 3;
		}
		if (retval == 0) {
			dev_dbg (hcd->self.controller, "--> PCI %s\n",
					pci_state(dev->current_state));
#ifdef	CONFIG_USB_SUSPEND
			pci_enable_wake (dev, state, hcd->remote_wakeup);
			pci_enable_wake (dev, 4, hcd->remote_wakeup);
#endif
		} else if (retval < 0) {
			dev_dbg (&dev->dev, "PCI %s suspend fail, %d\n",
					pci_state(state), retval);
			(void) usb_hcd_pci_resume (dev);
			break;
		}
		break;
	default:
		dev_dbg (hcd->self.controller, "hcd state %d; not suspended\n",
			hcd->state);
		retval = -EINVAL;
		break;
	}

	/* update power_state **ONLY** to make sysfs happier */
	if (retval == 0)
		dev->dev.power.power_state = state;
	return retval;
}
EXPORT_SYMBOL (usb_hcd_pci_suspend);

/**
 * usb_hcd_pci_resume - power management resume of a PCI-based HCD
 * @dev: USB Host Controller being resumed
 *
 * Store this function in the HCD's struct pci_driver as resume().
 */
int usb_hcd_pci_resume (struct pci_dev *dev)
{
	struct usb_hcd		*hcd;
	int			retval;
	int			has_pci_pm;

	hcd = pci_get_drvdata(dev);
	if (hcd->state != HCD_STATE_SUSPENDED) {
		dev_dbg (hcd->self.controller, 
				"can't resume, not suspended!\n");
		return 0;
	}
	has_pci_pm = pci_find_capability(dev, PCI_CAP_ID_PM);

	/* D3cold resume isn't usually reported this way... */
	dev_dbg(hcd->self.controller, "resume from PCI %s%s\n",
			pci_state(dev->current_state),
			has_pci_pm ? "" : " (legacy)");

	hcd->state = USB_STATE_RESUMING;

	if (has_pci_pm)
		pci_set_power_state (dev, 0);
	dev->dev.power.power_state = 0;
	retval = request_irq (dev->irq, usb_hcd_irq, SA_SHIRQ,
				hcd->driver->description, hcd);
	if (retval < 0) {
		dev_err (hcd->self.controller,
			"can't restore IRQ after resume!\n");
		return retval;
	}
	hcd->saw_irq = 0;
	pci_restore_state (dev);
#ifdef	CONFIG_USB_SUSPEND
	pci_enable_wake (dev, dev->current_state, 0);
	pci_enable_wake (dev, 4, 0);
#endif

	retval = hcd->driver->resume (hcd);
	if (!HCD_IS_RUNNING (hcd->state)) {
		dev_dbg (hcd->self.controller, 
				"resume fail, retval %d\n", retval);
		usb_hc_died (hcd);
	}

	return retval;
}
EXPORT_SYMBOL (usb_hcd_pci_resume);

#endif	/* CONFIG_PM */


