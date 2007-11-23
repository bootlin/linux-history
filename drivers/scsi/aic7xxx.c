/*+M*************************************************************************
 * Adaptec 274x/284x/294x device driver for Linux.
 *
 * Copyright (c) 1994 John Aycock
 *   The University of Calgary Department of Computer Science.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * Sources include the Adaptec 1740 driver (aha1740.c), the Ultrastor 24F
 * driver (ultrastor.c), various Linux kernel source, the Adaptec EISA
 * config file (!adp7771.cfg), the Adaptec AHA-2740A Series User's Guide,
 * the Linux Kernel Hacker's Guide, Writing a SCSI Device Driver for Linux,
 * the Adaptec 1542 driver (aha1542.c), the Adaptec EISA overlay file
 * (adp7770.ovl), the Adaptec AHA-2740 Series Technical Reference Manual,
 * the Adaptec AIC-7770 Data Book, the ANSI SCSI specification, the
 * ANSI SCSI-2 specification (draft 10c), ...
 *
 * ----------------------------------------------------------------
 *  Modified to include support for wide and twin bus adapters,
 *  DMAing of SCBs, tagged queueing, IRQ sharing, bug fixes,
 *  and other rework of the code.
 *
 *  Parts of this driver are based on the FreeBSD driver by Justin
 *  T. Gibbs.
 *
 *  A Boot time option was also added for not resetting the scsi bus.
 *
 *    Form:  aic7xxx=extended,no_reset
 *
 *    -- Daniel M. Eischen, deischen@iworks.InterWorks.org, 04/03/95
 *
 *  $Id: aic7xxx.c,v 2.10 1995/11/10 10:49:14 deang Exp $
 *-M*************************************************************************/

#ifdef MODULE
#include <linux/module.h>
#endif

#include <stdarg.h>
#include <asm/io.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/ioport.h>
#include <linux/bios32.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/pci.h>
#include <linux/proc_fs.h>
#include <linux/blk.h>
#include "sd.h"
#include "scsi.h"
#include "hosts.h"
#include "aic7xxx.h"
#include <linux/stat.h>

#include <linux/config.h>	/* for CONFIG_PCI */

struct proc_dir_entry proc_scsi_aic7xxx = {
    PROC_SCSI_AIC7XXX, 7, "aic7xxx",
    S_IFDIR | S_IRUGO | S_IXUGO, 2
};

#define AIC7XXX_C_VERSION  "$Revision: 2.10 $"

#define NUMBER(arr)     (sizeof(arr) / sizeof(arr[0]))
#define MIN(a,b)        ((a < b) ? a : b)
#define ALL_TARGETS -1
#ifndef TRUE
#  define TRUE 1
#endif
#ifndef FALSE
#  define FALSE 0
#endif

/*
 * Defines for PCI bus support, testing twin bus support, DMAing of
 * SCBs, tagged queueing, commands (SCBs) per lun, and SCSI bus reset
 * delay time.
 *
 *   o PCI bus support - this has been implemented and working since
 *     the December 1, 1994 release of this driver. If you don't have
 *     a PCI bus and do not wish to configure your kernel with PCI
 *     support, then make sure this define is set to the cprrect
 *     define for PCI support (CONFIG_PCI) and configure your kernel
 *     without PCI support (make config).
 *
 *   o Twin bus support - this has been tested and does work.
 *
 *   o DMAing of SCBs - thanks to Kai Makisara, this now works.
 *     This define is now taken out and DMAing of SCBs is always
 *     performed (8/12/95 - DE).
 *
 *   o Tagged queueing - this driver is capable of tagged queueing
 *     but I am unsure as to how well the higher level driver implements
 *     tagged queueing. Therefore, the maximum commands per lun is
 *     set to 2. If you want to implement tagged queueing, ensure
 *     this define is not commented out.
 *
 *   o Sharing IRQs - allowed for sharing of IRQs. This will allow
 *     for multiple aic7xxx host adapters sharing the same IRQ, but
 *     not for sharing IRQs with other devices. The higher level
 *     PCI code and interrupt handling needs to be modified to
 *     support this.
 *
 *   o Commands per lun - If tagged queueing is enabled, then you
 *     may want to try increasing AIC7XXX_CMDS_PER_LUN to more
 *     than 2.  By default, we limit the SCBs per lun to 2 with
 *     or without tagged queueing enabled.  If tagged queueing is
 *     disabled, the sequencer will keep the 2nd SCB in the input
 *     queue until the first one completes - so it is OK to to have
 *     more than 1 SCB queued.  If tagged queueing is enabled, then
 *     the sequencer will attempt to send the 2nd SCB to the device
 *     while the first SCB is executing and the device is disconnected.
 *     For adapters limited to 4 SCBs, you may want to actually
 *     decrease the commands per lun to 1, if you often have more
 *     than 2 devices active at the same time.  This will allocate
 *     1 SCB for each device and ensure that there will always be
 *     a free SCB for up to 4 devices active at the same time.
 *
 *  Daniel M. Eischen, deischen@iworks.InterWorks.org, 03/11/95
 */

/* Uncomment this for testing twin bus support. */
#define AIC7XXX_TWIN_SUPPORT

/* Uncomment this for tagged queueing. */
/* #define AIC7XXX_TAGGED_QUEUEING */

/* Uncomment this for allowing sharing of IRQs. */
#define AIC7XXX_SHARE_IRQS

/*
 * You can try raising me if tagged queueing is enabled, or lowering
 * me if you only have 4 SCBs.
 */
#define AIC7XXX_CMDS_PER_LUN 2

/* Set this to the delay in seconds after SCSI bus reset. */
#define AIC7XXX_RESET_DELAY 15

/*
 * Uncomment the following define for collection of SCSI transfer statistics
 * for the /proc filesystem.
 *
 * NOTE: This does affect performance since it has to maintain statistics.
 */
/* #define AIC7XXX_PROC_STATS */

/*
 * Define this to use polling rather than using kernel support for waking
 * up a waiting process.
 */
#define AIC7XXX_POLL

/*
 * Controller type and options
 */
typedef enum {
  AIC_NONE,
  AIC_274x,    /* EISA aic7770 */
  AIC_284x,    /* VLB  aic7770 */
  AIC_7870,    /* PCI  aic7870/aic7871 motherboard or 294x */
  AIC_7850,    /* PCI  aic7850 */
  AIC_7872,    /* PCI  aic7870 on 394x */
  AIC_7880,    /* PCI  aic7880/aic7881 motherboard or 294x Ultra */
  AIC_7882,    /* PCI  aic7870 on 394x Ultra */
} aha_type;

typedef enum {
  AIC_SINGLE,  /* Single Channel */
  AIC_TWIN,    /* Twin Channel */
  AIC_WIDE     /* Wide Channel */
} aha_bus_type;

typedef enum {
  AIC_UNKNOWN,
  AIC_ENABLED,
  AIC_DISABLED
} aha_status_type;

typedef enum {
  LIST_HEAD,
  LIST_SECOND,
  LIST_TAIL
} insert_type;

/*
 * There should be a specific return value for this in scsi.h, but
 * it seems that most drivers ignore it.
 */
#define DID_UNDERFLOW   DID_ERROR

/*
 *  What we want to do is have the higher level scsi driver requeue
 *  the command to us. There is no specific driver status for this
 *  condition, but the higher level scsi driver will requeue the
 *  command on a DID_BUS_BUSY error.
 */
#define DID_RETRY_COMMAND DID_BUS_BUSY

/*
 * EISA/VL-bus stuff
 */
#define MINSLOT		1
#define MAXSLOT		15
#define SLOTBASE(x)	((x) << 12)
#define MAXIRQ		15

/*
 * Standard EISA Host ID regs  (Offset from slot base)
 */
#define HID0(x)         ((x) + 0xC80)   /* 0,1: msb of ID2, 2-7: ID1      */
#define HID1(x)         ((x) + 0xC81)   /* 0-4: ID3, 5-7: LSB ID2         */
#define HID2(x)         ((x) + 0xC82)   /* product                        */
#define HID3(x)         ((x) + 0xC83)   /* firmware revision              */

/*
 * AIC-7770 I/O range to reserve for a card
 */
#define MINREG(x)	((x) + 0xC00ul)
#define MAXREG(x)	((x) + 0xCBFul)

/* -------------------- AIC-7770 offset definitions ----------------------- */

/*
 * SCSI Sequence Control (p. 3-11).
 * Each bit, when set starts a specific SCSI sequence on the bus
 */
#define SCSISEQ(x)		((x) + 0xC00ul)
#define		TEMODEO		0x80
#define		ENSELO		0x40
#define		ENSELI		0x20
#define		ENRSELI		0x10
#define		ENAUTOATNO	0x08
#define		ENAUTOATNI	0x04
#define		ENAUTOATNP	0x02
#define		SCSIRSTO	0x01


/*
 * SCSI Transfer Control 0 Register (pp. 3-13).
 * Controls the SCSI module data path.
 */
#define	SXFRCTL0(x)		((x) + 0xC01ul)
#define		DFON		0x80
#define		DFPEXP		0x40
#define		ULTRAEN		0x20
#define		CLRSTCNT	0x10
#define		SPIOEN		0x08
#define		SCAMEN		0x04
#define		CLRCHN		0x02
/*  UNUSED			0x01 */

/*
 * SCSI Transfer Control 1 Register (pp. 3-14,15).
 * Controls the SCSI module data path.
 */
#define SXFRCTL1(x)		((x) + 0xC02ul)
#define		BITBUCKET	0x80
#define		SWRAPEN		0x40
#define		ENSPCHK		0x20
#define		STIMESEL	0x18
#define		ENSTIMER	0x04
#define		ACTNEGEN	0x02
#define		STPWEN		0x01		/* Powered Termination */

/*
 * SCSI Control Signal Read Register (p. 3-15).
 * Reads the actual state of the SCSI bus pins
 */
#define SCSISIGI(x)		((x) + 0xC03ul)
#define		CDI		0x80
#define		IOI		0x40
#define		MSGI		0x20
#define		ATNI		0x10
#define		SELI		0x08
#define		BSYI		0x04
#define		REQI		0x02
#define		ACKI		0x01

/*
 * SCSI Contol Signal Write Register (p. 3-16).
 * Writing to this register modifies the control signals on the bus. Only
 * those signals that are allowed in the current mode (Initiator/Target) are
 * asserted.
 */
#define SCSISIGO(x)		((x) + 0xC03ul)
#define		CDO		0x80
#define		IOO		0x40
#define		MSGO		0x20
#define		ATNO		0x10
#define		SELO		0x08
#define		BSYO		0x04
#define		REQO		0x02
#define		ACKO		0x01

/*
 * SCSI Rate
 */
#define SCSIRATE(x)		((x) + 0xC04ul)
#define WIDEXFER		0x80		/* Wide transfer control */
#define SXFR			0x70		/* Sync transfer rate */
#define SOFS			0x0F		/* Sync offset */

/*
 * SCSI ID (p. 3-18).
 * Contains the ID of the board and the current target on the
 * selected channel
 */
#define SCSIID(x)		((x) + 0xC05ul)
#define		TID		0xF0		/* Target ID mask */
#define		OID		0x0F		/* Our ID mask */

/*
 * SCSI Transfer Count (pp. 3-19,20)
 * These registers count down the number of bytes transfered
 * across the SCSI bus.  The counter is decremented only once
 * the data has been safely transfered.  SDONE in SSTAT0 is
 * set when STCNT goes to 0
 */
#define STCNT(x)		((x) + 0xC08ul)

/*
 * SCSI Status 0 (p. 3-21)
 * Contains one set of SCSI Interrupt codes
 * These are most likely of interest to the sequencer
 */
#define SSTAT0(x)		((x) + 0xC0Bul)
#define		TARGET		0x80		/* Board is a target */
#define		SELDO		0x40		/* Selection Done */
#define		SELDI		0x20		/* Board has been selected */
#define		SELINGO		0x10		/* Selection In Progress */
#define		SWRAP		0x08		/* 24bit counter wrap */
#define		SDONE		0x04		/* STCNT = 0x000000 */
#define		SPIORDY		0x02		/* SCSI PIO Ready */
#define		DMADONE		0x01		/* DMA transfer completed */

/*
 * Clear SCSI Interrupt 1 (p. 3-23)
 * Writing a 1 to a bit clears the associated SCSI Interrupt in SSTAT1.
 */
#define CLRSINT1(x)		((x) + 0xC0Cul)
#define		CLRSELTIMEO	0x80
#define		CLRATNO		0x40
#define		CLRSCSIRSTI	0x20
/*  UNUSED			0x10 */
#define		CLRBUSFREE	0x08
#define		CLRSCSIPERR	0x04
#define		CLRPHASECHG	0x02
#define		CLRREQINIT	0x01

/*
 * SCSI Status 1 (p. 3-24)
 * These interrupt bits are of interest to the kernel driver
 */
#define SSTAT1(x)		((x) + 0xC0Cul)
#define		SELTO		0x80
#define		ATNTARG 	0x40
#define		SCSIRSTI	0x20
#define		PHASEMIS	0x10
#define		BUSFREE		0x08
#define		SCSIPERR	0x04
#define		PHASECHG	0x02
#define		REQINIT		0x01

/*
 * SCSI Interrrupt Mode 1 (pp. 3-28,29).
 * Set bits in this register enable the corresponding
 * interrupt source.
 */
#define	SIMODE1(x)		((x) + 0xC11ul)
#define		ENSELTIMO	0x80
#define		ENATNTARG	0x40
#define		ENSCSIRST	0x20
#define		ENPHASEMIS	0x10
#define		ENBUSFREE	0x08
#define		ENSCSIPERR	0x04
#define		ENPHASECHG	0x02
#define		ENREQINIT	0x01

/*
 * SCSI/Host Address (p. 3-30)
 * These registers hold the host address for the byte about to be
 * transfered on the SCSI bus.  They are counted up in the same
 * manner as STCNT is counted down.  SHADDR should always be used
 * to determine the address of the last byte transfered since HADDR
 * can be squewed by write ahead.
 */
#define	SHADDR(x)		((x) + 0xC14ul)

/*
 * Selection/Reselection ID (p. 3-31)
 * Upper four bits are the device id. The ONEBIT is set when the re/selecting
 * device did not set its own ID.
 */
#define SELID(x)		((x) + 0xC19ul)
#define		SELID_MASK	0xF0
#define		ONEBIT		0x08
/*  UNUSED			0x07 */

/*
 * SCSI Block Control (p. 3-32)
 * Controls Bus type and channel selection. In a twin channel configuration
 * addresses 0x00-0x1E are gated to the appropriate channel based on this
 * register. SELWIDE allows for the coexistence of 8bit and 16bit devices
 * on a wide bus.
 */
#define SBLKCTL(x)		((x) + 0xC1Ful)
/*  UNUSED			0xC0 */
#define		DIAGLEDEN	0x80
#define		DIAGLEDON	0x40
#define		AUTOFLUSHDIS	0x20		/* used for Rev C check */
/*  UNUSED			0x10 */
#define		SELBUS_MASK	0x0F
#define		SELBUSB		0x08
/*  UNUSED			0x04 */
#define		SELWIDE		0x02
/*  UNUSED			0x01 */
#define		SELSINGLE	0x00

/*
 * Sequencer Control (p. 3-33)
 * Error detection mode and speed configuration
 */
#define SEQCTL(x)		((x) + 0xC60ul)
#define		PERRORDIS	0x80
#define		PAUSEDIS	0x40
#define		FAILDIS		0x20
#define 	FASTMODE	0x10
#define		BRKADRINTEN	0x08
#define		STEP		0x04
#define		SEQRESET	0x02
#define		LOADRAM		0x01

/*
 * Sequencer RAM Data (p. 3-34)
 * Single byte window into the Scratch Ram area starting at the address
 * specified by SEQADDR0 and SEQADDR1. To write a full word, simply write
 * four bytes in sucessesion. The SEQADDRs will increment after the most
 * significant byte is written
 */
#define SEQRAM(x)		((x) + 0xC61ul)

/*
 * Sequencer Address Registers (p. 3-35)
 * Only the first bit of SEQADDR1 holds addressing information
 */
#define SEQADDR0(x)		((x) + 0xC62ul)
#define SEQADDR1(x)		((x) + 0xC63ul)

#define ACCUM(x)		((x) + 0xC64ul)		/* accumulator */
#define SINDEX(x)		((x) + 0xC65ul)

/*
 * Board Control (p. 3-43)
 */
#define BCTL(x)			((x) + 0xC84ul)
/*   RSVD			0xF0 */
#define		ACE		0x08	/* Support for external processors */
/*   RSVD			0x06 */
#define		ENABLE		0x01

/*
 * Bus On/Off Time (p. 3-44)
 */
#define BUSTIME(x)		((x) + 0xC85ul)
#define		BOFF		0xF0
#define		BON		0x0F

/*
 * Bus Speed (p. 3-45)
 */
#define	BUSSPD(x)		((x) + 0xC86ul)
#define		DFTHRSH		0xC0
#define		STBOFF		0x38
#define		STBON		0x07

/*
 * Host Control (p. 3-47) R/W
 * Overal host control of the device.
 */
#define HCNTRL(x)		((x) + 0xC87ul)
/*    UNUSED			0x80 */
#define		POWRDN		0x40
/*    UNUSED			0x20 */
#define		SWINT		0x10
#define		IRQMS		0x08
#define		PAUSE		0x04
#define		INTEN		0x02
#define		CHIPRST		0x01
#define		REQ_PAUSE	IRQMS | INTEN | PAUSE
#define		UNPAUSE_274X	IRQMS | INTEN
#define		UNPAUSE_284X	INTEN
#define		UNPAUSE_294X	IRQMS | INTEN

/*
 * Host Address (p. 3-48)
 * This register contains the address of the byte about
 * to be transfered across the host bus.
 */
#define HADDR(x)		((x) + 0xC88ul)

/*
 * SCB Pointer (p. 3-49)
 * Gate one of the four SCBs into the SCBARRAY window.
 */
#define SCBPTR(x)		((x) + 0xC90ul)

/*
 * Interrupt Status (p. 3-50)
 * Status for system interrupts
 */
#define INTSTAT(x)		((x) + 0xC91ul)
#define		SEQINT_MASK	0xF0		/* SEQINT Status Codes */
#define			BAD_PHASE	0x00
#define			SEND_REJECT	0x10
#define			NO_IDENT	0x20
#define			NO_MATCH	0x30
#define			MSG_SDTR	0x40
#define			MSG_WDTR	0x50
#define			MSG_REJECT	0x60
#define			BAD_STATUS	0x70
#define			RESIDUAL	0x80
#define			ABORT_TAG	0x90
#define			AWAITING_MSG	0xA0
#define			IMMEDDONE	0xB0
#define 	BRKADRINT 0x08
#define		SCSIINT	  0x04
#define		CMDCMPLT  0x02
#define		SEQINT    0x01
#define		INT_PEND  (BRKADRINT | SEQINT | SCSIINT | CMDCMPLT)

/*
 * Hard Error (p. 3-53)
 * Reporting of catastrophic errors. You usually cannot recover from
 * these without a full board reset.
 */
#define ERROR(x)		((x) + 0xC92ul)
/*    UNUSED			0xF0 */
#define		PARERR		0x08
#define		ILLOPCODE	0x04
#define		ILLSADDR	0x02
#define		ILLHADDR	0x01

/*
 * Clear Interrupt Status (p. 3-52)
 */
#define CLRINT(x)		((x) + 0xC92ul)
#define		CLRBRKADRINT	0x08
#define		CLRSCSIINT	0x04
#define		CLRCMDINT 	0x02
#define		CLRSEQINT 	0x01

/*
 * SCB Auto Increment (p. 3-59)
 * Byte offset into the SCB Array and an optional bit to allow auto
 * incrementing of the address during download and upload operations
 */
#define SCBCNT(x)		((x) + 0xC9Aul)
#define		SCBAUTO		0x80
#define		SCBCNT_MASK	0x1F

/*
 * Queue In FIFO (p. 3-60)
 * Input queue for queued SCBs (commands that the seqencer has yet to start)
 */
#define QINFIFO(x)		((x) + 0xC9Bul)

/*
 * Queue In Count (p. 3-60)
 * Number of queued SCBs
 */
#define QINCNT(x)		((x) + 0xC9Cul)

/*
 * Queue Out FIFO (p. 3-61)
 * Queue of SCBs that have completed and await the host
 */
#define QOUTFIFO(x)		((x) + 0xC9Dul)

/*
 * Queue Out Count (p. 3-61)
 * Number of queued SCBs in the Out FIFO
 */
#define QOUTCNT(x)		((x) + 0xC9Eul)

#define SCBARRAY(x)		((x) + 0xCA0ul)

/* ---------------- END AIC-7770 Register Definitions ----------------- */

/* --------------------- AHA-2840-only definitions -------------------- */

#define SEECTL_2840(x)		((x) + 0xCC0ul)
/*    UNUSED			0xF8 */
#define		CS_2840		0x04
#define		CK_2840		0x02
#define		DO_2840		0x01

#define STATUS_2840(x)		((x) + 0xCC1ul)
#define		EEPROM_TF	0x80
#define		BIOS_SEL	0x60
#define		ADSEL		0x1E
#define		DI_2840		0x01

/* --------------------- AIC-7870-only definitions -------------------- */

#define DSPCISTATUS(x)	 	((x) + 0xC86ul)
#define 	DFTHRESH        0xC0

/*
 * Serial EEPROM Control (p. 4-92 in 7870 Databook)
 * Controls the reading and writing of an external serial 1-bit
 * EEPROM Device.  In order to access the serial EEPROM, you must
 * first set the SEEMS bit that generates a request to the memory
 * port for access to the serial EEPROM device.  When the memory
 * port is not busy servicing another request, it reconfigures
 * to allow access to the serial EEPROM.  When this happens, SEERDY
 * gets set high to verify that the memory port access has been
 * granted.  See aic7xxx_read_eprom for detailed information on
 * the protocol necessary to read the serial EEPROM.
 */
#define SEECTL(x)		((x) + 0xC1Eul)
#define		EXTARBACK	0x80
#define		EXTARBREQ	0x40
#define		SEEMS		0x20
#define		SEERDY		0x10
#define		SEECS		0x08
#define		SEECK		0x04
#define		SEEDO		0x02
#define		SEEDI		0x01

#define DEVREVID  		0x08ul

#define	DEVSTATUS		0x40ul
#define		MPORTMODE	0x04	/* aic7870 only */
#define		RAMPSM		0x02	/* aic7870 only */
#define		VOLSENSE	0x01

#define	DEVCONFIG		0x41ul
#define		SCBRAMSEL	0x80
#define		MRDCEN		0x40
#define		EXTSCBTIME	0x20	/* aic7870 only */
#define		EXTSCBPEN	0x10	/* aic7870 only */
#define		BERREN		0x08
#define		DACEN		0x04
#define		STPWLEVEL	0x02
#define		DIFACTNEGEN	0x01	/* aic7870 only */

/* Scratch RAM offset definitions */

/* ---------------------- Scratch RAM Offsets ------------------------- */
/* These offsets are either to values that are initialized by the board's
 * BIOS or are specified by the Linux sequencer code. If I can figure out
 * how to read the EISA configuration info at probe time, the cards could
 * be run without BIOS support installed
 */

/*
 * 1 byte per target starting at this address for configuration values
 */
#define HA_TARG_SCRATCH(x)	((x) + 0xC20ul)

/*
 * The sequencer will stick the first byte of any rejected message here so
 * we can see what is getting thrown away.
 */
#define HA_REJBYTE(x)		((x) + 0xC31ul)

/*
 * Bit vector of targets that have disconnection disabled.
 */
#define	HA_DISC_DSB(x)		((x) + 0xC32ul)

/*
 * Length of pending message
 */
#define HA_MSG_LEN(x)		((x) + 0xC34ul)

/*
 * Outgoing Message Body
 */
#define HA_MSG_START(x)		((x) + 0xC35ul)

/*
 * These are offsets into the card's scratch ram. Some of the values are
 * specified in the AHA2742 technical reference manual and are initialized
 * by the BIOS at boot time.
 */
#define HA_ARG_1(x)		((x) + 0xC4Aul)	/* sdtr <-> rate parameters */
#define HA_RETURN_1(x)		((x) + 0xC4Aul)
#define		SEND_SENSE	0x80
#define		SEND_SDTR 	0x80
#define		SEND_WDTR 	0x80
#define		SEND_REJ	0x40

#define	SG_COUNT(x)		((x) + 0xC4Dul)
#define	SG_NEXT(x)		((x) + 0xC4Eul)
#define HA_SIGSTATE(x)		((x) + 0xC4Bul)	/* value in SCSISIGO */
#define HA_SCBCOUNT(x)		((x) + 0xC52ul)	/* number of hardware SCBs */

#define HA_FLAGS(x)		((x) + 0xC53ul)	/* TWIN and WIDE bus flags */
#define		SINGLE_BUS	0x00
#define		TWIN_BUS	0x01
#define		WIDE_BUS	0x02
#define		ACTIVE_MSG	0x20
#define		IDENTIFY_SEEN	0x40
#define		RESELECTING	0x80

#define HA_ACTIVE0(x)		((x) + 0xC54ul)	/* Active bits; targets 0-7 */
#define HA_ACTIVE1(x)		((x) + 0xC55ul)	/* Active bits; targets 8-15 */
#define	SAVED_TCL(x)		((x) + 0xC56ul)	/* Saved target, channel, LUN */
#define WAITING_SCBH(x)		((x) + 0xC57ul) /* Head of disconnected targets list. */
#define WAITING_SCBT(x)		((x) + 0xC58ul) /* Tail of disconnected targets list. */

#define HA_SCSICONF(x)		((x) + 0xC5Aul)	/* SCSI config register */
#define HA_INTDEF(x)		((x) + 0xC5Cul)	/* interrupt def'n register */
#define HA_HOSTCONF(x)		((x) + 0xC5Dul)	/* host config def'n register */

#define HA_274_BIOSCTRL(x)	((x) + 0xC5Ful) /* BIOS enabled for 274x */
#define BIOSMODE		0x30
#define BIOSDISABLED		0x30

#define MSG_ABORT		0x06
#define	MSG_BUS_DEVICE_RESET	0x0C
#define BUS_8_BIT		0x00
#define BUS_16_BIT		0x01
#define BUS_32_BIT		0x02


/*
 *
 * Define the format of the SEEPROM registers (16 bits).
 *
 */
struct seeprom_config {

/*
 * SCSI ID Configuration Flags
 */
#define CFXFER		0x0007		/* synchronous transfer rate */
#define CFSYNCH		0x0008		/* enable synchronous transfer */
#define CFDISC		0x0010		/* enable disconnection */
#define CFWIDEB		0x0020		/* wide bus device */
/* UNUSED		0x00C0 */
#define CFSTART		0x0100		/* send start unit SCSI command */
#define CFINCBIOS	0x0200		/* include in BIOS scan */
#define CFRNFOUND	0x0400		/* report even if not found */
/* UNUSED		0xF800 */
  unsigned short device_flags[16];	/* words 0-15 */

/*
 * BIOS Control Bits
 */
#define CFSUPREM	0x0001		/* support all removeable drives */
#define CFSUPREMB	0x0002		/* support removeable drives for boot only */
#define CFBIOSEN	0x0004		/* BIOS enabled */
/* UNUSED		0x0008 */
#define CFSM2DRV	0x0010		/* support more than two drives */
/* UNUSED		0x0060 */
#define CFEXTEND	0x0080		/* extended translation enabled */
/* UNUSED		0xFF00 */
  unsigned short bios_control;		/* word 16 */

/*
 * Host Adapter Control Bits
 */
/* UNUSED               0x0001 */
#define CFULTRAEN       0x0002          /* Ultra SCSI speed enable (Ultra cards) */
#define CFSTERM         0x0004          /* SCSI low byte termination (non-wide cards) */
#define CFWSTERM        0x0008          /* SCSI high byte termination (wide card) */
#define CFSPARITY	0x0010		/* SCSI parity */
/* UNUSED		0x0020 */
#define CFRESETB	0x0040		/* reset SCSI bus at IC initialization */
/* UNUSED		0xFF80 */
  unsigned short adapter_control;	/* word 17 */

/*
 * Bus Release, Host Adapter ID
 */
#define CFSCSIID	0x000F		/* host adapter SCSI ID */
/* UNUSED		0x00F0 */
#define CFBRTIME	0xFF00		/* bus release time */
  unsigned short brtime_id;		/* word 18 */

/*
 * Maximum targets
 */
#define CFMAXTARG	0x00FF	/* maximum targets */
/* UNUSED		0xFF00 */
  unsigned short max_targets;		/* word 19 */

  unsigned short res_1[11];		/* words 20-30 */
  unsigned short checksum;		/* word 31 */

};


#define AIC7XXX_DEBUG

/*
 * Pause the sequencer and wait for it to actually stop - this
 * is important since the sequencer can disable pausing for critical
 * sections.
 */
#define PAUSE_SEQUENCER(p) \
  outb(p->pause, HCNTRL(p->base));			\
  while ((inb(HCNTRL(p->base)) & PAUSE) == 0)		\
    ;							\

/*
 * Unpause the sequencer. Unremarkable, yet done often enough to
 * warrant an easy way to do it.
 */
#define UNPAUSE_SEQUENCER(p) \
  outb(p->unpause, HCNTRL(p->base))

/*
 * Restart the sequencer program from address zero
 */
#define RESTART_SEQUENCER(p) \
  do {							\
    outb(SEQRESET | FASTMODE, SEQCTL(p->base));		\
  } while (inb(SEQADDR0(p->base)) != 0 &&		\
	   inb(SEQADDR1(p->base)) != 0);		\
  UNPAUSE_SEQUENCER(p);

/*
 * If an error occurs during a data transfer phase, run the comand
 * to completion - it's easier that way - making a note of the error
 * condition in this location. This then will modify a DID_OK status
 * into an appropriate error for the higher-level SCSI code.
 */
#define aic7xxx_error(cmd)	((cmd)->SCp.Status)

/*
 * Keep track of the targets returned status.
 */
#define aic7xxx_status(cmd)	((cmd)->SCp.sent_command)

/*
 * The position of the SCSI commands scb within the scb array.
 */
#define aic7xxx_position(cmd)	((cmd)->SCp.have_data_in)

/*
 * Since the sequencer code DMAs the scatter-gather structures
 * directly from memory, we use this macro to assert that the
 * kernel structure hasn't changed.
 */
#define SG_STRUCT_CHECK(sg) \
  ((char *) &(sg).address - (char *) &(sg) != 0 ||  \
   (char *) &(sg).length  - (char *) &(sg) != 8 ||  \
   sizeof((sg).address) != 4 ||                   \
   sizeof((sg).length)  != 4 ||                   \
   sizeof(sg)           != 12)

/*
 * "Static" structures. Note that these are NOT initialized
 * to zero inside the kernel - we have to initialize them all
 * explicitly.
 *
 * We support multiple adapter cards per interrupt, but keep a
 * linked list of Scsi_Host structures for each IRQ.  On an interrupt,
 * use the IRQ as an index into aic7xxx_boards[] to locate the card
 * information.
 */
static struct Scsi_Host *aic7xxx_boards[MAXIRQ + 1];

/*
 * When we detect and register the card, it is possible to
 * have the card raise a spurious interrupt.  Because we need
 * to support multiple cards, we cannot tell which card caused
 * the spurious interrupt.  And, we might not even have added
 * the card info to the linked list at the time the spurious
 * interrupt gets raised.  This variable is suppose to keep track
 * of when we are registering a card and how many spurious
 * interrupts we have encountered.
 *
 *   0 - do not allow spurious interrupts.
 *   1 - allow 1 spurious interrupt
 *   2 - have 1 spurious interrupt, do not allow any more.
 *
 * I've made it an integer instead of a boolean in case we
 * want to allow more than one spurious interrupt for debugging
 * purposes.  Otherwise, it could just go from true to false to
 * true (or something like that).
 *
 * When the driver detects the cards, we'll set the count to 1
 * for each card detection and registration.  After the registration
 * of a card completes, we'll set the count back to 0.  So far, it
 * seems to be enough to allow a spurious interrupt only during
 * card registration; if a spurious interrupt is going to occur,
 * this is where it happens.
 *
 * We should be able to find a way to avoid getting the spurious
 * interrupt.  But until we do, we have to keep this ugly code.
 */
static int aic7xxx_spurious_count;

/*
 * The driver keeps up to four scb structures per card in memory. Only the
 * first 26 bytes of the structure are valid for the hardware, the rest used
 * for driver level bookeeping.
 */
#define SCB_DOWNLOAD_SIZE	26	/* amount to actually download */
#define SCB_UPLOAD_SIZE		26	/* amount to actually upload */

struct aic7xxx_scb {
/* ------------    Begin hardware supported fields    ---------------- */
/*1 */  unsigned char control;
#define	SCB_NEEDWDTR 0x80			/* Initiate Wide Negotiation */
#define SCB_DISCENB  0x40			/* Disconnection Enable */
#define	SCB_TE	     0x20			/* Tag enable */
#define SCB_NEEDSDTR 0x10			/* Initiate Sync Negotiation */
#define	SCB_NEEDDMA  0x08			/* Refresh SCB from host ram */
#define	SCB_DIS	     0x04
#define	SCB_TAG_TYPE 0x03
#define		SIMPLE_QUEUE	0x00
#define		HEAD_QUEUE	0x01
#define		OR_QUEUE	0x02
/*              ILLEGAL      0x03 */
/*2 */  unsigned char target_channel_lun;       /* 4/1/3 bits */
/*3 */  unsigned char SG_segment_count;
/*7 */  unsigned char SG_list_pointer[4] __attribute__ ((packed));
/*11*/  unsigned char SCSI_cmd_pointer[4] __attribute__ ((packed));
/*12*/  unsigned char SCSI_cmd_length;
/*14*/  unsigned char RESERVED[2];              /* must be zero */
/*15*/  unsigned char target_status;
/*18*/  unsigned char residual_data_count[3];
/*19*/  unsigned char residual_SG_segment_count;
/*23*/  unsigned char data_pointer[4] __attribute__ ((packed));
/*26*/  unsigned char data_count[3];
/*30*/  unsigned char host_scb[4] __attribute__ ((packed));
/*31*/  u_char next_waiting;            /* Used to thread SCBs awaiting selection. */
#define SCB_LIST_NULL 0xFF              /* SCB list equivelent to NULL */
#if 0
	/*
	 *  No real point in transferring this to the
	 *  SCB registers.
	 */
	unsigned char RESERVED[1];
#endif

	/*-----------------end of hardware supported fields----------------*/
	struct aic7xxx_scb *next;	/* next ptr when in free list */
	Scsi_Cmnd          *cmd;	/* Scsi_Cmnd for this scb */
#define SCB_FREE               0x00
#define SCB_ACTIVE             0x01
#define SCB_ABORTED            0x02
#define SCB_DEVICE_RESET       0x04
#define SCB_IMMED              0x08
#define SCB_SENSE              0x10
	int                 state;          /* current state of scb */
	unsigned int        position;       /* Position in scb array */
	struct scatterlist  sg;
	struct scatterlist  sense_sg;
	unsigned char       sense_cmd[6];   /* Allocate 6 characters for sense command */
#define TIMER_ENABLED		0x01
#define TIMER_EXPIRED		0x02
#define TIMED_CMD_DONE		0x04
        volatile unsigned char timer_status;
#ifndef AIC7XXX_POLL
	struct wait_queue   *waiting;       /* wait queue for device reset command */
        struct wait_queue   waitq;          /* waiting points to this */
	struct timer_list   timer;          /* timeout for device reset command */
#endif
};

typedef void (*timeout_fn)(unsigned long);

static struct {
  unsigned char errno;
  const char *errmesg;
} hard_error[] = {
  { ILLHADDR,  "Illegal Host Access" },
  { ILLSADDR,  "Illegal Sequencer Address referrenced" },
  { ILLOPCODE, "Illegal Opcode in sequencer program" },
  { PARERR,    "Sequencer Ram Parity Error" }
};

static unsigned char
generic_sense[] = { REQUEST_SENSE, 0, 0, 0, 255, 0 };

/*
 * The maximum number of SCBs we could have for ANY type
 * of card. DON'T FORGET TO CHANGE THE SCB MASK IN THE
 * SEQUENCER CODE IF THIS IS MODIFIED!
 */
#define AIC7XXX_MAXSCB	255

/*
 * Define a structure used for each host adapter, only one per IRQ.
 */
struct aic7xxx_host {
  int                      base;             /* card base address */
  int                      maxscb;           /* hardware SCBs */
  int                      numscb;           /* current number of scbs */
  int                      extended;         /* extended xlate? */
  aha_type                 type;             /* card type */
  int                      ultra_enabled;    /* Ultra SCSI speed enabled */
  int                      chan_num;         /* for 3940/3985, channel number */
  aha_bus_type             bus_type;         /* normal/twin/wide bus */
  unsigned char            a_scanned;        /* 0 not scanned, 1 scanned */
  unsigned char            b_scanned;        /* 0 not scanned, 1 scanned */
  unsigned int             isr_count;        /* Interrupt count */
  volatile unsigned char   unpause;          /* unpause value for HCNTRL */
  volatile unsigned char   pause;            /* pause value for HCNTRL */
  volatile unsigned short  needsdtr_copy;    /* default config */
  volatile unsigned short  needsdtr;
  volatile unsigned short  sdtr_pending;
  volatile unsigned short  needwdtr_copy;    /* default config */
  volatile unsigned short  needwdtr;
  volatile unsigned short  wdtr_pending;
  volatile unsigned short  discenable;       /* Targets allowed to disconnect */
  struct seeprom_config    seeprom;
  int                      have_seeprom;
  struct Scsi_Host        *next;             /* allow for multiple IRQs */
  struct aic7xxx_scb       scb_array[AIC7XXX_MAXSCB];  /* active commands */
  struct aic7xxx_scb      *free_scb;         /* list of free SCBs */
#ifdef AIC7XXX_PROC_STATS
  /*
   * Statistics Kept:
   *
   * Total Xfers (count for each command that has a data xfer),
   * broken down further by reads && writes.
   *
   * Binned sizes, writes && reads:
   *    < 512, 512, 1-2K, 2-4K, 4-8K, 8-16K, 16-32K, 32-64K, 64K-128K, > 128K
   *
   * Total amounts read/written above 512 bytes (amts under ignored)
   */
  struct aic7xxx_xferstats {
    long xfers;                              /* total xfer count */
    long w_total;                            /* total writes */
    long w_total512;                         /* 512 byte blocks written */
    long w_bins[10];                         /* binned write */
    long r_total;                            /* total reads */
    long r_total512;                         /* 512 byte blocks read */
    long r_bins[10];                         /* binned reads */
  } stats[2][16][8];                         /* channel, target, lun */
#endif /* AIC7XXX_PROC_STATS */
};

struct aic7xxx_host_config {
  int              irq;        /* IRQ number */
  int              base;       /* I/O base */
  int              maxscb;     /* hardware SCBs */
  int              unpause;    /* unpause value for HCNTRL */
  int              pause;      /* pause value for HCNTRL */
  int              scsi_id;    /* host SCSI ID */
  int              scsi_id_b;  /* host SCSI ID B channel for twin cards */
  int              extended;   /* extended xlate? */
  int              busrtime;   /* bus release time */
  int              walk_scbs;  /* external SCB RAM detected; walk the scb array */
  aha_type         type;       /* card type */
  int              ultra_enabled;    /* Ultra SCSI speed enabled */
  int              chan_num;   /* for 3940/3985, channel number */
  aha_bus_type     bus_type;   /* normal/twin/wide bus */
  aha_status_type  parity;     /* bus parity enabled/disabled */
  aha_status_type  low_term;   /* bus termination low byte */
  aha_status_type  high_term;  /* bus termination high byte (wide cards only) */
};

/*
 * Valid SCSIRATE values. (p. 3-17)
 * Provides a mapping of tranfer periods in ns to the proper value to
 * stick in the scsiscfr reg to use that transfer rate.
 */
static struct {
  short period;
  /* Rates in Ultra mode have bit 8 of sxfr set */
#define		ULTRA_SXFR 0x100
  short rate;
  const char *english;
} aic7xxx_syncrates[] = {
  {  50,  0x100,  "20.0" },
  {  62,  0x110,  "16.0" },
  {  75,  0x120,  "13.4" },
  { 100,  0x140,  "10.0" },
  { 100,  0x000,  "10.0" },
  { 125,  0x010,  "8.0"  },
  { 150,  0x020,  "6.67" },
  { 175,  0x030,  "5.7"  },
  { 200,  0x040,  "5.0"  },
  { 225,  0x050,  "4.4"  },
  { 250,  0x060,  "4.0"  },
  { 275,  0x070,  "3.6"  }
};

static int num_aic7xxx_syncrates =
    sizeof(aic7xxx_syncrates) / sizeof(aic7xxx_syncrates[0]);

static int number_of_3940s = 0;

#ifdef AIC7XXX_DEBUG

static void
debug(const char *fmt, ...)
{
  va_list ap;
  char buf[256];

  va_start(ap, fmt);
  vsprintf(buf, fmt, ap);
  printk(buf);
  va_end(ap);
}

static void
debug_config(struct aic7xxx_host_config *p)
{
  int host_conf, scsi_conf;
  unsigned char brelease;
  unsigned char dfthresh;

  static int DFT[] = { 0, 50, 75, 100 };
  static int SST[] = { 256, 128, 64, 32 };
  static const char *BUSW[] = { "", "-TWIN", "-WIDE" };

  host_conf = inb(HA_HOSTCONF(p->base));
  scsi_conf = inb(HA_SCSICONF(p->base));

  /*
   * The 7870 gets the bus release time and data FIFO threshold
   * from the serial EEPROM (stored in the config structure) and
   * scsi_conf register respectively.  The 7770 gets the bus
   * release time and data FIFO threshold from the scsi_conf and
   * host_conf registers respectively.
   */
  if ((p->type == AIC_274x) || (p->type == AIC_284x))
  {
    dfthresh = (host_conf >> 6);
  }
  else
  {
    dfthresh = (scsi_conf >> 6);
  }

  brelease = p->busrtime;
  if (brelease == 0)
  {
    brelease = 2;
  }

  switch (p->type)
  {
    case AIC_274x:
      printk("AIC7770%s AT EISA SLOT %d:\n", BUSW[p->bus_type], p->base >> 12);
      break;

    case AIC_284x:
      printk("AIC7770%s AT VLB SLOT %d:\n", BUSW[p->bus_type], p->base >> 12);
      break;

    case AIC_7870:
      printk("AIC7870/7871%s (PCI-bus):\n", BUSW[p->bus_type]);
      break;

    case AIC_7850:
      printk("AIC7850%s (PCI-bus):\n", BUSW[p->bus_type]);
      break;

    case AIC_7872:
      printk("AIC7872%s (PCI-bus):\n", BUSW[p->bus_type]);
      break;

    case AIC_7880:
      printk("AIC7880/7881%s (PCI-bus):\n", BUSW[p->bus_type]);
      break;

    case AIC_7882:
      printk("AIC7882%s (PCI-bus):\n", BUSW[p->bus_type]);
      break;

    default:
      panic("aic7xxx debug_config: internal error\n");
  }

  printk("    irq %d\n"
	 "    bus release time %d bclks\n"
	 "    data fifo threshold %d%%\n",
	 p->irq,
	 brelease,
	 DFT[dfthresh]);

  printk("    SCSI CHANNEL A:\n"
	 "        scsi id %d\n"
	 "        scsi selection timeout %d ms\n"
	 "        scsi bus reset at power-on %sabled\n",
	 scsi_conf & 0x07,
	 SST[(scsi_conf >> 3) & 0x03],
	 (scsi_conf & 0x40) ? "en" : "dis");

  if (((p->type == AIC_274x) || (p->type == AIC_284x)) && p->parity == AIC_UNKNOWN)
  {
    /*
     * Set the parity for 7770 based cards.
     */
    p->parity = (scsi_conf & 0x20) ? AIC_ENABLED : AIC_DISABLED;
  }
  if (p->parity != AIC_UNKNOWN)
  {
    printk("        scsi bus parity %sabled\n",
	   (p->parity == AIC_ENABLED) ? "en" : "dis");
  }

  if (p->type == AIC_274x)
  {
    p->low_term = (scsi_conf & 0x80) ? AIC_ENABLED : AIC_DISABLED;
  }
  if (p->low_term != AIC_UNKNOWN)
  {
    printk("        scsi bus termination (low byte) %sabled\n",
	  (p->low_term == AIC_ENABLED) ? "en" : "dis");
  }
  if ((p->bus_type == AIC_WIDE) && (p->high_term != AIC_UNKNOWN))
  {
    printk("        scsi bus termination (high byte) %sabled\n",
	  (p->high_term == AIC_ENABLED) ? "en" : "dis");
  }
}

#if 0
static void
debug_scb(struct aic7xxx_scb *scb)
{
  printk("control 0x%x, tcl 0x%x, sg_count %d, sg_ptr 0x%x, cmdp 0x%x, cmdlen %d\n",
         scb->control, scb->target_channel_lun, scb->SG_segment_count,
         (scb->SG_list_pointer[3] << 24) | (scb->SG_list_pointer[2] << 16) |
         (scb->SG_list_pointer[1] << 8) | scb->SG_list_pointer[0],
         (scb->SCSI_cmd_pointer[3] << 24) | (scb->SCSI_cmd_pointer[2] << 16) |
         (scb->SCSI_cmd_pointer[1] << 8) | scb->SCSI_cmd_pointer[0],
         scb->SCSI_cmd_length);
  printk("reserved 0x%x, target status 0x%x, resid SG count %d, resid data count %d\n",
         (scb->RESERVED[1] << 8) | scb->RESERVED[0], scb->target_status,
         scb->residual_SG_segment_count, (scb->residual_data_count[2] << 16) |
         (scb->residual_data_count[1] << 8) | scb->residual_data_count[0]);
  printk("data ptr 0x%x, data count %d, host scb 0x%x, next waiting %d\n",
         (scb->data_pointer[3] << 24) | (scb->data_pointer[2] << 16) |
         (scb->data_pointer[1] << 8) | scb->data_pointer[0],
         (scb->data_count[2] << 16) | (scb->data_count[1] << 8) | scb->data_count[0],
         (unsigned int) scb->host_scb, scb->next_waiting);
  printk("next ptr 0x%lx, Scsi Cmnd 0x%lx, state 0x%x, position %d\n",
         (unsigned long) scb->next, (unsigned long) scb->cmd, scb->state,
         scb->position);
}
#endif

#else
#  define debug(fmt, args...)
#  define debug_config(x)
#  define debug_scb(x)
#endif AIC7XXX_DEBUG

/*
 * XXX - these options apply unilaterally to _all_ 274x/284x/294x
 *       cards in the system. This should be fixed, but then,
 *       does anyone really have more than one in a machine?
 */
static unsigned int aic7xxx_extended = 0;    /* extended translation on? */
static unsigned int aic7xxx_no_reset = 0;    /* no resetting of SCSI bus */

/*+F*************************************************************************
 * Function:
 *   aic7xxx_setup
 *
 * Description:
 *   Handle Linux boot parameters. This routine allows for assigning a value
 *   to a parameter with a ':' between the parameter and the value.
 *   ie. aic7xxx=unpause:0x0A,extended
 *-F*************************************************************************/
void
aic7xxx_setup(char *s, int *dummy)
{
  int   i, n;
  char *p;

  static struct {
    const char *name;
    unsigned int *flag;
  } options[] = {
    { "extended",    &aic7xxx_extended },
    { "no_reset",    &aic7xxx_no_reset },
    { NULL,          NULL }
  };

  for (p = strtok(s, ","); p; p = strtok(NULL, ","))
  {
    for (i = 0; options[i].name; i++)
    {
      n = strlen(options[i].name);
      if (!strncmp(options[i].name, p, n))
      {
        if (p[n] == ':')
        {
          *(options[i].flag) = simple_strtoul(p + n + 1, NULL, 0);
        }
        else
        {
          *(options[i].flag) = !0;
        }
      }
    }
  }
}

/*+F*************************************************************************
 * Function:
 *   aic7xxx_loadseq
 *
 * Description:
 *   Load the sequencer code into the controller memory.
 *-F*************************************************************************/
static void
aic7xxx_loadseq(int base)
{
  static unsigned char seqprog[] = {
    /*
     * Each sequencer instruction is 29 bits
     * long (fill in the excess with zeroes)
     * and has to be loaded from least -> most
     * significant byte, so this table has the
     * byte ordering reversed.
     */
#   include "aic7xxx_seq.h"
  };

  /*
   * When the AIC-7770 is paused (as on chip reset), the
   * sequencer address can be altered and a sequencer
   * program can be loaded by writing it, byte by byte, to
   * the sequencer RAM port - the Adaptec documentation
   * recommends using REP OUTSB to do this, hence the inline
   * assembly. Since the address autoincrements as we load
   * the program, reset it back to zero afterward. Disable
   * sequencer RAM parity error detection while loading, and
   * make sure the LOADRAM bit is enabled for loading.
   */
  outb(PERRORDIS | SEQRESET | LOADRAM, SEQCTL(base));

  asm volatile("cld\n\t"
	       "rep\n\t"
	       "outsb"
	       : /* no output */
	       :"S" (seqprog), "c" (sizeof(seqprog)), "d" (SEQRAM(base))
	       :"si", "cx", "dx");

  /*
   * WARNING!  This is a magic sequence!  After extensive
   * experimentation, it seems that you MUST turn off the
   * LOADRAM bit before you play with SEQADDR again, else
   * you will end up with parity errors being flagged on
   * your sequencer program. (You would also think that
   * turning off LOADRAM and setting SEQRESET to reset the
   * address to zero would work, but you need to do it twice
   * for it to take effect on the address. Timing problem?)
   */
  do {
    /*
     * Actually, reset it until
     * the address shows up as
     * zero just to be safe..
     */
    outb(SEQRESET | FASTMODE, SEQCTL(base));
  } while ((inb(SEQADDR0(base)) != 0) && (inb(SEQADDR1(base)) != 0));
}

/*+F*************************************************************************
 * Function:
 *   aic7xxx_delay
 *
 * Description:
 *   Delay for specified amount of time.
 *-F*************************************************************************/
static void
aic7xxx_delay(int seconds)
{
  unsigned long i;

  i = jiffies + (seconds * HZ);  /* compute time to stop */

  while (jiffies < i)
  {
    ;  /* Do nothing! */
  }
}

#ifdef AIC7XXX_POLL
/*+F*************************************************************************
 * Function:
 *   aic7xxx_poll_scb
 *
 * Description:
 *   Function to poll for command completion when in aborting an SCB.
 *-F*************************************************************************/
static void
aic7xxx_poll_scb(struct aic7xxx_host *p,
                 struct aic7xxx_scb  *scb,
                 unsigned long       timeout_ticks)
{
  unsigned long timer_expiration = jiffies + timeout_ticks;

  while ((jiffies < timer_expiration) && !(scb->timer_status & TIMED_CMD_DONE))
  {
    udelay(1000);  /* delay for 1 msec. */
  }
}

#else
/*+F*************************************************************************
 * Function:
 *   aic7xxx_scb_timeout
 *
 * Description:
 *   Called when a SCB reset command times out.  The input is actually
 *   a pointer to the SCB.
 *-F*************************************************************************/
static void
aic7xxx_scb_timeout(unsigned long data)
{
  struct aic7xxx_scb *scb = (struct aic7xxx_scb *) data;

  scb->timer_status |= TIMER_EXPIRED;
  wake_up(&(scb->waiting));
}

/*+F*************************************************************************
 * Function:
 *   aic7xxx_scb_untimeout
 *
 * Description:
 *   This function clears the timeout and wakes up a waiting SCB.
 *-F*************************************************************************/
static void
aic7xxx_scb_untimeout(struct aic7xxx_scb *scb)
{
  if (scb->timer_status & TIMER_ENABLED)
  {
    scb->timer_status = TIMED_CMD_DONE;
    wake_up(&(scb->waiting));
  }
}
#endif

/*+F*************************************************************************
 * Function:
 *   aic7xxx_scb_tsleep
 *
 * Description:
 *   Emulates a BSD tsleep where a process can sleep for a specified
 *   amount of time, but may be awakened before that.  Linux provides
 *   a sleep_on, wake_up, add_timer, and del_timer which can be used to
 *   emulate tsleep, but there's not enough information available on
 *   how to use them.  For now, we'll just poll for SCB completion.
 *
 *   The parameter ticks is the number of clock ticks
 *   to wait before a timeout.  A 0 is returned if the scb does not
 *   timeout, 1 is returned for a timeout.
 *-F*************************************************************************/
static int
aic7xxx_scb_tsleep(struct aic7xxx_host *p,
                   struct aic7xxx_scb  *scb,
                   unsigned long        ticks)
{
  scb->timer_status = TIMER_ENABLED;
#ifdef AIC7XXX_POLL
  UNPAUSE_SEQUENCER(p);
  aic7xxx_poll_scb(p, scb, ticks);
#else
  scb->waiting = &(scb->waitq);
  scb->timer.expires = jiffies + ticks;
  scb->timer.data = (unsigned long) scb;
  scb->timer.function = (timeout_fn) aic7xxx_scb_timeout;
  add_timer(&scb->timer);
  UNPAUSE_SEQUENCER(p);
  sleep_on(&(scb->waiting));
  del_timer(&scb->timer);
#endif
  if (!(scb->timer_status & TIMED_CMD_DONE))
  {
    scb->timer_status = 0x0;
    return (1);
  }
  else
  {
    scb->timer_status = 0x0;
    return (0);
  }
}

/*+F*************************************************************************
 * Function:
 *   rcs_version
 *
 * Description:
 *   Return a string containing just the RCS version number from either
 *   an Id or Revison RCS clause.
 *-F*************************************************************************/
const char *
rcs_version(const char *version_info)
{
  static char buf[10];
  char *bp, *ep;

  bp = NULL;
  strcpy(buf, "????");
  if (!strncmp(version_info, "$Id: ", 5))
  {
    if ((bp = strchr(version_info, ' ')) != NULL)
    {
      bp++;
      if ((bp = strchr(bp, ' ')) != NULL)
      {
	bp++;
      }
    }
  }
  else
  {
    if (!strncmp(version_info, "$Revision: ", 11))
    {
      if ((bp = strchr(version_info, ' ')) != NULL)
      {
	bp++;
      }
    }
  }

  if (bp != NULL)
  {
    if ((ep = strchr(bp, ' ')) != NULL)
    {
      register int len = ep - bp;

      strncpy(buf, bp, len);
      buf[len] = '\0';
    }
  }

  return buf;
}

/*+F*************************************************************************
 * Function:
 *   aic7xxx_info
 *
 * Description:
 *   Return a string describing the driver.
 *-F*************************************************************************/
const char *
aic7xxx_info(struct Scsi_Host *notused)
{
  static char buffer[128];

  strcpy(buffer, "Adaptec AHA274x/284x/294x (EISA/VLB/PCI-Fast SCSI) ");
  strcat(buffer, rcs_version(AIC7XXX_C_VERSION));
  strcat(buffer, "/");
  strcat(buffer, rcs_version(AIC7XXX_H_VERSION));
  strcat(buffer, "/");
  strcat(buffer, rcs_version(AIC7XXX_SEQ_VER));

  return buffer;
}

/*+F*************************************************************************
 * Function:
 *   aic7xxx_length
 *
 * Description:
 *   How much data should be transferred for this SCSI command? Stop
 *   at segment sg_last if it's a scatter-gather command so we can
 *   compute underflow easily.
 *-F*************************************************************************/
static unsigned
aic7xxx_length(Scsi_Cmnd *cmd, int sg_last)
{
  int i, segments;
  unsigned length;
  struct scatterlist *sg;

  segments = cmd->use_sg - sg_last;
  sg = (struct scatterlist *) cmd->buffer;

  if (cmd->use_sg)
  {
    for (i = length = 0; (i < cmd->use_sg) && (i < segments); i++)
    {
      length += sg[i].length;
    }
  }
  else
  {
    length = cmd->request_bufflen;
  }

  return (length);
}

/*+F*************************************************************************
 * Function:
 *   aic7xxx_scsirate
 *
 * Description:
 *   Look up the valid period to SCSIRATE conversion in our table
 *-F*************************************************************************/
static void
aic7xxx_scsirate(struct aic7xxx_host *p, unsigned char *scsirate,
                 unsigned char period, unsigned char offset,
                 int target, char channel)
{
  int i;

  for (i = 0; i < num_aic7xxx_syncrates; i++)
  {
    if ((aic7xxx_syncrates[i].period - period) >= 0)
    {
      /*
       * Watch out for Ultra speeds when ultra is not enabled and
       * vice-versa.
       */
      if (p->ultra_enabled)
      {
        if (! (aic7xxx_syncrates[i].rate & ULTRA_SXFR))
        {
          printk ("aic7xxx: target %d, channel %c, requests %sMB/s transfers, "
                  "but adapter in Ultra mode can only sync at 10MB/s or "
                  "above.\n", target, channel, aic7xxx_syncrates[i].english);
          break;  /* Use asynchronous transfers. */
        }
      }
      else
      {
        /*
         * Check for an Ultra device trying to negotiate an Ultra rate
         * on an adapter with Ultra mode disabled.
         */
        if (aic7xxx_syncrates[i].rate & ULTRA_SXFR)
        {
          /*
           * This should only happen if the driver is the first to negotiate
           * and chooses a high rate.  We'll just move down the table until
           * we hit a non Ultra speed.
           */
           continue;
        }
      }
      *scsirate = (aic7xxx_syncrates[i].rate) | (offset & 0x0F);
      printk("aic7xxx: target %d, channel %c, now synchronous at %sMB/s, "
             "offset = 0x%x\n",
	     target, channel, aic7xxx_syncrates[i].english, offset);
      return;
    }
  }

  /*
   * Default to asyncronous transfer
   */
  *scsirate = 0;
  printk("aic7xxx: target %d, channel %c, using asynchronous transfers\n",
         target, channel);
}

/*+F*************************************************************************
 * Function:
 *   aic7xxx_putscb
 *
 * Description:
 *   Transfer a SCB to the controller.
 *-F*************************************************************************/
static void
aic7xxx_putscb(int base, struct aic7xxx_scb *scb)
{
  /*
   * All we need to do, is to output the position
   * of the SCB in the SCBARRAY to the QINFIFO
   * of the host adapter.
   */
  outb(scb->position, QINFIFO(base));
}

/*+F*************************************************************************
 * Function:
 *   aic7xxx_putscb_dma
 *
 * Description:
 *   DMA a SCB to the controller.
 *-F*************************************************************************/
static void
aic7xxx_putscb_dma(int base, struct aic7xxx_scb *scb)
{
  /*
   * By turning on the SCB auto increment, any reference
   * to the SCB I/O space postincrements the SCB address
   * we're looking at. So turn this on and dump the relevant
   * portion of the SCB to the card.
   */
  outb(SCBAUTO, SCBCNT(base));

  asm volatile("cld\n\t"
	       "rep\n\t"
	       "outsb"
	       : /* no output */
	       :"S" (scb), "c" (31), "d" (SCBARRAY(base))
	       :"si", "cx", "dx");

  outb(0, SCBCNT(base));
}

/*+F*************************************************************************
 * Function:
 *   aic7xxx_getscb
 *
 * Description:
 *   Get a SCB from the controller.
 *-F*************************************************************************/
static void
aic7xxx_getscb(int base, struct aic7xxx_scb *scb)
{
  /*
   * This is almost identical to aic7xxx_putscb().
   */
  outb(SCBAUTO, SCBCNT(base));

  asm volatile("cld\n\t"
	       "rep\n\t"
	       "insb"
	       : /* no output */
	       :"D" (scb), "c" (SCB_UPLOAD_SIZE), "d" (SCBARRAY(base))
	       :"di", "cx", "dx");

  outb(0, SCBCNT(base));
}

/*+F*************************************************************************
 * Function:
 *   aic7xxx_match_scb
 *
 * Description:
 *   Checks to see if an scb matches the target/channel as specified.
 *   If target is ALL_TARGETS (-1), then we're looking for any device
 *   on the specified channel; this happens when a channel is going
 *   to be reset and all devices on that channel must be aborted.
 *-F*************************************************************************/
static int
aic7xxx_match_scb(struct aic7xxx_scb *scb, int target, char channel)
{
  int targ = (scb->target_channel_lun >> 4) & 0x0F;
  char chan = (scb->target_channel_lun & SELBUSB) ? 'B' : 'A';

  if (target == ALL_TARGETS)
  {
    return (chan == channel);
  }
  else
  {
    return ((chan == channel) && (targ == target));
  }
}

/*+F*************************************************************************
 * Function:
 *   aic7xxx_unbusy_target
 *
 * Description:
 *   Set the specified target inactive.
 *-F*************************************************************************/
static void
aic7xxx_unbusy_target(unsigned char target, char channel, int base)
{
  unsigned char active;
  unsigned long active_port = HA_ACTIVE0(base);

  if ((target > 0x07) || (channel == 'B'))
  {
    /*
     * targets on the Second channel or above id 7 store info in byte two
     * of HA_ACTIVE
     */
    active_port++;
  }
  active = inb(active_port);
  active &= ~(0x01 << (target & 0x07));
  outb(active_port, active);
}

/*+F*************************************************************************
 * Function:
 *   aic7xxx_done
 *
 * Description:
 *   Calls the higher level scsi done function and frees the scb.
 *-F*************************************************************************/
static void
aic7xxx_done(struct aic7xxx_host *p, struct aic7xxx_scb *scb)
{
  long flags;
  Scsi_Cmnd *cmd = scb->cmd;

  if (scb->timer_status & TIMER_ENABLED)
  {
#ifdef AIC7XXX_POLL
    scb->timer_status |= TIMED_CMD_DONE;
#else
    aic7xxx_scb_untimeout(scb);
#endif
  }
  else
  {
    /*
     * This is a critical section, since we don't want the
     * queue routine mucking with the host data.
     */
    save_flags(flags);
    cli();

    /*
     * Process the command after marking the scb as free
     * and adding it to the free list.
     */
    scb->state = SCB_FREE;
    scb->next = p->free_scb;
    p->free_scb = &(p->scb_array[scb->position]);
    scb->cmd = NULL;

    restore_flags(flags);

    cmd->scsi_done(cmd);
  }
}

/*+F*************************************************************************
 * Function:
 *   aic7xxx_add_waiting_scb
 *
 * Description:
 *   Add this SCB to the "waiting for selection" list.
 *-F*************************************************************************/
static void
aic7xxx_add_waiting_scb(u_long              base,
                        struct aic7xxx_scb *scb,
                        insert_type         where)
{
  unsigned char head, tail;
  unsigned char curscb;

  curscb = inb(SCBPTR(base));
  head = inb(WAITING_SCBH(base));
  tail = inb(WAITING_SCBT(base));
  if (head == SCB_LIST_NULL)
  {
    /*
     * List was empty
     */
    head = scb->position;
    tail = SCB_LIST_NULL;
  }
  else
  {
    if (where == LIST_HEAD)
    {
      outb(scb->position, SCBPTR(base));
      outb(head, SCBARRAY(base) + 30);
      head = scb->position;
    }
    else
    {
      if (tail == SCB_LIST_NULL)
      {
        /*
         * List had one element
         */
        tail = scb->position;
        outb(head, SCBPTR(base));
        outb(tail, SCBARRAY(base) + 30);
      }
      else
      {
        if (where == LIST_SECOND)
        {
          unsigned char third_scb;

          outb(head, SCBPTR(base));
          third_scb = inb(SCBARRAY(base) + 30);
          outb(scb->position, SCBARRAY(base) + 30);
          outb(scb->position, SCBPTR(base));
          outb(third_scb, SCBARRAY(base) + 30);
        }
        else
        {
          outb(tail, SCBPTR(base));
          tail = scb->position;
          outb(tail, SCBARRAY(base) + 30);
        }
      }
    }
  }
  outb(head, WAITING_SCBH(base));
  outb(tail, WAITING_SCBT(base));
  outb(curscb, SCBPTR(base));
}

/*+F*************************************************************************
 * Function:
 *   aic7xxx_abort_waiting_scb
 *
 * Description:
 *   Manipulate the waiting for selection list and return the
 *   scb that follows the one that we remove.
 *-F*************************************************************************/
static unsigned char
aic7xxx_abort_waiting_scb(struct aic7xxx_host *p, struct aic7xxx_scb *scb,
                          unsigned char prev, unsigned char timedout_scb)
{
  unsigned char curscb, next;
  int target = (scb->target_channel_lun >> 4) & 0x0F;
  char channel = (scb->target_channel_lun & SELBUSB) ? 'B' : 'A';
  int base = p->base;

  /*
   * Select the SCB we want to abort and
   * pull the next pointer out of it.
   */
  curscb = inb(SCBPTR(base));
  outb(scb->position, SCBPTR(base));
  next = inb(SCBARRAY(base) + 30);

  /*
   * Clear the necessary fields
   */
  outb(SCB_NEEDDMA, SCBARRAY(base));
  outb(SCB_LIST_NULL, SCBARRAY(base) + 30);
  aic7xxx_unbusy_target(target, channel, base);

  /*
   * Update the waiting list
   */
  if (prev == SCB_LIST_NULL)
  {
    /*
     * First in the list
     */
    outb(next, WAITING_SCBH(base));
  }
  else
  {
    /*
     * Select the scb that pointed to us and update its next pointer.
     */
    outb(prev, SCBPTR(base));
    outb(next, SCBARRAY(base) + 30);
  }
  /*
   * Update the tale pointer
   */
  if (inb(WAITING_SCBT(base)) == scb->position)
  {
    outb(prev, WAITING_SCBT(base));
  }

  /*
   * Point us back at the original scb position
   * and inform the SCSI system that the command
   * has been aborted.
   */
  outb(curscb, SCBPTR(base));
  scb->state |= SCB_ABORTED;
  scb->cmd->result = (DID_RESET << 16);
  aic7xxx_done(p, scb);

  return (next);
}

/*+F*************************************************************************
 * Function:
 *   aic7xxx_reset_device
 *
 * Description:
 *   The device at the given target/channel has been reset.  Abort
 *   all active and queued scbs for that target/channel.
 *-F*************************************************************************/
static int
aic7xxx_reset_device(struct aic7xxx_host *p, int target, char channel,
                     unsigned char timedout_scb)
{
  int base = p->base;
  struct aic7xxx_scb *scb;
  unsigned char active_scb;
  int i = 0;
  int found = 0;

  /*
   * Restore this when we're done
   */
  active_scb = inb(SCBPTR(base));

  /*
   * Search the QINFIFO.
   */
  {
    int saved_queue[AIC7XXX_MAXSCB];
    int queued = inb(QINCNT(base));

    for (i = 0; i < (queued - found); i++)
    {
      saved_queue[i] = inb(QINFIFO(base));
      scb = &(p->scb_array[saved_queue[i]]);
      if (aic7xxx_match_scb(scb, target, channel))
      {
        /*
         * We found an scb that needs to be aborted.
         */
        scb->state |= SCB_ABORTED;
        scb->cmd->result = (DID_RESET << 16);
        aic7xxx_done(p, scb);
        outb(scb->position, SCBPTR(base));
        outb(SCB_NEEDDMA, SCBARRAY(base));
        i--;
        found++;
      }
    }
    /*
     * Now put the saved scbs back.
     */
    for (queued = 0; queued < i; queued++)
    {
      outb(saved_queue[queued], QINFIFO(base));
    }
  }

  /*
   * Search waiting for selection list.
   */
  {
    unsigned char next, prev;

    next = inb(WAITING_SCBH(base));  /* Start at head of list. */
    prev = SCB_LIST_NULL;

    while (next != SCB_LIST_NULL)
    {
      scb = &(p->scb_array[next]);
      /*
       * Select the SCB.
       */
      if (aic7xxx_match_scb(scb, target, channel))
      {
        next = aic7xxx_abort_waiting_scb(p, scb, prev, timedout_scb);
        found++;
      }
      else
      {
        outb(scb->position, SCBPTR(base));
        prev = next;
        next = inb(SCBARRAY(base) + 30);
      }
    }
  }

  /*
   * Go through the entire SCB array now and look for
   * commands for this target that are active.  These
   * are other (most likely tagged) commands that
   * were disconnected when the reset occured.
   */
  for(i = 0; i < p->numscb; i++)
  {
    scb = &(p->scb_array[i]);
    if ((scb->state & SCB_ACTIVE) && aic7xxx_match_scb(scb, target, channel))
    {
      /*
       * Ensure the target is "free"
       */
      aic7xxx_unbusy_target(target, channel, base);
      outb(scb->position, SCBPTR(base));
      outb(SCB_NEEDDMA, SCBARRAY(base));
      scb->state |= SCB_ABORTED;
      scb->cmd->result = (DID_RESET << 16);
      aic7xxx_done(p, scb);
      found++;
    }
  }

  outb(active_scb, SCBPTR(base));
  return (found);
}

/*+F*************************************************************************
 * Function:
 *   aic7xxx_reset_current_bus
 *
 * Description:
 *   Reset the current SCSI bus.
 *-F*************************************************************************/
static void
aic7xxx_reset_current_bus(int base)
{
  outb(SCSIRSTO, SCSISEQ(base));
  udelay(1000);
  outb(0, SCSISEQ(base));
}

/*+F*************************************************************************
 * Function:
 *   aic7xxx_reset_channel
 *
 * Description:
 *   Reset the channel.
 *-F*************************************************************************/
static int
aic7xxx_reset_channel(struct aic7xxx_host *p, char channel,
                     unsigned char timedout_scb)
{
  int base = p->base;
  unsigned char sblkctl;
  char cur_channel;
  unsigned long offset, offset_max;
  int found;

  /*
   * Clean up all the state information for the
   * pending transactions on this bus.
   */
  found = aic7xxx_reset_device(p, ALL_TARGETS, channel, timedout_scb);

  if (channel == 'B')
  {
    p->needsdtr |= (p->needsdtr_copy & 0xFF00);
    p->sdtr_pending &= 0x00FF;
    outb(0, HA_ACTIVE1(base));
    offset = HA_TARG_SCRATCH(base) + 8;
    offset_max = HA_TARG_SCRATCH(base) + 16;
  }
  else
  {
    if (p->bus_type == AIC_WIDE)
    {
      p->needsdtr = p->needsdtr_copy;
      p->needwdtr = p->needwdtr_copy;
      p->sdtr_pending = 0;
      p->wdtr_pending = 0;
      outb(0, HA_ACTIVE0(base));
      outb(0, HA_ACTIVE1(base));
      offset = HA_TARG_SCRATCH(base);
      offset_max = HA_TARG_SCRATCH(base) + 16;
    }
    else
    {
      p->needsdtr |= (p->needsdtr_copy & 0x00FF);
      p->sdtr_pending &= 0xFF00;
      outb(0, HA_ACTIVE0(base));
      offset = HA_TARG_SCRATCH(base);
      offset_max = HA_TARG_SCRATCH(base) + 8;
    }
  }
  while (offset < offset_max)
  {
    /*
     * Revert to async/narrow transfers
     * until we renegotiate.
     */
    u_char targ_scratch;
    targ_scratch = inb(offset);
    targ_scratch &= SXFR;
    outb(targ_scratch, offset);
    offset++;
  }

  /*
   * Reset the bus and unpause/restart the controller
   */

  /*
   * Case 1: Command for another bus is active
   */
  sblkctl = inb(SBLKCTL(base));
  cur_channel = (sblkctl & SELBUSB) ? 'B' : 'A';
  if (cur_channel != channel)
  {
    /*
     * Stealthily reset the other bus without upsetting the current bus
     */
    outb(sblkctl ^ SELBUSB, SBLKCTL(base));
    aic7xxx_reset_current_bus(base);
    outb(sblkctl, SBLKCTL(base));

    UNPAUSE_SEQUENCER(p);
  }
  /*
   * Case 2: A command from this bus is active or we're idle
   */
  else
  {
    aic7xxx_reset_current_bus(base);
    RESTART_SEQUENCER(p);
  }

  return found;
}

/*+F*************************************************************************
 * Function:
 *   aic7xxx_isr
 *
 * Description:
 *   SCSI controller interrupt handler.
 *
 *   NOTE: Since we declared this using SA_INTERRUPT, interrupts should
 *         be disabled all through this function unless we say otherwise.
 *-F*************************************************************************/
static void
aic7xxx_isr(int irq, struct pt_regs * regs)
{
  int base, intstat;
  struct aic7xxx_host *p;
  struct aic7xxx_scb *scb;
  unsigned char ha_flags, transfer;
  unsigned char scsi_id, bus_width;
  unsigned char offset, rate, scratch, scratch_offset;
  unsigned char max_offset, rej_byte;
  unsigned short target_mask, active;
  char channel;
  void *addr;
  int actual;
  int scb_index;
  Scsi_Cmnd *cmd;

  p = (struct aic7xxx_host *) aic7xxx_boards[irq]->hostdata;

  /*
   * Search for the host with a pending interrupt.  If we can't find
   * one, then we've encountered a spurious interrupt.
   */
  while ((p != NULL) && !(inb(INTSTAT(p->base)) & INT_PEND))
  {
    if (p->next == NULL)
    {
      p = NULL;
    }
    else
    {
      p = (struct aic7xxx_host *) p->next->hostdata;
    }
  }

  if (p == NULL)
  {
    if (aic7xxx_spurious_count == 1)
    {
      aic7xxx_spurious_count = 2;
      printk("aic7xxx_isr: Encountered spurious interrupt.\n");
      return;
    }
    else
    {
      /*
       * The best we can do is to set p back to head of list and process
       * the erroneous interrupt - most likely a BRKADRINT.
       */
      p = (struct aic7xxx_host *) aic7xxx_boards[irq]->hostdata;
    }
  }

  /*
   * Keep track of interrupts for /proc/scsi
   */
  p->isr_count++;

  if ((p->a_scanned == 0) && (p->isr_count == 1))
  {
    /*
     * We must only have one card at this IRQ and it must have been
     * added to the board data before the spurious interrupt occurred.
     * It is sufficient that we check isr_count and not the spurious
     * interrupt count.
     */
    printk("aic7xxx_isr: Encountered spurious interrupt.\n");
    return;
  }

  base = p->base;
  /*
   * Handle all the interrupt sources - especially for SCSI
   * interrupts, we won't get a second chance at them.
   */
  intstat = inb(INTSTAT(base));

  if (intstat & BRKADRINT)
  {
    int i;
    unsigned char errno = inb(ERROR(base));

    printk("aic7xxx_isr: brkadrint (0x%x):\n", errno);
    for (i = 0; i < NUMBER(hard_error); i++)
    {
      if (errno & hard_error[i].errno)
      {
	printk("  %s\n", hard_error[i].errmesg);
      }
    }

    panic("aic7xxx_isr: brkadrint, error = 0x%x, seqaddr = 0x%x\n",
	  inb(ERROR(base)), (inb(SEQADDR1(base)) << 8) | inb(SEQADDR0(base)));
  }

  if (intstat & SEQINT)
  {
    /*
     * Although the sequencer is paused immediately on
     * a SEQINT, an interrupt for a SCSIINT condition will
     * unpaused the sequencer before this point.
     */
    PAUSE_SEQUENCER(p);

    scsi_id = (inb(SCSIID(base)) >> 4) & 0x0F;
    scratch_offset = scsi_id;
    channel = 'A';
    if (inb(SBLKCTL(base)) & SELBUSB)
    {
      channel = 'B';
      scratch_offset += 8;
    }
    target_mask = (0x01 << scratch_offset);

    switch (intstat & SEQINT_MASK)
    {
      case BAD_PHASE:
	panic("aic7xxx_isr: unknown scsi bus phase\n");
	break;

      case SEND_REJECT:
        rej_byte = inb(HA_REJBYTE(base));
        if (rej_byte != 0x20)
        {
          debug("aic7xxx_isr warning: issuing message reject, 1st byte 0x%x\n",
                rej_byte);
        }
        else
        {
          scb_index = inb(SCBPTR(base));
          scb = &(p->scb_array[scb_index]);
          printk("aic7xxx_isr warning: Tagged message rejected for target %d,"
                 " channel %c.\n", scsi_id, channel);
          scb->cmd->device->tagged_supported = 0;
          scb->cmd->device->tagged_queue = 0;
        }
	break;

      case NO_IDENT:
	panic("aic7xxx_isr: Target %d, channel %c, did not send an IDENTIFY "
	      "message.  SAVED_TCL = 0x%x\n",
              scsi_id, channel, inb(SAVED_TCL(base)));
	break;

      case NO_MATCH:
	printk("aic7xxx_isr: No active SCB for reconnecting target %d, "
	      "channel %c - issuing ABORT\n", scsi_id, channel);
        printk("SAVED_TCL = 0x%x\n", inb(SAVED_TCL(base)));
        aic7xxx_unbusy_target(scsi_id, channel, base);
        outb(SCB_NEEDDMA, SCBARRAY(base));

	outb(CLRSELTIMEO, CLRSINT1(base));
	RESTART_SEQUENCER(p);
	break;

      case MSG_SDTR:
	/*
	 * Help the sequencer to translate the negotiated
	 * transfer rate. Transfer is 1/4 the period
	 * in ns as is returned by the sync negotiation
	 * message. So, we must multiply by four.
	 */
	transfer = (inb(HA_ARG_1(base)) << 2);
	offset = inb(ACCUM(base));
	scratch = inb(HA_TARG_SCRATCH(base) + scratch_offset);
	/*
	 * The maximum offset for a wide device is 0x08; for a
	 * 8-bit bus device the maximum offset is 0x0F.
	 */
	if (scratch & WIDEXFER)
	{
	  max_offset = 0x08;
	}
	else
	{
	  max_offset = 0x0F;
	}
	aic7xxx_scsirate(p, &rate, transfer, MIN(offset, max_offset), scsi_id, channel);
	/*
	 * Preserve the wide transfer flag.
	 */
	scratch = rate | (scratch & WIDEXFER);
	outb(scratch, HA_TARG_SCRATCH(base) + scratch_offset);
	outb(scratch, SCSIRATE(base));
	if ((scratch & 0x0F) == 0)
	{ /*
	   * The requested rate was so low that asynchronous transfers
	   * are faster (not to mention the controller won't support
	   * them), so we issue a reject to ensure we go to asynchronous
	   * transfers.
	   */
	   outb(SEND_REJ, HA_RETURN_1(base));
	}
	else
	{
	  /*
	   * See if we initiated Sync Negotiation
	   */
	  if (p->sdtr_pending & target_mask)
	  {
	    /*
	     * Don't send an SDTR back to the target.
	     */
	    outb(0, HA_RETURN_1(base));
	  }
	  else
	  {
	    /*
	     * Send our own SDTR in reply.
	     */
	    printk("aic7xxx_isr: Sending SDTR!!\n");
	    outb(SEND_SDTR, HA_RETURN_1(base));
	  }
	}
	/*
	 * Clear the flags.
	 */
	p->needsdtr &= ~target_mask;
	p->sdtr_pending &= ~target_mask;
#if 0
  scb_index = inb(SCBPTR(base));
  scb = &(p->scb_array[scb_index]);
  debug_scb(scb);
#endif

	break;

      case MSG_WDTR:
      {
	bus_width = inb(ACCUM(base));
	printk("aic7xxx_isr: Received MSG_WDTR, scsi_id %d, channel %c "
	       "needwdtr = 0x%x\n", scsi_id, channel, p->needwdtr);
	scratch = inb(HA_TARG_SCRATCH(base) + scratch_offset);

	if (p->wdtr_pending & target_mask)
	{
	  /*
	   * Don't send an WDTR back to the target, since we asked first.
	   */
	  outb(0, HA_RETURN_1(base));
	  switch (bus_width)
	  {
	    case BUS_8_BIT:
	      scratch &= 0x7F;
	      break;

	    case BUS_16_BIT:
	      printk("aic7xxx_isr: target %d, channel %c, using 16 bit transfers\n",
		     scsi_id, channel);
	      scratch |= 0x80;
	      break;
	  }
	}
	else
	{
	  /*
	   * Send our own WDTR in reply.
	   */
	  printk("aic7xxx_isr: Will send WDTR!!\n");
	  switch (bus_width)
	  {
	    case BUS_8_BIT:
	      scratch &= 0x7F;
	      break;

	    case BUS_32_BIT:
	      /*
               * Negotiate 16 bits.
               */
	      bus_width = BUS_16_BIT;
	      /* Yes, we mean to fall thru here. */

	    case BUS_16_BIT:
	      printk("aic7xxx_isr: target %d, channel %c, using 16 bit transfers\n",
		     scsi_id, channel);
	      scratch |= 0x80;
	      break;
	  }
	  outb(bus_width | SEND_WDTR, HA_RETURN_1(base));
	}
	p->needwdtr &= ~target_mask;
	p->wdtr_pending &= ~target_mask;
	outb(scratch, HA_TARG_SCRATCH(base) + scratch_offset);
	outb(scratch, SCSIRATE(base));
	break;
      }

      case MSG_REJECT:
      {
	/*
	 * What we care about here is if we had an
	 * outstanding SDTR or WDTR message for this
	 * target. If we did, this is a signal that
	 * the target is refusing negotiation.
	 */

	scratch = inb(HA_TARG_SCRATCH(base) + scratch_offset);

	if (p->wdtr_pending & target_mask)
	{
	  /*
	   * note 8bit xfers and clear flag
	   */
	  scratch &= 0x7F;
	  p->needwdtr &= ~target_mask;
	  p->wdtr_pending &= ~target_mask;
	  outb(scratch, HA_TARG_SCRATCH(base) + scratch_offset);
	  printk("aic7xxx: target %d, channel %c, refusing WIDE negotiation. "
                 "Using 8 bit transfers\n", scsi_id, channel);
	}
	else
	{
	  if (p->sdtr_pending & target_mask)
	  {
	    /*
	     * note asynch xfers and clear flag
	     */
	    scratch &= 0xF0;
	    p->needsdtr &= ~target_mask;
	    p->sdtr_pending &= ~target_mask;
	    outb(scratch, HA_TARG_SCRATCH(base) + scratch_offset);
	    printk("aic7xxx: target %d, channel %c, refusing syncronous negotiation. "
                   "Using asyncronous transfers\n", scsi_id, channel);
	  }
	  /*
	   * Otherwise, we ignore it.
	   */
	}
	outb(scratch, HA_TARG_SCRATCH(base) + scratch_offset);
	outb(scratch, SCSIRATE(base));
	break;
      }

      case BAD_STATUS:
	scb_index = inb(SCBPTR(base));
	scb = &(p->scb_array[scb_index]);
	outb(0, HA_RETURN_1(base));   /* CHECK_CONDITION may change this */
	if ((scb->state != SCB_ACTIVE) || (scb->cmd == NULL))
	{
	  printk("aic7xxx_isr: referenced scb not valid "
		 "during seqint 0x%x scb(%d) state(%x), cmd(%x)\n",
		 intstat, scb_index, scb->state, (unsigned int) scb->cmd);
	}
	else
	{
	  cmd = scb->cmd;
	  aic7xxx_getscb(base, scb);
	  aic7xxx_status(cmd) = scb->target_status;

	  cmd->result |= scb->target_status;

	  switch (status_byte(scb->target_status))
	  {
	    case GOOD:
              printk("aic7xxx_isr: Interrupted for status of 0???\n");
	      break;

	    case CHECK_CONDITION:
	      if ((aic7xxx_error(cmd) == 0) && !(cmd->flags & WAS_SENSE))
	      {
                unsigned char tcl;
                unsigned char control;
		void         *req_buf;

                tcl = scb->target_channel_lun;
		/*
                 * Send a sense command to the requesting target.
                 */
		cmd->flags |= WAS_SENSE;
		memcpy((void *) scb->sense_cmd, (void *) generic_sense,
		       sizeof(generic_sense));

		scb->sense_cmd[1] = (cmd->lun << 5);
		scb->sense_cmd[4] = sizeof(cmd->sense_buffer);

		scb->sense_sg.address = (char *) &cmd->sense_buffer;
		scb->sense_sg.length = sizeof(cmd->sense_buffer);
		req_buf = &scb->sense_sg;
		cmd->cmd_len = COMMAND_SIZE(cmd->cmnd[0]);
                control = scb->control;
		memset(scb, 0, SCB_DOWNLOAD_SIZE);
                scb->control = control & SCB_DISCENB;
		scb->target_channel_lun = tcl;
		addr = scb->sense_cmd;
		scb->SCSI_cmd_length = COMMAND_SIZE(scb->sense_cmd[0]);
		memcpy(scb->SCSI_cmd_pointer, &addr,
		       sizeof(scb->SCSI_cmd_pointer));
		scb->SG_segment_count = 1;
		memcpy(scb->SG_list_pointer, &req_buf,
			sizeof(scb->SG_list_pointer));
                scb->data_count[0] = scb->sense_sg.length & 0xFF;
                scb->data_count[1] = (scb->sense_sg.length >> 8) & 0xFF;
                scb->data_count[2] = (scb->sense_sg.length >> 16) & 0xFF;
		memcpy(scb->data_pointer, &(scb->sense_sg.address), 4);

		outb(SCBAUTO, SCBCNT(base));
		asm volatile("cld\n\t"
			     "rep\n\t"
			     "outsb"
			     : /* no output */
			     :"S" (scb), "c" (SCB_DOWNLOAD_SIZE), "d" (SCBARRAY(base))
			     :"si", "cx", "dx");
		outb(0, SCBCNT(base));
		outb(SCB_LIST_NULL, (SCBARRAY(base) + 30));
                /*
                 * Ensure that the target is "BUSY" so we don't get overlapping
                 * commands if we happen to be doing tagged I/O.
                 */
                active = inb(HA_ACTIVE0(base)) | (inb(HA_ACTIVE1(base)) << 8);
                active |= target_mask;
                outb(active & 0xFF, HA_ACTIVE0(base));
                outb((active >> 8) & 0xFF, HA_ACTIVE1(base));

                aic7xxx_add_waiting_scb(base, scb, LIST_HEAD);
		outb(SEND_SENSE, HA_RETURN_1(base));
	      }  /* first time sense, no errors */
	      else
	      {
		/*
		 * Indicate that we asked for sense, have the sequencer do
		 * a normal command complete, and have the scsi driver handle
		 * this condition.
		 */
		cmd->flags |= ASKED_FOR_SENSE;
	      }
	      break;

	    case BUSY:
	      printk("aic7xxx_isr: Target busy\n");
	      if (!aic7xxx_error(cmd))
	      {
		aic7xxx_error(cmd) = DID_BUS_BUSY;
	      }
	      break;

	    case QUEUE_FULL:
	      printk("aic7xxx_isr: Queue full\n");
	      if (!aic7xxx_error(cmd))
	      {
		aic7xxx_error(cmd) = DID_RETRY_COMMAND;
	      }
	      break;

	    default:
	      printk("aic7xxx_isr: Unexpected target status 0x%x\n",
		     scb->target_status);
	      if (!aic7xxx_error(cmd))
	      {
		aic7xxx_error(cmd) = DID_RETRY_COMMAND;
	      }
	      break;
	  }  /* end switch */
	}  /* end else of */
	break;

      case RESIDUAL:
	scb_index = inb(SCBPTR(base));
	scb = &(p->scb_array[scb_index]);
	if ((scb->state != SCB_ACTIVE) || (scb->cmd == NULL))
	{
	  printk("aic7xxx_isr: referenced scb not valid "
		 "during seqint 0x%x scb(%d) state(%x), cmd(%x)\n",
		 intstat, scb_index, scb->state, (unsigned int) scb->cmd);
	}
	else
	{
	  cmd = scb->cmd;
	  /*
	   *  Don't destroy valid residual information with
	   *  residual coming from a check sense operation.
	   */
	  if (!(cmd->flags & WAS_SENSE))
	  {
	    /*
	     *  We had an underflow. At this time, there's only
	     *  one other driver that bothers to check for this,
	     *  and cmd->underflow seems to be set rather half-
	     *  heartedly in the higher-level SCSI code.
	     */
	    actual = aic7xxx_length(cmd, scb->residual_SG_segment_count);

	    actual -= ((inb(SCBARRAY(base + 17)) << 16) |
		       (inb(SCBARRAY(base + 16)) <<  8) |
		       inb(SCBARRAY(base + 15)));

	    if (actual < cmd->underflow)
	    {
	      printk("aic7xxx: target %d underflow - "
		     "wanted (at least) %u, got %u, count=%d\n",
		     cmd->target, cmd->underflow, actual, inb(SCBARRAY(base + 18)));
	      aic7xxx_error(cmd) = DID_RETRY_COMMAND;
	      aic7xxx_status(cmd) = scb->target_status;
	    }
	  }
	}
	break;

      case ABORT_TAG:
	scb_index = inb(SCBPTR(base));
	scb = &(p->scb_array[scb_index]);
	if ((scb->state != SCB_ACTIVE) || (scb->cmd == NULL))
	{
	  printk("aic7xxx_isr: referenced scb not valid "
		 "during seqint 0x%x scb(%d) state(%x), cmd(%x)\n",
		 intstat, scb_index, scb->state, (unsigned int) scb->cmd);
	}
	else
	{
	  cmd = scb->cmd;
	  /*
	   * We didn't recieve a valid tag back from the target
	   * on a reconnect.
	   */
	  printk("aic7xxx_isr: invalid tag recieved on channel %c "
		 "target %d, lun %d -- sending ABORT_TAG\n",
		  channel, scsi_id, cmd->lun & 0x07);

	  cmd->result = (DID_RETRY_COMMAND << 16);
          aic7xxx_done(p, scb);
	}
	break;

      case AWAITING_MSG:
	scb_index = inb(SCBPTR(base));
	scb = &(p->scb_array[scb_index]);
	if ((scb->state != SCB_ACTIVE) || (scb->cmd == NULL))
	{
	  printk("aic7xxx_isr: referenced scb not valid "
		 "during seqint 0x%x scb(%d) state(%x), cmd(%x)\n",
		 intstat, scb_index, scb->state, (unsigned int) scb->cmd);
	}
	else
	{
	  /*
	   * This SCB had a zero length command, informing the sequencer
	   * that we wanted to send a special message to this target.
	   * We only do this for BUS_DEVICE_RESET messages currently.
	   */
	   if (scb->state & SCB_DEVICE_RESET)
	   {
	     outb(MSG_BUS_DEVICE_RESET, HA_MSG_START(base));
	     outb(1, HA_MSG_LEN(base));
	   }
	   else
	   {
	     panic("aic7xxx_isr: AWAITING_SCB for an SCB that does "
		   "not have a waiting message");
	   }
	}
	break;

      case IMMEDDONE:
        scb_index = inb(SCBPTR(base));
	scb = &(p->scb_array[scb_index]);
        if (scb->state & SCB_DEVICE_RESET)
        {
          int found;

          /*
           * Go back to async/narrow transfers and renogiate.
           */
          aic7xxx_unbusy_target(scsi_id, channel, base);
          p->needsdtr |= (p->needsdtr_copy & target_mask);
          p->needwdtr |= (p->needwdtr_copy & target_mask);
          p->sdtr_pending &= ~target_mask;
          p->wdtr_pending &= ~target_mask;
          scratch = inb(HA_TARG_SCRATCH(base) + scratch_offset);
          scratch &= SXFR;
          outb(scratch, HA_TARG_SCRATCH(base));
          found = aic7xxx_reset_device(p, (int) scsi_id, channel, SCB_LIST_NULL);
        }
        else
        {
          panic("aic7xxx_isr: Immediate complete for unknown operation.\n");
        }
        break;

      default:               /* unknown */
	debug("aic7xxx_isr: seqint, intstat = 0x%x, scsisigi = 0x%x\n",
	      intstat, inb(SCSISIGI(base)));
	break;
    }
    outb(CLRSEQINT, CLRINT(base));
    UNPAUSE_SEQUENCER(p);
  }

  if (intstat & SCSIINT)
  {
    int status = inb(SSTAT1(base));

    scb_index = inb(SCBPTR(base));
    scb = &(p->scb_array[scb_index]);
    if ((scb->state != SCB_ACTIVE) || (scb->cmd == NULL))
    {
      printk("aic7xxx_isr: no command for scb (scsiint)\n");
      /*
       * Turn off the interrupt and set status
       * to zero, so that it falls through the
       * reset of the SCSIINT code.
       */
      outb(status, CLRSINT1(base));
      UNPAUSE_SEQUENCER(p);
      outb(CLRSCSIINT, CLRINT(base));
      status = 0;
      scb = NULL;
    }
    else
    {
      cmd = scb->cmd;

      /*
       * Only the SCSI Status 1 register has information
       * about exceptional conditions that we'd have a
       * SCSIINT about; anything in SSTAT0 will be handled
       * by the sequencer. Note that there can be multiple
       * bits set.
       */
      if (status & SELTO)
      {
	unsigned char target_mask = (1 << (cmd->target & 0x07));
	unsigned char waiting;

	/*
	 * Hardware selection timer has expired. Turn
	 * off SCSI selection sequence.
	 */
	outb(ENRSELI, SCSISEQ(base));
	cmd->result = (DID_TIME_OUT << 16);
	/*
	 * Clear an pending messages for the timed out
	 * target and mark the target as free.
	 */
	ha_flags = inb(HA_FLAGS(base));
	outb(ha_flags & ~ACTIVE_MSG, HA_FLAGS(base));

	if (scb->target_channel_lun & 0x88)
	{
	  active = inb(HA_ACTIVE1(base));
	  active = active & ~(target_mask);
	  outb(active, HA_ACTIVE1(base));
	}
	else
	{
	  active = inb(HA_ACTIVE0(base));
	  active &= ~(target_mask);
	  outb(active, HA_ACTIVE0(base));
	}

	outb(SCB_NEEDDMA, SCBARRAY(base));

	/*
	 * Shut off the offending interrupt sources, reset
	 * the sequencer address to zero and unpause it,
	 * then call the high-level SCSI completion routine.
	 *
	 * WARNING!  This is a magic sequence!  After many
	 * hours of guesswork, turning off the SCSI interrupts
	 * in CLRSINT? does NOT clear the SCSIINT bit in
	 * INTSTAT. By writing to the (undocumented, unused
	 * according to the AIC-7770 manual) third bit of
	 * CLRINT, you can clear INTSTAT. But, if you do it
	 * while the sequencer is paused, you get a BRKADRINT
	 * with an Illegal Host Address status, so the
	 * sequencer has to be restarted first.
	 */
	outb(CLRSELTIMEO, CLRSINT1(base));

	outb(CLRSCSIINT, CLRINT(base));

	/*
         * Shift the waiting for selection queue forward
         */
	waiting = inb(WAITING_SCBH(base));
	outb(waiting, SCBPTR(base));
	waiting = inb(SCBARRAY(base) + 30);
	outb(waiting, WAITING_SCBH(base));

	RESTART_SEQUENCER(p);
        aic7xxx_done(p, scb);
#if 0
  printk("aic7xxx_isr: SELTO scb(%d) state(%x), cmd(%x)\n",
	 scb->position, scb->state, (unsigned int) scb->cmd);
#endif
      }
      else
      {
	if (status & SCSIPERR)
	{
	  /*
	   * A parity error has occurred during a data
	   * transfer phase. Flag it and continue.
	   */
	  printk("aic7xxx: parity error on target %d, "
		 "channel %d, lun %d\n",
		 cmd->target,
		 cmd->channel & 0x01,
		 cmd->lun & 0x07);
	  aic7xxx_error(cmd) = DID_PARITY;

	  /*
	   * Clear interrupt and resume as above.
	   */
	  outb(CLRSCSIPERR, CLRSINT1(base));
	  UNPAUSE_SEQUENCER(p);

	  outb(CLRSCSIINT, CLRINT(base));
	  scb = NULL;
	}
	else
	{
	  if (!(status & BUSFREE))
	  {
	     /*
	      * We don't know what's going on. Turn off the
	      * interrupt source and try to continue.
	      */
	     printk("aic7xxx_isr: sstat1 = 0x%x\n", status);
	     outb(status, CLRSINT1(base));
	     UNPAUSE_SEQUENCER(p);
	     outb(CLRSCSIINT, CLRINT(base));
	     scb = NULL;
	  }
	}
      }
    }  /* else */
  }

  if (intstat & CMDCMPLT)
  {
    int complete;

    /*
     * The sequencer will continue running when it
     * issues this interrupt. There may be >1 commands
     * finished, so loop until we've processed them all.
     */
    do {
      complete = inb(QOUTFIFO(base));

      scb = &(p->scb_array[complete]);
      if ((scb->state != SCB_ACTIVE) || (scb->cmd == NULL))
      {
	printk("aic7xxx warning: "
	       "no command for scb %d (cmdcmplt)\n"
	       "QOUTCNT = %d, SCB state = 0x%x, CMD = 0x%x, pos = %d\n",
	       complete, inb(QOUTFIFO(base)),
	       scb->state, (unsigned int) scb->cmd, scb->position);
	outb(CLRCMDINT, CLRINT(base));
	continue;
      }
      cmd = scb->cmd;

      cmd->result = (aic7xxx_error(cmd) << 16) | aic7xxx_status(cmd);
      if ((cmd->flags & WAS_SENSE) && !(cmd->flags & ASKED_FOR_SENSE))
      {
        /*
         * Got sense information.
         */
	cmd->flags &= ASKED_FOR_SENSE;
      }
#if 0
      printk("aic7xxx_intr: (complete) state = %d, cmd = 0x%x, free = 0x%x\n",
	     scb->state, (unsigned int) scb->cmd, (unsigned int) p->free_scb);
#endif

      /*
       * Clear interrupt status before checking
       * the output queue again. This eliminates
       * a race condition whereby a command could
       * complete between the queue poll and the
       * interrupt clearing, so notification of the
       * command being complete never made it back
       * up to the kernel.
       */
      outb(CLRCMDINT, CLRINT(base));
      aic7xxx_done(p, scb);
#if 0
  if (scb != &p->scb_array[scb->position])
  {
    printk("aic7xxx_isr: (complete) address mismatch, pos %d\n", scb->position);
  }
  printk("aic7xxx_isr: (complete) state = %d, cmd = 0x%x, free = 0x%x\n",
	 scb->state, (unsigned int) scb->cmd, (unsigned int) p->free_scb);
#endif

#ifdef AIC7XXX_PROC_STATS
      /*
       * XXX: we should actually know how much actually transferred
       * XXX: for each command, but apparently that's too difficult.
       */
      actual = aic7xxx_length(cmd, 0);
      if (((cmd->flags & WAS_SENSE) == 0) && (actual > 0))
      {
        struct aic7xxx_xferstats *sp;
        long *ptr;
        int x;

        sp = &p->stats[cmd->channel & 0x01][cmd->target & 0x0F][cmd->lun & 0x07];
        sp->xfers++;

        if (cmd->request.cmd == WRITE)
        {
          sp->w_total++;
          sp->w_total512 += (actual >> 9);
          ptr = sp->w_bins;
        }
        else
        {
          sp->r_total++;
          sp->r_total512 += (actual >> 9);
          ptr = sp->r_bins;
        }
        for (x = 9; x <= 17; x++)
        {
          if (actual < (1 << x))
          {
            ptr[x - 9]++;
            break;
          }
        }
        if (x > 17)
        {
          ptr[x - 9]++;
        }
      }
#endif /* AIC7XXX_PROC_STATS */

    } while (inb(QOUTCNT(base)));
  }
}

/*+F*************************************************************************
 * Function:
 *   aic7xxx_probe
 *
 * Description:
 *   Probing for EISA boards: it looks like the first two bytes
 *   are a manufacturer code - three characters, five bits each:
 *
 *               BYTE 0   BYTE 1   BYTE 2   BYTE 3
 *              ?1111122 22233333 PPPPPPPP RRRRRRRR
 *
 *   The characters are baselined off ASCII '@', so add that value
 *   to each to get the real ASCII code for it. The next two bytes
 *   appear to be a product and revision number, probably vendor-
 *   specific. This is what is being searched for at each port,
 *   and what should probably correspond to the ID= field in the
 *   ECU's .cfg file for the card - if your card is not detected,
 *   make sure your signature is listed in the array.
 *
 *   The fourth byte's lowest bit seems to be an enabled/disabled
 *   flag (rest of the bits are reserved?).
 *-F*************************************************************************/
static aha_type
aic7xxx_probe(int slot, int base)
{
  int i;
  unsigned char buf[4];

  static struct {
    int n;
    unsigned char signature[sizeof(buf)];
    aha_type type;
  } AIC7xxx[] = {
    { 4, { 0x04, 0x90, 0x77, 0x71 }, AIC_274x },  /* host adapter 274x */
    { 4, { 0x04, 0x90, 0x77, 0x70 }, AIC_274x },  /* motherboard 274x  */
    { 4, { 0x04, 0x90, 0x77, 0x56 }, AIC_284x },  /* 284x, BIOS enabled */
    { 4, { 0x04, 0x90, 0x77, 0x57 }, AIC_284x }   /* 284x, BIOS disabled */
  };

  /*
   * The VL-bus cards need to be primed by
   * writing before a signature check.
   */
  for (i = 0; i < sizeof(buf); i++)
  {
    outb(0x80 + i, base);
    buf[i] = inb(base + i);
  }

  for (i = 0; i < NUMBER(AIC7xxx); i++)
  {
    /*
     * Signature match on enabled card?
     */
    if (!memcmp(buf, AIC7xxx[i].signature, AIC7xxx[i].n))
    {
      if (inb(base + 4) & 1)
      {
	return (AIC7xxx[i].type);
      }

      printk("aic7xxx disabled at slot %d, ignored\n", slot);
    }
  }

  return (AIC_NONE);
}

/*+F*************************************************************************
 * Function:
 *   read_2840_seeprom
 *
 * Description:
 *   Reads the 2840 serial EEPROM and returns 1 if successful and 0 if
 *   not successful.
 *
 *   See read_seeprom (for the 2940) for the instruction set of the 93C46
 *   chip.
 *
 *   The 2840 interface to the 93C46 serial EEPROM is through the
 *   STATUS_2840 and SEECTL_2840 registers.  The CS_2840, CK_2840, and
 *   DO_2840 bits of the SEECTL_2840 register are connected to the chip
 *   select, clock, and data out lines respectively of the serial EEPROM.
 *   The DI_2840 bit of the STATUS_2840 is connected to the data in line
 *   of the serial EEPROM.  The EEPROM_TF bit of STATUS_2840 register is
 *   useful in that it gives us an 800 nsec timer.  After a read from the
 *   SEECTL_2840 register the timing flag is cleard and goes high 800 nsec
 *   later.
 *
 *-F*************************************************************************/
static int
read_2840_seeprom(int base, struct seeprom_config *sc)
{
  int i = 0, k = 0;
  unsigned char temp;
  unsigned short checksum = 0;
  unsigned short *seeprom = (unsigned short *) sc;
  struct seeprom_cmd {
    unsigned char len;
    unsigned char bits[3];
  };
  struct seeprom_cmd seeprom_read = {3, {1, 1, 0}};

#define CLOCK_PULSE(p) \
  while ((inb(STATUS_2840(base)) & EEPROM_TF) == 0)	\
  {						\
    ;  /* Do nothing */				\
  }						\
  (void) inb(SEECTL_2840(base));

  /*
   * Read the first 32 registers of the seeprom.  For the 2840,
   * the 93C46 SEEPROM is a 1024-bit device with 64 16-bit registers
   * but only the first 32 are used by Adaptec BIOS.  The loop
   * will range from 0 to 31.
   */
  for (k = 0; k < (sizeof(*sc) / 2); k++)
  {
    /*
     * Send chip select for one clock cycle.
     */
    outb(CK_2840 | CS_2840, SEECTL_2840(base));
    CLOCK_PULSE(base);

    /*
     * Now we're ready to send the read command followed by the
     * address of the 16-bit register we want to read.
     */
    for (i = 0; i < seeprom_read.len; i++)
    {
      temp = CS_2840 | seeprom_read.bits[i];
      outb(temp, SEECTL_2840(base));
      CLOCK_PULSE(base);
      temp = temp ^ CK_2840;
      outb(temp, SEECTL_2840(base));
      CLOCK_PULSE(base);
    }
    /*
     * Send the 6 bit address (MSB first, LSB last).
     */
    for (i = 5; i >= 0; i--)
    {
      temp = k;
      temp = (temp >> i) & 1;  /* Mask out all but lower bit. */
      temp = CS_2840 | temp;
      outb(temp, SEECTL_2840(base));
      CLOCK_PULSE(base);
      temp = temp ^ CK_2840;
      outb(temp, SEECTL_2840(base));
      CLOCK_PULSE(base);
    }

    /*
     * Now read the 16 bit register.  An initial 0 precedes the
     * register contents which begins with bit 15 (MSB) and ends
     * with bit 0 (LSB).  The initial 0 will be shifted off the
     * top of our word as we let the loop run from 0 to 16.
     */
    for (i = 0; i <= 16; i++)
    {
      temp = CS_2840;
      outb(temp, SEECTL_2840(base));
      CLOCK_PULSE(base);
      temp = temp ^ CK_2840;
      seeprom[k] = (seeprom[k] << 1) | (inb(STATUS_2840(base)) & DI_2840);
      outb(temp, SEECTL_2840(base));
      CLOCK_PULSE(base);
    }
    /*
     * The serial EEPROM has a checksum in the last word.  Keep a
     * running checksum for all words read except for the last
     * word.  We'll verify the checksum after all words have been
     * read.
     */
    if (k < (sizeof(*sc) / 2) - 1)
    {
      checksum = checksum + seeprom[k];
    }

    /*
     * Reset the chip select for the next command cycle.
     */
    outb(0, SEECTL_2840(base));
    CLOCK_PULSE(base);
    outb(CK_2840, SEECTL_2840(base));
    CLOCK_PULSE(base);
    outb(0, SEECTL_2840(base));
    CLOCK_PULSE(base);
  }

#if 0
  printk("Computed checksum 0x%x, checksum read 0x%x\n", checksum, sc->checksum);
  printk("Serial EEPROM:");
  for (k = 0; k < (sizeof(*sc) / 2); k++)
  {
    if (((k % 8) == 0) && (k != 0))
    {
      printk("\n              ");
    }
    printk(" 0x%x", seeprom[k]);
  }
  printk("\n");
#endif

  if (checksum != sc->checksum)
  {
    printk("aic7xxx: SEEPROM checksum error, ignoring SEEPROM settings.\n");
    return (0);
  }

  return (1);
#undef CLOCK_PULSE
}

/*+F*************************************************************************
 * Function:
 *   read_seeprom
 *
 * Description:
 *   Reads the serial EEPROM and returns 1 if successful and 0 if
 *   not successful.
 *
 *   The instruction set of the 93C46 chip is as follows:
 *
 *               Start  OP
 *     Function   Bit  Code  Address    Data     Description
 *     -------------------------------------------------------------------
 *     READ        1    10   A5 - A0             Reads data stored in memory,
 *                                               starting at specified address
 *     EWEN        1    00   11XXXX              Write enable must preceed
 *                                               all programming modes
 *     ERASE       1    11   A5 - A0             Erase register A5A4A3A2A1A0
 *     WRITE       1    01   A5 - A0   D15 - D0  Writes register
 *     ERAL        1    00   10XXXX              Erase all registers
 *     WRAL        1    00   01XXXX    D15 - D0  Writes to all registers
 *     EWDS        1    00   00XXXX              Disables all programming
 *                                               instructions
 *     *Note: A value of X for address is a don't care condition.
 *
 *   The 93C46 has a four wire interface: clock, chip select, data in, and
 *   data out.  In order to perform one of the above functions, you need
 *   to enable the chip select for a clock period (typically a minimum of
 *   1 usec, with the clock high and low a minimum of 750 and 250 nsec
 *   respectively.  While the chip select remains high, you can clock in
 *   the instructions (above) starting with the start bit, followed by the
 *   OP code, Address, and Data (if needed).  For the READ instruction, the
 *   requested 16-bit register contents is read from the data out line but
 *   is preceded by an initial zero (leading 0, followed by 16-bits, MSB
 *   first).  The clock cycling from low to high initiates the next data
 *   bit to be sent from the chip.
 *
 *   The 7870 interface to the 93C46 serial EEPROM is through the SEECTL
 *   register.  After successful arbitration for the memory port, the
 *   SEECS bit of the SEECTL register is connected to the chip select.
 *   The SEECK, SEEDO, and SEEDI are connected to the clock, data out,
 *   and data in lines respectively.  The SEERDY bit of SEECTL is useful
 *   in that it gives us an 800 nsec timer.  After a write to the SEECTL
 *   register, the SEERDY goes high 800 nsec later.  The one exception
 *   to this is when we first request access to the memory port.  The
 *   SEERDY goes high to signify that access has been granted and, for
 *   this case, has no implied timing.
 *
 *-F*************************************************************************/
static int
read_seeprom(int base, int offset, struct seeprom_config *sc)
{
  int i = 0, k;
  unsigned long timeout;
  unsigned char temp;
  unsigned short checksum = 0;
  unsigned short *seeprom = (unsigned short *) sc;
  struct seeprom_cmd {
    unsigned char len;
    unsigned char bits[3];
  };
  struct seeprom_cmd seeprom_read = {3, {1, 1, 0}};

#define CLOCK_PULSE(p) \
  while ((inb(SEECTL(base)) & SEERDY) == 0)	\
  {						\
    ;  /* Do nothing */				\
  }

  /*
   * Request access of the memory port.  When access is
   * granted, SEERDY will go high.  We use a 1 second
   * timeout which should be near 1 second more than
   * is needed.  Reason: after the 7870 chip reset, there
   * should be no contention.
   */
  outb(SEEMS, SEECTL(base));
  timeout = jiffies + 100;  /* 1 second timeout */
  while ((jiffies < timeout) && ((inb(SEECTL(base)) & SEERDY) == 0))
  {
    ; /* Do nothing!  Wait for access to be granted.  */
  }
  if ((inb(SEECTL(base)) & SEERDY) == 0)
  {
    outb(0, SEECTL(base));
    return (0);
  }

  /*
   * Read the first 32 registers of the seeprom.  For the 7870,
   * the 93C46 SEEPROM is a 1024-bit device with 64 16-bit registers
   * but only the first 32 are used by Adaptec BIOS.  The loop
   * will range from 0 to 31.
   */
  for (k = 0; k < (sizeof(*sc) / 2); k++)
  {
    /*
     * Send chip select for one clock cycle.
     */
    outb(SEEMS | SEECK | SEECS, SEECTL(base));
    CLOCK_PULSE(base);

    /*
     * Now we're ready to send the read command followed by the
     * address of the 16-bit register we want to read.
     */
    for (i = 0; i < seeprom_read.len; i++)
    {
      temp = SEEMS | SEECS | (seeprom_read.bits[i] << 1);
      outb(temp, SEECTL(base));
      CLOCK_PULSE(base);
      temp = temp ^ SEECK;
      outb(temp, SEECTL(base));
      CLOCK_PULSE(base);
    }
    /*
     * Send the 6 bit address (MSB first, LSB last).
     */
    for (i = 5; i >= 0; i--)
    {
      temp = k + offset;
      temp = (temp >> i) & 1;  /* Mask out all but lower bit. */
      temp = SEEMS | SEECS | (temp << 1);
      outb(temp, SEECTL(base));
      CLOCK_PULSE(base);
      temp = temp ^ SEECK;
      outb(temp, SEECTL(base));
      CLOCK_PULSE(base);
    }

    /*
     * Now read the 16 bit register.  An initial 0 precedes the
     * register contents which begins with bit 15 (MSB) and ends
     * with bit 0 (LSB).  The initial 0 will be shifted off the
     * top of our word as we let the loop run from 0 to 16.
     */
    for (i = 0; i <= 16; i++)
    {
      temp = SEEMS | SEECS;
      outb(temp, SEECTL(base));
      CLOCK_PULSE(base);
      temp = temp ^ SEECK;
      seeprom[k] = (seeprom[k] << 1) | (inb(SEECTL(base)) & SEEDI);
      outb(temp, SEECTL(base));
      CLOCK_PULSE(base);
    }

    /*
     * The serial EEPROM has a checksum in the last word.  Keep a
     * running checksum for all words read except for the last
     * word.  We'll verify the checksum after all words have been
     * read.
     */
    if (k < (sizeof(*sc) / 2) - 1)
    {
      checksum = checksum + seeprom[k];
    }

    /*
     * Reset the chip select for the next command cycle.
     */
    outb(SEEMS, SEECTL(base));
    CLOCK_PULSE(base);
    outb(SEEMS | SEECK, SEECTL(base));
    CLOCK_PULSE(base);
    outb(SEEMS, SEECTL(base));
    CLOCK_PULSE(base);
  }

  /*
   * Release access to the memory port and the serial EEPROM.
   */
  outb(0, SEECTL(base));

#if 0
  printk("Computed checksum 0x%x, checksum read 0x%x\n", checksum, sc->checksum);
  printk("Serial EEPROM:");
  for (k = 0; k < (sizeof(*sc) / 2); k++)
  {
    if (((k % 8) == 0) && (k != 0))
    {
      printk("\n              ");
    }
    printk(" 0x%x", seeprom[k]);
  }
  printk("\n");
#endif

  if (checksum != sc->checksum)
  {
    printk("aic7xxx: SEEPROM checksum error, ignoring SEEPROM settings.\n");
    return (0);
  }

  return (1);
#undef CLOCK_PULSE
}

/*+F*************************************************************************
 * Function:
 *   detect_maxscb
 *
 * Description:
 *   Return the maximum number of SCB's allowed for a given controller.
 *-F*************************************************************************/
static int
detect_maxscb(aha_type type, int base, int walk_scbs)
{
  unsigned char sblkctl_reg, scb_byte;
  int maxscb = 0, i;

  switch (type)
  {
    case AIC_274x:
    case AIC_284x:
      /*
       * Check for Rev C or E boards. Rev E boards can supposedly have
       * more than 4 SCBs, while the Rev C boards are limited to 4 SCBs.
       * Until we know how to access more than 4 SCBs for the Rev E chips,
       * we limit them, along with the Rev C chips, to 4 SCBs.
       *
       * The Rev E boards have a read/write autoflush bit in the
       * SBLKCTL registor, while in the Rev C boards it is read only.
       */
      sblkctl_reg = inb(SBLKCTL(base)) ^ AUTOFLUSHDIS;
      outb(sblkctl_reg, SBLKCTL(base));
      if (inb(SBLKCTL(base)) == sblkctl_reg)
      {
        /*
         * We detected a Rev E board.
         */
	printk("aic7770: Rev E and subsequent; using 4 SCB's\n");
	outb(sblkctl_reg ^ AUTOFLUSHDIS, SBLKCTL(base));
	maxscb = 4;
      }
      else
      {
	printk("aic7770: Rev C and previous; using 4 SCB's\n");
	maxscb = 4;
      }
      break;

    case AIC_7850:
      maxscb = 3;
      break;

    case AIC_7870:
    case AIC_7880:
      maxscb = 16;
      break;

    case AIC_7872:
    case AIC_7882:
      /*
       * Is suppose to have 255 SCBs, but we'll walk the SCBs
       * looking for more if external RAM is detected.
       */
      maxscb = 16;
      break;

    case AIC_NONE:
      /*
       * This should never happen... But just in case.
       */
      break;
  }
  if (walk_scbs)
  {
    /*
     * This adapter has external SCB memory.
     * Walk the SCBs to determine how many there are.
     */
    i = 0;
    while (i < AIC7XXX_MAXSCB)
    {
      outb(i, SCBPTR(base));
      scb_byte = ~(inb(SCBARRAY(base)));  /* complement the byte */
      outb(scb_byte, SCBARRAY(base));     /* write it back out */
      if (inb(SCBARRAY(base)) != scb_byte)
      {
        break;
      }
      i++;
    }
    maxscb = i;		
  }

  return (maxscb);
}

/*+F*************************************************************************
 * Function:
 *   aic7xxx_register
 *
 * Description:
 *   Register a Adaptec aic7xxx chip SCSI controller with the kernel.
 *-F*************************************************************************/
static int
aic7xxx_register(Scsi_Host_Template *template,
                 struct aic7xxx_host_config *config)
{
  static const char * board_name[] = {"", "274x", "284x", "7870", "7850",
                                      "7872", "7881"};
  int i;
  unsigned char sblkctl;
  int max_targets;
  int found = 1, base;
  int bios_disabled = 0;
  unsigned char target_settings;
  unsigned char scsi_conf, host_conf;
  int have_seeprom = 0;
  struct Scsi_Host *host;
  struct aic7xxx_host *p;
  struct seeprom_config sc;

  base = config->base;

  /*
   * Lock out other contenders for our i/o space.
   */
  request_region(MINREG(base), MAXREG(base) - MINREG(base), "aic7xxx");

  switch (config->type)
  {
    case AIC_274x:
#if 0
      printk("aha274x: HCNTRL:0x%x\n", inb(HCNTRL(base)));
#endif
      /*
       * For some 274x boards, we must clear the CHIPRST bit
       * and pause the sequencer. For some reason, this makes
       * the driver work. For 284x boards, we give it a
       * CHIPRST just like the 294x boards.
       *
       * Use the BIOS settings to determine the interrupt
       * trigger type (level or edge) and use this value
       * for pausing and unpausing the sequencer.
       */
      config->unpause = (inb(HCNTRL(base)) & IRQMS) | INTEN;
      config->pause = config->unpause | PAUSE;
      config->extended = aic7xxx_extended;

      outb(config->pause | CHIPRST, HCNTRL(base));
      aic7xxx_delay(1);
      if (inb(HCNTRL(base)) & CHIPRST)
      {
	printk("aic7xxx_register: Chip reset not cleared; clearing manually.\n");
      }
      outb(config->pause, HCNTRL(base));

      /*
       * Just to be on the safe side with the 274x, we will re-read the irq
       * since there was some issue about reseting the board.
       */
      config->irq = inb(HA_INTDEF(base)) & 0x0F;
      if ((inb(HA_274_BIOSCTRL(base)) & BIOSMODE) == BIOSDISABLED)
      {
        bios_disabled = 1;
      }
      host_conf = inb(HA_HOSTCONF(base));
      config->busrtime = host_conf & 0x3C;
      /* XXX Is this valid for motherboard based controllers? */
      /* Setup the FIFO threshold and the bus off time */
      outb(host_conf & DFTHRSH, BUSSPD(base));
      outb((host_conf << 2) & BOFF, BUSTIME(base));

      /*
       * A reminder until this can be detected automatically.
       */
      printk("aha274x: Extended translation %sabled\n",
	     config->extended ? "en" : "dis");
      break;

    case AIC_284x:
#if 0
      printk("aha284x: HCNTRL:0x%x\n", inb(HCNTRL(base)));
#endif
      outb(CHIPRST, HCNTRL(base));
      config->unpause = UNPAUSE_284X;
      config->pause = REQ_PAUSE; /* DWG would like to be like the rest */
      aic7xxx_delay(1);
      outb(config->pause, HCNTRL(base));

      config->extended = aic7xxx_extended;
      config->irq = inb(HA_INTDEF(base)) & 0x0F;
      if ((inb(HA_274_BIOSCTRL(base)) & BIOSMODE) == BIOSDISABLED)
      {
        bios_disabled = 1;
      }
      host_conf = inb(HA_HOSTCONF(base));

      printk("aha284x: Reading SEEPROM...");
      have_seeprom = read_2840_seeprom(base, &sc);
      if (!have_seeprom)
      {
	printk("aha284x: Unable to read SEEPROM\n");
        config->busrtime = host_conf & 0x3C;
      }
      else
      {
	printk("done.\n");
	config->extended = ((sc.bios_control & CFEXTEND) >> 7);
	config->scsi_id = (sc.brtime_id & CFSCSIID);
	config->parity = (sc.adapter_control & CFSPARITY) ?
			 AIC_ENABLED : AIC_DISABLED;
	config->low_term = (sc.adapter_control & CFSTERM) ?
			      AIC_ENABLED : AIC_DISABLED;
	config->high_term = (sc.adapter_control & CFWSTERM) ?
			      AIC_ENABLED : AIC_DISABLED;
	config->busrtime = ((sc.brtime_id & CFBRTIME) >> 8);
      }
      /* XXX Is this valid for motherboard based controllers? */
      /* Setup the FIFO threshold and the bus off time */
      outb(host_conf & DFTHRSH, BUSSPD(base));
      outb((host_conf << 2) & BOFF, BUSTIME(base));

      printk("aha284x: Extended translation %sabled\n",
	     config->extended ? "en" : "dis");
      break;

    case AIC_7850:
    case AIC_7870:
    case AIC_7872:
    case AIC_7880:
    case AIC_7882:
#if 0
      printk("aic%s HCNTRL:0x%x\n", board_name[config->type], inb(HCNTRL(base)));
#endif

      outb(CHIPRST, HCNTRL(base));
      config->unpause = UNPAUSE_294X;
      config->pause = config->unpause | PAUSE;
      aic7xxx_delay(1);
      outb(config->pause, HCNTRL(base));

      config->extended = aic7xxx_extended;
      config->scsi_id = 7;

      printk("aic78xx: Reading SEEPROM...");
      have_seeprom = read_seeprom(base, config->chan_num * (sizeof(sc) / 2), &sc);
      if (!have_seeprom)
      {
	printk("aic78xx: Unable to read SEEPROM\n");
      }
      else
      {
	printk("done.\n");
	config->extended = ((sc.bios_control & CFEXTEND) >> 7);
	config->scsi_id = (sc.brtime_id & CFSCSIID);
	config->parity = (sc.adapter_control & CFSPARITY) ?
			 AIC_ENABLED : AIC_DISABLED;
	config->low_term = (sc.adapter_control & CFSTERM) ?
			      AIC_ENABLED : AIC_DISABLED;
	config->high_term = (sc.adapter_control & CFWSTERM) ?
			      AIC_ENABLED : AIC_DISABLED;
	config->busrtime = ((sc.brtime_id & CFBRTIME) >> 8);
        if (((config->type == AIC_7880) || (config->type == AIC_7882)) &&
            (sc.adapter_control & CFULTRAEN))
        {
          printk ("aic7xxx: Enabling support for Ultra SCSI speed.\n");
          config->ultra_enabled = TRUE;
        }
      }

      /*
       * XXX - force data fifo threshold to 100%. Why does this
       *       need to be done?
       */
      outb(inb(DSPCISTATUS(base)) | DFTHRESH, DSPCISTATUS(base));
      outb(config->scsi_id | DFTHRESH, HA_SCSICONF(base));

      /*
       * In case we are a wide card, place scsi ID in second conf byte.
       */
      outb(config->scsi_id, (HA_SCSICONF(base) + 1));

      printk("aic%s: Extended translation %sabled\n", board_name[config->type],
	     config->extended ? "en" : "dis");
      break;

    default:
      panic("aic7xxx_register: internal error\n");
  }

  config->maxscb = detect_maxscb(config->type, base, config->walk_scbs);

  if ((config->type == AIC_274x) || (config->type == AIC_284x))
  {
    if (config->pause & IRQMS)
    {
      printk("aic7xxx: Using Level Sensitive Interrupts\n");
    }
    else
    {
      printk("aic7xxx: Using Edge Triggered Interrupts\n");
    }
  }

  /*
   * Read the bus type from the SBLKCTL register. Set the FLAGS
   * register in the sequencer for twin and wide bus cards.
   */
  sblkctl = inb(SBLKCTL(base));
  switch (sblkctl & SELBUS_MASK)
  {
    case SELSINGLE:     /* narrow/normal bus */
      config->scsi_id = inb(HA_SCSICONF(base)) & 0x07;
      config->bus_type = AIC_SINGLE;
      outb(SINGLE_BUS, HA_FLAGS(base));
      break;

    case SELWIDE:     /* Wide bus */
      config->scsi_id = inb(HA_SCSICONF(base) + 1) & 0x0F;
      config->bus_type = AIC_WIDE;
      printk("aic7xxx: Enabling wide channel of %s-Wide\n",
	     board_name[config->type]);
      outb(WIDE_BUS, HA_FLAGS(base));
      break;

    case SELBUSB:     /* Twin bus */
      config->scsi_id = inb(HA_SCSICONF(base)) & 0x07;
#ifdef AIC7XXX_TWIN_SUPPORT
      config->scsi_id_b = inb(HA_SCSICONF(base) + 1) & 0x07;
      config->bus_type = AIC_TWIN;
      printk("aic7xxx: Enabled channel B of %s-Twin\n",
	     board_name[config->type]);
      outb(TWIN_BUS, HA_FLAGS(base));
#else
      config->bus_type = AIC_SINGLE;
      printk("aic7xxx: Channel B of %s-Twin will be ignored\n",
	     board_name[config->type]);
      outb(0, HA_FLAGS(base));
#endif
      break;

    default:
      printk("aic7xxx is an unsupported type 0x%x, please "
	     "mail deang@ims.com\n", inb(SBLKCTL(base)));
      outb(0, HA_FLAGS(base));
      return (0);
  }

  /*
   * For the 294x cards, clearing DIAGLEDEN and DIAGLEDON, will
   * take the card out of diagnostic mode and make the host adatper
   * LED follow bus activity (will not always be on).
   */
  outb(sblkctl & ~(DIAGLEDEN | DIAGLEDON), SBLKCTL(base));

  /*
   * The IRQ level in i/o port 4 maps directly onto the real
   * IRQ number. If it's ok, register it with the kernel.
   *
   * NB. the Adaptec documentation says the IRQ number is only
   *     in the lower four bits; the ECU information shows the
   *     high bit being used as well. Which is correct?
   *
   * The 294x cards (PCI) get their interrupt from PCI BIOS.
   */
  if (((config->type == AIC_274x) || (config->type == AIC_284x)) &&
      (config->irq < 9 || config->irq > 15))
  {
    printk("aic7xxx uses unsupported IRQ level, ignoring.\n");
    return (0);
  }

  /*
   * Check the IRQ to see if it is shared by another aic7xxx
   * controller. If it is and sharing of IRQs is not defined,
   * then return 0 hosts found. If sharing of IRQs is allowed
   * or the IRQ is not shared by another host adapter, then
   * proceed.
   */
#ifndef AIC7XXX_SHARE_IRQS
   if (aic7xxx_boards[config->irq] != NULL)
   {
     printk("aic7xxx_register: Sharing of IRQs is not configured.\n");
     return (0);
   }
#endif

  /*
   * Print out debugging information before re-enabling
   * the card - a lot of registers on it can't be read
   * when the sequencer is active.
   */
  debug_config(config);

  /*
   * Before registry, make sure that the offsets of the
   * struct scatterlist are what the sequencer will expect,
   * otherwise disable scatter-gather altogether until someone
   * can fix it. This is important since the sequencer will
   * DMA elements of the SG array in while executing commands.
   */
  if (template->sg_tablesize != SG_NONE)
  {
    struct scatterlist sg;

    if (SG_STRUCT_CHECK(sg))
    {
      printk("aic7xxx warning: kernel scatter-gather "
	     "structures changed, disabling it.\n");
      template->sg_tablesize = SG_NONE;
    }
  }

  /*
   * Register each "host" and fill in the returned Scsi_Host
   * structure as best we can. Some of the parameters aren't
   * really relevant for bus types beyond ISA, and none of the
   * high-level SCSI code looks at it anyway. Why are the fields
   * there? Also save the pointer so that we can find the
   * information when an IRQ is triggered.
   */
  host = scsi_register(template, sizeof(struct aic7xxx_host));
  host->can_queue = config->maxscb;
  host->cmd_per_lun = AIC7XXX_CMDS_PER_LUN;
  host->this_id = config->scsi_id;
  host->irq = config->irq;
  if (config->bus_type == AIC_WIDE)
  {
    host->max_id = 16;
  }
  if (config->bus_type == AIC_TWIN)
  {
    host->max_channel = 1;
  }

  p = (struct aic7xxx_host *) host->hostdata;

  /*
   * Initialize the scb array by setting the state to free.
   */
  for (i = 0; i < AIC7XXX_MAXSCB; i++)
  {
    p->scb_array[i].state = SCB_FREE;
    p->scb_array[i].next = NULL;
    p->scb_array[i].cmd = NULL;
  }

  p->isr_count = 0;
  p->a_scanned = 0;
  p->b_scanned = 0;
  p->base = base;
  p->maxscb = config->maxscb;
  p->numscb = 0;
  p->extended = config->extended;
  p->type = config->type;
  p->ultra_enabled = config->ultra_enabled;
  p->chan_num = config->chan_num;
  p->bus_type = config->bus_type;
  p->have_seeprom = have_seeprom;
  p->seeprom = sc;
  p->free_scb = NULL;
  p->next = NULL;

  p->unpause = config->unpause;
  p->pause = config->pause;

  if (aic7xxx_boards[config->irq] == NULL)
  {
    /*
     * Warning! This must be done before requesting the irq.  It is
     * possible for some boards to raise an interrupt as soon as
     * they are enabled.  So when we request the irq from the Linux
     * kernel, an interrupt is triggered immediately.  Therefore, we
     * must ensure the board data is correctly set before the request.
     */
    aic7xxx_boards[config->irq] = host;

    /*
     * Register IRQ with the kernel.
     */
    if (request_irq(config->irq, aic7xxx_isr, SA_INTERRUPT, "aic7xxx"))
    {
      printk("aic7xxx couldn't register irq %d, ignoring\n", config->irq);
      aic7xxx_boards[config->irq] = NULL;
      return (0);
    }
  }
  else
  {
    /*
     * We have found a host adapter sharing an IRQ of a previously
     * registered host adapter. Add this host adapter's Scsi_Host
     * to the beginning of the linked list of hosts at the same IRQ.
     */
    p->next = aic7xxx_boards[config->irq];
    aic7xxx_boards[config->irq] = host;
  }

  /*
   * Load the sequencer program, then re-enable the board -
   * resetting the AIC-7770 disables it, leaving the lights
   * on with nobody home. On the PCI bus you *may* be home,
   * but then your mailing address is dynamically assigned
   * so no one can find you anyway :-)
   */
  printk("aic7xxx: Downloading sequencer code...");
  aic7xxx_loadseq(base);

  /*
   * Set Fast Mode and Enable the board
   */
  outb(FASTMODE, SEQCTL(base));

  if ((p->type == AIC_274x) || (p->type == AIC_284x))
  {
    outb(ENABLE, BCTL(base));
  }

  printk("done.\n");

  /*
   * Set the SCSI Id, SXFRCTL0, SXFRCTL1, and SIMODE1, for both channels
   */
  if (p->bus_type == AIC_TWIN)
  {
    /*
     * Select Channel B.
     */
    outb((sblkctl & ~SELBUS_MASK) | SELBUSB, SBLKCTL(base));

    outb(config->scsi_id_b, SCSIID(base));
    scsi_conf = inb(HA_SCSICONF(base) + 1) & (ENSPCHK | STIMESEL);
    outb(scsi_conf | ENSTIMER | ACTNEGEN | STPWEN, SXFRCTL1(base));
    outb(ENSELTIMO | ENSCSIPERR, SIMODE1(base));
    if (p->ultra_enabled)
    {
      outb(ULTRAEN, SXFRCTL0(base));
    }

    /*
     * Select Channel A
     */
    outb((sblkctl & ~SELBUS_MASK) | SELSINGLE, SBLKCTL(base));
  }
  outb(config->scsi_id, SCSIID(base));
  scsi_conf = inb(HA_SCSICONF(base)) & (ENSPCHK | STIMESEL);
  outb(scsi_conf | ENSTIMER | ACTNEGEN | STPWEN, SXFRCTL1(base));
  outb(ENSELTIMO | ENSCSIPERR, SIMODE1(base));
  if (p->ultra_enabled)
  {
    outb(ULTRAEN, SXFRCTL0(base));
  }

  /*
   * Look at the information that board initialization or the board
   * BIOS has left us. In the lower four bits of each target's
   * scratch space any value other than 0 indicates that we should
   * initiate synchronous transfers. If it's zero, the user or the
   * BIOS has decided to disable synchronous negotiation to that
   * target so we don't activate the needsdtr flag.
   */
  p->needsdtr_copy = 0;
  p->sdtr_pending = 0;
  p->needwdtr_copy = 0;
  p->wdtr_pending = 0;
  if (p->bus_type == AIC_SINGLE)
  {
    max_targets = 8;
  }
  else
  {
    max_targets = 16;
  }

  /*
   * Grab the disconnection disable table and invert it for our needs
   */
  if (have_seeprom)
  {
    p->discenable = 0;
  }
  else
  {
    if (bios_disabled)
    {
      printk("aic7xxx : Host Adapter Bios disabled. Using default SCSI "
             "device parameters\n");
      p->discenable = 0xFFFF;
    }
    else
    {
      p->discenable = ~((inb(HA_DISC_DSB(base + 1)) << 8) |
          inb(HA_DISC_DSB(base)));
    }
  }

  for (i = 0; i < max_targets; i++)
  {
    if (have_seeprom)
    {
      target_settings = ((sc.device_flags[i] & CFXFER) << 4);
      if (sc.device_flags[i] & CFSYNCH)
      {
	p->needsdtr_copy |= (0x01 << i);
      }
      if ((sc.device_flags[i] & CFWIDEB) && (p->bus_type == AIC_WIDE))
      {
	p->needwdtr_copy |= (0x01 << i);
      }
      if (sc.device_flags[i] & CFDISC)
      {
        p->discenable |= (0x01 << i);
      }
    }
    else
    {
      target_settings = inb(HA_TARG_SCRATCH(base) + i);
      if (target_settings & 0x0F)
      {
	p->needsdtr_copy |= (0x01 << i);
	/*
	 * Default to asynchronous transfers (0 offset)
	 */
	target_settings &= 0xF0;
      }
      /*
       * If we are not wide, forget WDTR. This makes the driver
       * work on some cards that don't leave these fields cleared
       * when BIOS is not installed.
       */
      if ((target_settings & 0x80) && (p->bus_type == AIC_WIDE))
      {
	p->needwdtr_copy |= (0x01 << i);
	target_settings &= 0x7F;
      }
    }
    outb(target_settings, (HA_TARG_SCRATCH(base) + i));
  }

  p->needsdtr = p->needsdtr_copy;
  p->needwdtr = p->needwdtr_copy;
#if 0
  printk("NeedSdtr = 0x%x, 0x%x\n", p->needsdtr_copy, p->needsdtr);
  printk("NeedWdtr = 0x%x, 0x%x\n", p->needwdtr_copy, p->needwdtr);
#endif

  /*
   * Clear the control byte for every SCB so that the sequencer
   * doesn't get confused and think that one of them is valid
   */
  for (i = 0; i < config->maxscb; i++)
  {
    outb(i, SCBPTR(base));
    outb(0, SCBARRAY(base));
  }

  /*
   * For reconnecting targets, the sequencer code needs to
   * know how many SCBs it has to search through.
   */
  outb(config->maxscb, HA_SCBCOUNT(base));

  /*
   * Clear the active flags - no targets are busy.
   */
  outb(0, HA_ACTIVE0(base));
  outb(0, HA_ACTIVE1(base));

  /*
   * We don't have any waiting selections
   */
  outb(SCB_LIST_NULL, WAITING_SCBH(base));
  outb(SCB_LIST_NULL, WAITING_SCBT(base));

  /*
   * Reset the SCSI bus. Is this necessary?
   *   There may be problems for a warm boot without resetting
   *   the SCSI bus. Either BIOS settings in scratch RAM
   *   will not get reinitialized, or devices may stay at
   *   previous negotiated settings (SDTR and WDTR) while
   *   the driver will think that no negotiations have been
   *   performed.
   *
   * Some devices need a long time to "settle" after a SCSI
   * bus reset.
   */

  if (!aic7xxx_no_reset)
  {
    printk("aic7xxx: Resetting the SCSI bus...");
    if (p->bus_type == AIC_TWIN)
    {
      /*
       * Select Channel B.
       */
      outb((sblkctl & ~SELBUS_MASK) | SELBUSB, SBLKCTL(base));

      outb(SCSIRSTO, SCSISEQ(base));
      udelay(1000);
      outb(0, SCSISEQ(base));

      /*
       * Select Channel A.
       */
      outb((sblkctl & ~SELBUS_MASK) | SELSINGLE, SBLKCTL(base));
    }

    outb(SCSIRSTO, SCSISEQ(base));
    udelay(1000);
    outb(0, SCSISEQ(base));

    aic7xxx_delay(AIC7XXX_RESET_DELAY);

    printk("done.\n");
  }

  /*
   * Unpause the sequencer before returning and enable
   * interrupts - we shouldn't get any until the first
   * command is sent to us by the high-level SCSI code.
   */
  UNPAUSE_SEQUENCER(p);
  return (found);
}

/*+F*************************************************************************
 * Function:
 *   aic7xxx_detect
 *
 * Description:
 *   Try to detect and register an Adaptec 7770 or 7870 SCSI controller.
 *-F*************************************************************************/
int
aic7xxx_detect(Scsi_Host_Template *template)
{
  aha_type type = AIC_NONE;
  int found = 0, slot, base;
  unsigned char irq = 0;
  int i;
  struct aic7xxx_host_config config;

  template->proc_dir = &proc_scsi_aic7xxx;
  config.chan_num = 0;

  /*
   * Since we may allow sharing of IRQs, it is imperative
   * that we "null-out" the aic7xxx_boards array. It is
   * not guaranteed to be initialized to 0 (NULL). We use
   * a NULL entry to indicate that no prior hosts have
   * been found/registered for that IRQ.
   */
  for (i = 0; i <= MAXIRQ; i++)
  {
    aic7xxx_boards[i] = NULL;
  }

  /*
   * Initialize the spurious count to 0.
   */
  aic7xxx_spurious_count = 0;

  /*
   * EISA/VL-bus card signature probe.
   */
  for (slot = MINSLOT; slot <= MAXSLOT; slot++)
  {
    base = SLOTBASE(slot);

    if (check_region(MINREG(base), MAXREG(base) - MINREG(base)))
    {
      /*
       * Some other driver has staked a
       * claim to this i/o region already.
       */
      continue;
    }

    type = aic7xxx_probe(slot, HID0(base));
    if (type != AIC_NONE)
    {
      /*
       * We found a card, allow 1 spurious interrupt.
       */
      aic7xxx_spurious_count = 1;

#if 0
      printk("aic7xxx: HCNTRL:0x%x\n", inb(HCNTRL(base)));
      outb(inb(HCNTRL(base)) | CHIPRST, HCNTRL(base));
#endif

      /*
       * We "find" a AIC-7770 if we locate the card
       * signature and we can set it up and register
       * it with the kernel without incident.
       */
      config.type = type;
      config.base = base;
      config.irq = irq;
      config.parity = AIC_UNKNOWN;
      config.low_term = AIC_UNKNOWN;
      config.high_term = AIC_UNKNOWN;
      config.busrtime = 0;
      config.walk_scbs = FALSE;
      config.ultra_enabled = FALSE;
      found += aic7xxx_register(template, &config);

      /*
       * Disallow spurious interrupts.
       */
      aic7xxx_spurious_count = 0;
    }
  }

#ifdef CONFIG_PCI

  /*
   * PCI-bus probe.
   */
  if (pcibios_present())
  {
    int error;
    int done = 0;
    unsigned int io_port;
    unsigned short index = 0;
    unsigned char pci_bus, pci_device_fn;
    unsigned char devrevid, devconfig, devstatus;
    char rev_id[] = {'B', 'C', 'D'};

    while (!done)
    {
      if ((!pcibios_find_device(PCI_VENDOR_ID_ADAPTEC,
				PCI_DEVICE_ID_ADAPTEC_7870,
				index, &pci_bus, &pci_device_fn)) ||
	   (!pcibios_find_device(PCI_VENDOR_ID_ADAPTEC,
				PCI_DEVICE_ID_ADAPTEC_7871,
				index, &pci_bus, &pci_device_fn)))
      {
	type = AIC_7870;
      }
      else
      {
	if (!pcibios_find_device(PCI_VENDOR_ID_ADAPTEC,
				PCI_DEVICE_ID_ADAPTEC_7850,
				index, &pci_bus, &pci_device_fn))
	{
	  type = AIC_7850;
	}
	else
	{
	  if (!pcibios_find_device(PCI_VENDOR_ID_ADAPTEC,
				  PCI_DEVICE_ID_ADAPTEC_7872,
				  index, &pci_bus, &pci_device_fn))
	  {
	    type = AIC_7872;
            config.chan_num = number_of_3940s & 0x1;
            number_of_3940s++;
	  }
	  else
	  {
            if ((!pcibios_find_device(PCI_VENDOR_ID_ADAPTEC,
                                     PCI_DEVICE_ID_ADAPTEC_7881,
                                     index, &pci_bus, &pci_device_fn)) ||
                (!pcibios_find_device(PCI_VENDOR_ID_ADAPTEC,
                                     PCI_DEVICE_ID_ADAPTEC_7880,
                                     index, &pci_bus, &pci_device_fn)))
	    {
	      type = AIC_7880;
	    }
            else
            {
	      if (!pcibios_find_device(PCI_VENDOR_ID_ADAPTEC,
				      PCI_DEVICE_ID_ADAPTEC_7882,
				      index, &pci_bus, &pci_device_fn))
	      {
	        type = AIC_7882;
              }
              else
              {
	        type = AIC_NONE;
	        done = 1;
              }
            }
          }
	}
      }

      if (!done)
      {
	/*
	 * Read esundry information from PCI BIOS.
	 */
	error = pcibios_read_config_dword(pci_bus, pci_device_fn,
					  PCI_BASE_ADDRESS_0, &io_port);
	if (error)
	{
	  panic("aic7xxx_detect: error 0x%x reading i/o port.\n", error);
	}

	error = pcibios_read_config_byte(pci_bus, pci_device_fn,
					 PCI_INTERRUPT_LINE, &irq);
	if (error)
	{
	  panic("aic7xxx_detect: error %d reading irq.\n", error);
	}

	error = pcibios_read_config_byte(pci_bus, pci_device_fn,
					 DEVREVID, &devrevid);
	if (error)
	{
	  panic("aic7xxx_detect: error %d reading device revision id.\n", error);
	}

	if (devrevid < 3)
	{
	  printk("aic7xxx_detect: AIC-7870 Rev %c\n", rev_id[devrevid]);
	}

	error = pcibios_read_config_byte(pci_bus, pci_device_fn,
					 DEVCONFIG, &devconfig);
	if (error)
	{
	  panic("aic7xxx_detect: error %d reading device configuration.\n", error);
	}

	error = pcibios_read_config_byte(pci_bus, pci_device_fn,
					 DEVSTATUS, &devstatus);
	if (error)
	{
	  panic("aic7xxx_detect: error %d reading device status.\n", error);
	}

	printk("aic7xxx_detect: devconfig 0x%x, devstatus 0x%x\n",
	       devconfig, devstatus);

	/*
	 * Make the base I/O register look like EISA and VL-bus.
	 */
	base = io_port - 0xC01;

	/*
	 * I don't think we need to bother with allowing
	 * spurious interrupts for the 787x/7850, but what
	 * the hey.
	 */
	aic7xxx_spurious_count = 1;

        config.type = type;
        config.base = base;
        config.irq = irq;
        config.parity = AIC_UNKNOWN;
        config.low_term = AIC_UNKNOWN;
        config.high_term = AIC_UNKNOWN;
        config.busrtime = 0;
        config.walk_scbs = FALSE;
        config.ultra_enabled = FALSE;
	if ((devstatus & RAMPSM) || (devconfig & SCBRAMSEL) ||
            (type == AIC_7872))
	{
          config.walk_scbs = TRUE;
	}
	found += aic7xxx_register(template, &config);

	/*
	 * Disable spurious interrupts.
	 */
	aic7xxx_spurious_count = 0;

	index++;
      }
    }
  }
#endif CONFIG_PCI

  template->name = aic7xxx_info(NULL);
  return (found);
}


/*+F*************************************************************************
 * Function:
 *   aic7xxx_buildscb
 *
 * Description:
 *   Build a SCB.
 *-F*************************************************************************/
static void
aic7xxx_buildscb(struct aic7xxx_host *p,
		 Scsi_Cmnd *cmd,
		 struct aic7xxx_scb *scb)
{
  void *addr;
  unsigned short mask;
  struct scatterlist *sg;

  /*
   * Setup the control byte if we need negotiation and have not
   * already requested it.
   */
#ifdef AIC7XXX_TAGGED_QUEUEING
  if (cmd->device->tagged_supported)
  {
    if (cmd->device->tagged_queue == 0)
    {
      printk("aic7xxx_buildscb: Enabling tagged queuing for target %d, "
	     "channel %d\n", cmd->target, cmd->channel);
      cmd->device->tagged_queue = 1;
      cmd->device->current_tag = 1;  /* enable tagging */
    }
    cmd->tag = cmd->device->current_tag;
    cmd->device->current_tag++;
    scb->control |= SCB_TE;
  }
#endif
  mask = (0x01 << (cmd->target | (cmd->channel << 3)));
  if (p->discenable & mask)
  {
    scb->control |= SCB_DISCENB;
  }
  if ((p->needwdtr & mask) && !(p->wdtr_pending & mask))
  {
    p->wdtr_pending |= mask;
    scb->control |= SCB_NEEDWDTR;
#if 0
    printk("Sending WDTR request to target %d.\n", cmd->target);
#endif
  }
  else
  {
    if ((p->needsdtr & mask) && !(p->sdtr_pending & mask))
    {
      p->sdtr_pending |= mask;
      scb->control |= SCB_NEEDSDTR;
#if 0
      printk("Sending SDTR request to target %d.\n", cmd->target);
#endif
    }
  }

#if 0
  printk("aic7xxx_queue: target %d, cmd 0x%x (size %u), wdtr 0x%x, mask 0x%x\n",
	 cmd->target, cmd->cmnd[0], cmd->cmd_len, p->needwdtr, mask);
#endif
  scb->target_channel_lun = ((cmd->target << 4) & 0xF0) |
	((cmd->channel & 0x01) << 3) | (cmd->lun & 0x07);

  /*
   * The interpretation of request_buffer and request_bufflen
   * changes depending on whether or not use_sg is zero; a
   * non-zero use_sg indicates the number of elements in the
   * scatter-gather array.
   */

  /*
   * XXX - this relies on the host data being stored in a
   *       little-endian format.
   */
  addr = cmd->cmnd;
  scb->SCSI_cmd_length = cmd->cmd_len;
  memcpy(scb->SCSI_cmd_pointer, &addr, sizeof(scb->SCSI_cmd_pointer));

  if (cmd->use_sg)
  {
#if 0
    debug("aic7xxx_buildscb: SG used, %d segments, length %u\n",
           cmd->use_sg, length);
#endif
    scb->SG_segment_count = cmd->use_sg;
    memcpy(scb->SG_list_pointer, &cmd->request_buffer,
	   sizeof(scb->SG_list_pointer));
    memcpy(&sg, &cmd->request_buffer, sizeof(sg));
    memcpy(scb->data_pointer, &(sg[0].address), sizeof(scb->data_pointer));
    scb->data_count[0] = sg[0].length & 0xFF;
    scb->data_count[1] = (sg[0].length >> 8) & 0xFF;
    scb->data_count[2] = (sg[0].length >> 16) & 0xFF;
  }
  else
  {
#if 0
  debug("aic7xxx_buildscb: Creating scatterlist, addr=0x%lx, length=%d.\n",
	(unsigned long) cmd->request_buffer, cmd->request_bufflen);
#endif
    if (cmd->request_bufflen == 0)
    {
      /*
       * In case the higher level SCSI code ever tries to send a zero
       * length command, ensure the SCB indicates no data.  The driver
       * will interpret a zero length command as a Bus Device Reset.
       */
      scb->SG_segment_count = 0;
      memset(scb->SG_list_pointer, 0, sizeof(scb->SG_list_pointer));
      memset(scb->data_pointer, 0, sizeof(scb->data_pointer));
      memset(scb->data_count, 0, sizeof(scb->data_count));
    }
    else
    {
      scb->SG_segment_count = 1;
      scb->sg.address = (char *) cmd->request_buffer;
      scb->sg.length = cmd->request_bufflen;
      addr = &scb->sg;
      memcpy(scb->SG_list_pointer, &addr, sizeof(scb->SG_list_pointer));
      scb->data_count[0] = scb->sg.length & 0xFF;
      scb->data_count[1] = (scb->sg.length >> 8) & 0xFF;
      scb->data_count[2] = (scb->sg.length >> 16) & 0xFF;
      memcpy(scb->data_pointer, &cmd->request_buffer, sizeof(scb->data_pointer));
    }
  }
}

/*+F*************************************************************************
 * Function:
 *   aic7xxx_queue
 *
 * Description:
 *   Queue a SCB to the controller.
 *-F*************************************************************************/
int
aic7xxx_queue(Scsi_Cmnd *cmd, void (*fn)(Scsi_Cmnd *))
{
  long flags;
  struct aic7xxx_host *p;
  struct aic7xxx_scb *scb;
  unsigned char curscb;

  p = (struct aic7xxx_host *) cmd->host->hostdata;

  /*
   * Check to see if channel was scanned.
   */
  if (!p->a_scanned && (cmd->channel == 0))
  {
    printk("aic7xxx: Scanning channel A for devices.\n");
    p->a_scanned = 1;
  }
  else
  {
    if (!p->b_scanned && (cmd->channel == 1))
    {
      printk("aic7xxx: Scanning channel B for devices.\n");
      p->b_scanned = 1;
    }
  }

#if 0
  debug("aic7xxx_queue: cmd 0x%x (size %u), target %d, channel %d, lun %d\n",
	cmd->cmnd[0], cmd->cmd_len, cmd->target, cmd->channel,
	cmd->lun & 0x07);
#endif

  /*
   * This is a critical section, since we don't want the
   * interrupt routine mucking with the host data or the
   * card. Since the kernel documentation is vague on
   * whether or not we are in a cli/sti pair already, save
   * the flags to be on the safe side.
   */
  save_flags(flags);
  cli();

  /*
   * Find a free slot in the SCB array to load this command
   * into. Since can_queue is set to the maximum number of
   * SCBs for the card, we should always find one.
   *
   * First try to find an scb in the free list. If there are
   * none in the free list, then check the current number of
   * of scbs and take an unused one from the scb array.
   */
  scb = p->free_scb;
  if (scb != NULL)
  { /* found one in the free list */
    p->free_scb = scb->next;   /* remove and update head of list */
    /*
     * Warning! For some unknown reason, the scb at the head
     * of the free list is not the same address that it should
     * be. That's why we set the scb pointer taken by the
     * position in the array. The scb at the head of the list
     * should match this address, but it doesn't.
     */
    scb = &(p->scb_array[scb->position]);
    scb->control = 0;
    scb->state = SCB_ACTIVE;
  }
  else
  {
    if (p->numscb >= p->maxscb)
    {
      panic("aic7xxx_queue: couldn't find a free scb\n");
    }
    else
    {
      /*
       * Initialize the scb within the scb array. The
       * position within the array is the position on
       * the board that it will be loaded.
       */
      scb = &(p->scb_array[p->numscb]);
      memset(scb, 0, sizeof(*scb));

      scb->position = p->numscb;
      p->numscb++;
      scb->state = SCB_ACTIVE;
      scb->next_waiting = SCB_LIST_NULL;
      memcpy(scb->host_scb, &scb, sizeof(scb));
      scb->control = SCB_NEEDDMA;
      PAUSE_SEQUENCER(p);
      curscb = inb(SCBPTR(p->base));
      outb(scb->position, SCBPTR(p->base));
      aic7xxx_putscb_dma(p->base, scb);
      outb(curscb, SCBPTR(p->base));
      UNPAUSE_SEQUENCER(p);
      scb->control = 0;
    }
  }

  scb->cmd = cmd;
  aic7xxx_position(cmd) = scb->position;
#if 0
  debug_scb(scb);
#endif;

  /*
   * Construct the SCB beforehand, so the sequencer is
   * paused a minimal amount of time.
   */
  aic7xxx_buildscb(p, cmd, scb);

#if 0
  if (scb != &p->scb_array[scb->position])
  {
    printk("aic7xxx_queue: address of scb by position does not match scb address\n");
  }
  printk("aic7xxx_queue: SCB pos=%d, cmdptr=0x%x, state=%d, freescb=0x%x\n",
	 scb->position, (unsigned int) scb->cmd,
	 scb->state, (unsigned int) p->free_scb);
#endif
  /*
   * Pause the sequencer so we can play with its registers -
   * wait for it to acknowledge the pause.
   *
   * XXX - should the interrupts be left on while doing this?
   */
  PAUSE_SEQUENCER(p);

  /*
   * Save the SCB pointer and put our own pointer in - this
   * selects one of the four banks of SCB registers. Load
   * the SCB, then write its pointer into the queue in FIFO
   * and restore the saved SCB pointer.
   */
  aic7xxx_putscb(p->base, scb);

  /*
   * Make sure the Scsi_Cmnd pointer is saved, the struct it
   * points to is set up properly, and the parity error flag
   * is reset, then unpause the sequencer and watch the fun
   * begin.
   */
  cmd->scsi_done = fn;
  aic7xxx_error(cmd) = DID_OK;
  aic7xxx_status(cmd) = 0;
  scb->timer_status = 0x0;
  cmd->result = 0;
  memset(&cmd->sense_buffer, 0, sizeof(cmd->sense_buffer));

  UNPAUSE_SEQUENCER(p);
#if 0
  printk("aic7xxx_queue: After - cmd = 0x%lx, scb->cmd = 0x%lx, pos = %d\n",
         (long) cmd, (long) scb->cmd, scb->position);
#endif;
  restore_flags(flags);
  return (0);
}

/*+F*************************************************************************
 * Function:
 *   aic7xxx_abort_scb
 *
 * Description:
 *   Abort an scb.  If the scb has not previously been aborted, then
 *   we attempt to send a BUS_DEVICE_RESET message to the target.  If
 *   the scb has previously been unsuccessfully aborted, then we will
 *   reset the channel and have all devices renegotiate.
 *-F*************************************************************************/
static void
aic7xxx_abort_scb(struct aic7xxx_host *p, struct aic7xxx_scb *scb)
{
  int base = p->base;
  int found = 0;
  char channel = scb->target_channel_lun & SELBUSB ? 'B': 'A';

  /*
   * Ensure that the card doesn't do anything
   * behind our back.
   */
  PAUSE_SEQUENCER(p);

  /*
   * First, determine if we want to do a bus reset or simply a bus device
   * reset.  If this is the first time that a transaction has timed out,
   * just schedule a bus device reset.  Otherwise, we reset the bus and
   * abort all pending I/Os on that bus.
   */
  if (scb->state & SCB_ABORTED)
  {
    /*
     * Been down this road before.  Do a full bus reset.
     */
    found = aic7xxx_reset_channel(p, channel, scb->position);
  }
  else
  {
    unsigned char active_scb, control;
    struct aic7xxx_scb *active_scbp;

    /*
     * Send a Bus Device Reset Message:
     * The target we select to send the message to may be entirely
     * different than the target pointed to by the scb that timed
     * out.  If the command is in the QINFIFO or the waiting for
     * selection list, its not tying up the bus and isn't responsible
     * for the delay so we pick off the active command which should
     * be the SCB selected by SCBPTR.  If its disconnected or active,
     * we device reset the target scbp points to.  Although it may
     * be that this target is not responsible for the delay, it may
     * may also be that we're timing out on a command that just takes
     * too much time, so we try the bus device reset there first.
     */
    active_scb = inb(SCBPTR(base));
    active_scbp = &(p->scb_array[active_scb]);
    control = inb(SCBARRAY(base));

    /*
     * Test to see if scbp is disconnected
     */
    outb(scb->position, SCBPTR(base));
    if (inb(SCBARRAY(base)) & SCB_DIS)
    {
      scb->state |= (SCB_DEVICE_RESET | SCB_ABORTED);
      scb->SG_segment_count = 0;
      memset(scb->SG_list_pointer, 0, sizeof(scb->SG_list_pointer));
      memset(scb->data_pointer, 0, sizeof(scb->data_pointer));
      memset(scb->data_count, 0, sizeof(scb->data_count));
      outb(SCBAUTO, SCBCNT(base));
      asm volatile("cld\n\t"
                   "rep\n\t"
                   "outsb"
                   : /* no output */
                   :"S" (scb), "c" (SCB_DOWNLOAD_SIZE), "d" (SCBARRAY(base))
                   :"si", "cx", "dx");
      outb(0, SCBCNT(base));
      aic7xxx_add_waiting_scb(base, scb, LIST_SECOND);
      aic7xxx_scb_tsleep(p, scb, 2 * HZ);  /* unpauses the sequencer */
    }
    else
    {
      /*
       * Is the active SCB really active?
       */
      if ((active_scbp->state & SCB_ACTIVE) && (control & SCB_NEEDDMA))
      {
        unsigned char flags = inb(HA_FLAGS(base));
        if (flags & ACTIVE_MSG)
        {
          /*
           * If we're in a message phase, tacking on another message
           * may confuse the target totally. The bus is probably wedged,
           * so reset the channel.
           */
          channel = (active_scbp->target_channel_lun & SELBUSB) ? 'B': 'A';
          aic7xxx_reset_channel(p, channel, scb->position);
        }
        else
        {
          /*
           * Load the message buffer and assert attention.
           */
          active_scbp->state |= (SCB_DEVICE_RESET | SCB_ABORTED);
          outb(flags | ACTIVE_MSG, HA_FLAGS(base));
          outb(1, HA_MSG_LEN(base));
          outb(MSG_BUS_DEVICE_RESET, HA_MSG_START(base));
          if (active_scbp->target_channel_lun != scb->target_channel_lun)
          {
            /*
             * XXX - We would like to increment the timeout on scb, but
             *       access to that routine is denied because it is hidden
             *       in scsi.c.  If we were able to do this, it would give
             *       scb a new lease on life.
             */
            ;
          }
          aic7xxx_scb_tsleep(p, active_scbp, 2 * HZ);
        }
      }
      else
      {
        /*
         * No active command to single out, so reset
         * the bus for the timed out target.
         */
        aic7xxx_reset_channel(p, channel, scb->position);
      }
    }
  }
}

/*+F*************************************************************************
 * Function:
 *   aic7xxx_abort
 *
 * Description:
 *   Abort the current SCSI command(s).
 *-F*************************************************************************/
int
aic7xxx_abort(Scsi_Cmnd *cmd)
{
  struct aic7xxx_scb  *scb;
  struct aic7xxx_host *p;
  long flags;

  p = (struct aic7xxx_host *) cmd->host->hostdata;
  scb = &(p->scb_array[aic7xxx_position(cmd)]);

  save_flags(flags);
  cli();

  if (scb->state & SCB_ACTIVE)
  {
    if (scb->state & SCB_IMMED)
    {
      /*
       * Don't know how set the number of retries to 0.
       */
      /* cmd->retries = 0; */
      aic7xxx_done (p, scb);
    }
    else
    {
      /*
       * Abort the operation.
       */
      aic7xxx_abort_scb(p, scb);
    }
  }
  restore_flags(flags);
  return (0);
}

/*+F*************************************************************************
 * Function:
 *   aic7xxx_reset
 *
 * Description:
 *   Resetting the bus always succeeds - is has to, otherwise the
 *   kernel will panic! Try a surgical technique - sending a BUS
 *   DEVICE RESET message - on the offending target before pulling
 *   the SCSI bus reset line.
 *-F*************************************************************************/
int
aic7xxx_reset(Scsi_Cmnd *cmd)
{
  return (aic7xxx_abort(cmd));
}

/*+F*************************************************************************
 * Function:
 *   aic7xxx_biosparam
 *
 * Description:
 *   Return the disk geometry for the given SCSI device.
 *-F*************************************************************************/
int
aic7xxx_biosparam(Disk *disk, kdev_t dev, int geom[])
{
  int heads, sectors, cylinders;
  struct aic7xxx_host *p;

  p = (struct aic7xxx_host *) disk->device->host->hostdata;

  /*
   * XXX - if I could portably find the card's configuration
   *       information, then this could be autodetected instead
   *       of left to a boot-time switch.
   */
  heads = 64;
  sectors = 32;
  cylinders = disk->capacity / (heads * sectors);

  if (p->extended && cylinders > 1024)
  {
    heads = 255;
    sectors = 63;
    cylinders = disk->capacity / (255 * 63);
  }

  geom[0] = heads;
  geom[1] = sectors;
  geom[2] = cylinders;

  return (0);
}

#include "aic7xxx_proc.c"

#ifdef MODULE
/* Eventually this will go into an include file, but this will be later */
Scsi_Host_Template driver_template = AIC7XXX;

#include "scsi_module.c"
#endif

/*
 * Overrides for Emacs so that we almost follow Linus's tabbing style.
 * Emacs will notice this stuff at the end of the file and automatically
 * adjust the settings for this buffer only.  This must remain at the end
 * of the file.
 * ---------------------------------------------------------------------------
 * Local variables:
 * c-indent-level: 2
 * c-brace-imaginary-offset: 0
 * c-brace-offset: -2
 * c-argdecl-indent: 2
 * c-label-offset: -2
 * c-continued-statement-offset: 2
 * c-continued-brace-offset: 0
 * indent-tabs-mode: nil
 * tab-width: 8
 * End:
 */
