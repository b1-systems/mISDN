/* $Id: sedl_fax.c,v 0.18 2001/05/18 00:48:52 kkeil Exp $
 *
 * sedl_fax.c  low level stuff for Sedlbauer Speedfax + cards
 *
 * Copyright  (C) 2000,2001 Karsten Keil (kkeil@suse.de)
 *
 * Author     Karsten Keil (kkeil@suse.de)
 *
 *
 * Thanks to  Sedlbauer AG for informations
 *            Marcus Niemann
 *            Edgar Toernig
 *
 * This file is (c) under GNU PUBLIC LICENSE
 *
 */

/* Supported cards:
 * Card:	Chip:		Configuration:	Comment:
 * ---------------------------------------------------------------------
 * Speed Fax+	ISAC_ISAR	ISAPNP		Full analog support
 * Speed Fax+ 	ISAC_ISAR	PCI PNP		Full analog support
 *
 * Important:
 * For the sedlbauer speed fax+ to work properly you have to download 
 * the firmware onto the card.
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/kernel_stat.h>
#include <linux/delay.h>
#include "hisax_hw.h"
#include "isac.h"
#include "isar.h"
#include "hisaxl1.h"
#include "helper.h"
#include "debug.h"

extern const char *CardType[];

const char *Sedlfax_revision = "$Revision: 0.18 $";

const char *Sedlbauer_Types[] =
	{"None", "speed fax+", "speed fax+ pyramid", "speed fax+ pci"};

#ifndef PCI_VENDOR_ID_TIGERJET
#define PCI_VENDOR_ID_TIGERJET		0xe159
#endif
#ifndef PCI_DEVICE_ID_TIGERJET_100
#define PCI_DEVICE_ID_TIGERJET_100	0x0002
#endif
#define PCI_SUBVENDOR_SPEEDFAX_PYRAMID	0x51
#define PCI_SUBVENDOR_SPEEDFAX_PCI	0x54
#define PCI_SUB_ID_SEDLBAUER		0x01
 
#define SEDL_SPEEDFAX_ISA	1
#define SEDL_SPEEDFAX_PYRAMID	2
#define SEDL_SPEEDFAX_PCI	3

#define byteout(addr,val) outb(val,addr)
#define bytein(addr) inb(addr)

#define SEDL_ISA_ISAC	4
#define SEDL_ISA_ISAR	6
#define SEDL_ISA_ADR	8
#define SEDL_ISA_RESET_ON	10
#define SEDL_ISA_RESET_OFF	12

#define SEDL_PCI_ADR	0xc8
#define SEDL_PCI_ISAC	0xd0
#define SEDL_PCI_ISAR	0xe0

/* TIGER 100 Registers */

#define TIGER_RESET_ADDR	0x00
#define TIGER_EXTERN_RESET_ON	0x01
#define TIGER_EXTERN_RESET_OFF	0x00
#define TIGER_AUX_CTRL		0x02
#define TIGER_AUX_DATA		0x03
#define TIGER_AUX_IRQMASK	0x05
#define TIGER_AUX_STATUS	0x07

/* Tiger AUX BITs */
#define SEDL_AUX_IOMASK		0xdd	/* 1 and 5 are inputs */
#define SEDL_ISAR_RESET_BIT_OFF 0x00
#define SEDL_ISAR_RESET_BIT_ON	0x01
#define SEDL_TIGER_IRQ_BIT	0x02
#define SEDL_ISAR_PCI_LED1_BIT	0x08
#define SEDL_ISAR_PCI_LED2_BIT	0x10

#define SEDL_PCI_RESET_ON	(SEDL_ISAR_RESET_BIT_ON)
#define SEDL_PCI_RESET_OFF	(SEDL_ISAR_PCI_LED1_BIT | SEDL_ISAR_PCI_LED2_BIT)


#define SEDL_RESET      0x3	/* same as DOS driver */

/* data struct */
// #define SPIN_DEBUG
typedef struct _sedl_fax {
	struct _sedl_fax	*prev;
	struct _sedl_fax	*next;
	u_char			subtyp;
	u_int			irq;
	u_int			cfg;
	u_int			addr;
	u_int			isac;
	u_int			isar;
	spinlock_t		devlock;
	u_long			flags;
#ifdef SPIN_DEBUG
	void			*lock_adr;
#endif
	volatile u_int		in_irq;
	struct isar_reg		ir;
	dchannel_t		dch;
	bchannel_t		bch[2];
} sedl_fax;

static inline u_char
readreg(unsigned int ale, unsigned int adr, u_char off)
{
	byteout(ale, off);
	return (bytein(adr));
}

static inline void
readfifo(unsigned int ale, unsigned int adr, u_char off, u_char * data, int size)
{
	byteout(ale, off);
	insb(adr, data, size);
}


static inline void
writereg(unsigned int ale, unsigned int adr, u_char off, u_char data)
{
	byteout(ale, off);
	byteout(adr, data);
}

static inline void
writefifo(unsigned int ale, unsigned int adr, u_char off, u_char * data, int size)
{
	byteout(ale, off);
	outsb(adr, data, size);
}

/* Interface functions */

static u_char
ReadISAC(void *p, u_char offset)
{
	return (readreg(((sedl_fax *)p)->addr, ((sedl_fax *)p)->isac, offset));
}

static void
WriteISAC(void *p, u_char offset, u_char value)
{
	writereg(((sedl_fax *)p)->addr, ((sedl_fax *)p)->isac, offset, value);
}

static void
ReadISACfifo(void *p, u_char * data, int size)
{
	readfifo(((sedl_fax *)p)->addr, ((sedl_fax *)p)->isac, 0, data, size);
}

static void
WriteISACfifo(void *p, u_char * data, int size)
{
	writefifo(((sedl_fax *)p)->addr, ((sedl_fax *)p)->isac, 0, data, size);
}

/* ISAR access routines
 * mode = 0 access with IRQ on
 * mode = 1 access with IRQ off
 * mode = 2 access with IRQ off and using last offset
 */
  
static u_char
ReadISAR(void *p, int mode, u_char offset)
{	
	if (mode == 0)
		return (readreg(((sedl_fax *)p)->addr, ((sedl_fax *)p)->isar, offset));
	else if (mode == 1)
		byteout(((sedl_fax *)p)->addr, offset);
	return(bytein(((sedl_fax *)p)->isar));
}

static void
WriteISAR(void *p, int mode, u_char offset, u_char value)
{
	if (mode == 0)
		writereg(((sedl_fax *)p)->addr, ((sedl_fax *)p)->isar, offset, value);
	else {
		if (mode == 1)
			byteout(((sedl_fax *)p)->addr, offset);
		byteout(((sedl_fax *)p)->isar, value);
	}
}

static void lock_dev(void *data)
{
	register u_long	flags;
	register sedl_fax *sf = (sedl_fax *)data;

	spin_lock_irqsave(&sf->devlock, flags);
	sf->flags = flags;
#ifdef SPIN_DEBUG
	sf->lock_adr = __builtin_return_address(0);
#endif
} 

static void unlock_dev(void *data)
{
	register sedl_fax *sf = (sedl_fax *)data;

	spin_unlock_irqrestore(&sf->devlock, sf->flags);
#ifdef SPIN_DEBUG
	sf->lock_adr = NULL;
#endif
}

inline void
do_sedl_interrupt(sedl_fax *sf)
{
	u_char val;
	int cnt = 8;

	lock_dev(sf);
	if (sf->in_irq) {
		unlock_dev(sf);
		printk(KERN_WARNING "sedl_interrupt while in irq\n");
		return;
	}
	sf->in_irq = 1;
	val = readreg(sf->addr, sf->isar, ISAR_IRQBIT);
      Start_ISAR:
	if (val & ISAR_IRQSTA)
		isar_int_main(&sf->bch[0]);
	val = readreg(sf->addr, sf->isac, ISAC_ISTA);
      Start_ISAC:
	if (val)
		isac_interrupt(&sf->dch, val);
	val = readreg(sf->addr, sf->isar, ISAR_IRQBIT);
	if ((val & ISAR_IRQSTA) && cnt) {
		cnt--;
		if (sf->dch.debug & L1_DEB_HSCX)
			printk(KERN_DEBUG "ISAR IntStat after IntRoutine cpu%d\n",
				smp_processor_id());
		goto Start_ISAR;
	}
	val = readreg(sf->addr, sf->isac, ISAC_ISTA);
	if (val && cnt) {
		cnt--;
		if (sf->dch.debug & L1_DEB_ISAC)
			printk(KERN_DEBUG "ISAC IntStat after IntRoutine cpu%d\n",
				smp_processor_id());
		goto Start_ISAC;
	}
	if (!cnt)
		if (sf->dch.debug & L1_DEB_ISAC)
			printk(KERN_DEBUG "Sedlbauer IRQ LOOP\n");
	sf->in_irq = 0;
	writereg(sf->addr, sf->isar, ISAR_IRQBIT, 0);
	writereg(sf->addr, sf->isac, ISAC_MASK, 0xFF);
	writereg(sf->addr, sf->isac, ISAC_MASK, 0x0);
	writereg(sf->addr, sf->isar, ISAR_IRQBIT, ISAR_IRQMSK);
	unlock_dev(sf);
}

static void
speedfax_isa_interrupt(int intno, void *dev_id, struct pt_regs *regs)
{
	sedl_fax *sf = dev_id;

	if (!sf) {
		printk(KERN_WARNING "Sedlbauer: Spurious ISA interrupt!\n");
		return;
	}
	do_sedl_interrupt(sf);
}

static void
speedfax_pci_interrupt(int intno, void *dev_id, struct pt_regs *regs)
{
	sedl_fax *sf = dev_id;
	u_char val;

	if (!sf) {
		printk(KERN_WARNING "Sedlbauer: Spurious PCI interrupt!\n");
		return;
	}
	val = bytein(sf->cfg + TIGER_AUX_STATUS);
	if (val & SEDL_TIGER_IRQ_BIT) /* for us or shared ? */
		return; /* shared */
	do_sedl_interrupt(sf);
}

void
release_sedlbauer(sedl_fax *sf)
{
	int bytecnt = 256;

	if (sf->subtyp == SEDL_SPEEDFAX_ISA)
		bytecnt = 16;
	if (sf->cfg)
		release_region(sf->cfg, bytecnt);
}

static void
reset_speedfax(sedl_fax *sf)
{

	printk(KERN_INFO "Sedlbauer: resetting card\n");

	if (sf->subtyp == SEDL_SPEEDFAX_ISA) {
		byteout(sf->cfg + SEDL_ISA_RESET_ON, SEDL_RESET);
		mdelay(1);
		byteout(sf->cfg + SEDL_ISA_RESET_OFF, 0);
		mdelay(1);
	} else {
		byteout(sf->cfg + TIGER_RESET_ADDR, TIGER_EXTERN_RESET_ON);
		byteout(sf->cfg + TIGER_AUX_DATA, SEDL_PCI_RESET_ON);
		mdelay(1);
		byteout(sf->cfg + TIGER_RESET_ADDR, TIGER_EXTERN_RESET_OFF);
		byteout(sf->cfg + TIGER_AUX_DATA, SEDL_PCI_RESET_OFF);
		mdelay(1);
	}
}

static int init_card(sedl_fax *sf)
{
	int irq_cnt, cnt = 3;
	long flags;
	u_int shared = SA_SHIRQ;
	void *irq_func = speedfax_pci_interrupt;

	if (sf->subtyp == SEDL_SPEEDFAX_ISA) {
		irq_func = speedfax_isa_interrupt;
		shared = 0;
	}
	save_flags(flags);
	irq_cnt = kstat_irqs(sf->irq);
	printk(KERN_INFO "%s: IRQ %d count %d cpu%d\n",
		sf->dch.inst.name, sf->irq, irq_cnt, smp_processor_id());
	lock_dev(sf);
	sf->in_irq = 1;
	if (request_irq(sf->irq, irq_func, shared, "speedfax", sf)) {
		printk(KERN_WARNING "HiSax: couldn't get interrupt %d\n",
			sf->irq);
		unlock_dev(sf);
		return(-EIO);
	}
	while (cnt) {
		clear_pending_isac_ints(&sf->dch);
		init_isac(&sf->dch);
		init_isar(&sf->bch[0]);
		init_isar(&sf->bch[1]);
		sf->in_irq = 0;
		WriteISAC(sf, ISAC_MASK, 0);
		/* RESET Receiver and Transmitter */
		WriteISAC(sf, ISAC_CMDR, 0x41);
		unlock_dev(sf);
		sti();
		current->state = TASK_UNINTERRUPTIBLE;
		/* Timeout 10ms */
		schedule_timeout((10*HZ)/1000);
		restore_flags(flags);
		printk(KERN_INFO "%s: IRQ %d count %d cpu%d\n",
			sf->dch.inst.name, sf->irq, kstat_irqs(sf->irq), smp_processor_id());
		if (kstat_irqs(sf->irq) == irq_cnt) {
			printk(KERN_WARNING
			       "Sedlbauer speedfax: IRQ(%d) getting no interrupts during init %d\n",
			       sf->irq, 4 - cnt);
			if (cnt == 1) {
				return (-EIO);
			} else {
				reset_speedfax(sf);
				cnt--;
			}
		} else {
			return(0);
		}
		lock_dev(sf);
	}
	unlock_dev(sf);
	return(-EIO);
}


#define MAX_CARDS	4
#define MODULE_PARM_T	"1-4i"
static int sedl_cnt;
static hisaxobject_t	speedfax;
static int debug;
static u_int protocol[MAX_CARDS];
static u_int io[MAX_CARDS];
static u_int irq[MAX_CARDS];
static int layermask[MAX_CARDS];
static int cfg_idx;

#ifdef MODULE
MODULE_AUTHOR("Karsten Keil");
MODULE_PARM(debug, "1i");
MODULE_PARM(io, MODULE_PARM_T);
MODULE_PARM(protocol, MODULE_PARM_T);
MODULE_PARM(irq, MODULE_PARM_T);
MODULE_PARM(layermask, MODULE_PARM_T);
#define Speedfax_init init_module
#endif

static char SpeedfaxName[] = "Speedfax";

static	struct pci_dev *dev_sedl;
static	int pci_finished_lookup;

int
setup_speedfax(sedl_fax *sf, u_int io_cfg, u_int irq_cfg)
{
	int bytecnt, ver;
	char tmp[64];
	u16 sub_vendor_id, sub_id;

	strcpy(tmp, Sedlfax_revision);
	printk(KERN_INFO "HiSax: Sedlbauer speedfax driver Rev. %s\n", HiSax_getrev(tmp));
	
 	sf->subtyp = 0;
	bytecnt = 16;
/* Probe for Sedlbauer speedfax pci */
#if CONFIG_PCI
	if (pci_present() && !pci_finished_lookup) {
		while ((dev_sedl = pci_find_device(PCI_VENDOR_ID_TIGERJET,
				PCI_DEVICE_ID_TIGERJET_100, dev_sedl))) {
			sf->irq = dev_sedl->irq;
			sf->cfg = dev_sedl->base_address[ 0] & PCI_BASE_ADDRESS_IO_MASK;
			pci_read_config_word(dev_sedl, PCI_SUBSYSTEM_VENDOR_ID, &sub_vendor_id);
			pci_read_config_word(dev_sedl, PCI_SUBSYSTEM_ID, &sub_id);
			printk(KERN_INFO "Sedlbauer: PCI subvendor:%x subid %x\n",
				sub_vendor_id, sub_id);
			printk(KERN_INFO "Sedlbauer: PCI base adr %#x\n",
				sf->cfg);
			if (sub_id != PCI_SUB_ID_SEDLBAUER) {
				printk(KERN_WARNING "Sedlbauer: unknown sub id %#x\n", sub_id);
				continue;
			}
			if (!sf->irq) {
				printk(KERN_WARNING "Sedlbauer: No IRQ for PCI card found\n");
				continue;
			}
			if (sub_vendor_id == PCI_SUBVENDOR_SPEEDFAX_PYRAMID) {
				sf->subtyp = SEDL_SPEEDFAX_PYRAMID;
			} else if (sub_vendor_id == PCI_SUBVENDOR_SPEEDFAX_PCI) {
				sf->subtyp = SEDL_SPEEDFAX_PCI;
			} else {
				printk(KERN_WARNING "Sedlbauer: unknown sub vendor id %#x\n",
					sub_vendor_id);
				continue;
			}
			if (pci_enable_device(dev_sedl))
				continue;
			bytecnt = 256;
			break;
	 	}
	 	if (!dev_sedl) {
	 		pci_finished_lookup = 1;
			printk(KERN_INFO "Sedlbauer: No more PCI cards found\n");
		}
#else
		printk(KERN_WARNING "Sedlbauer: NO_PCI_BIOS\n");
#endif /* CONFIG_PCI */
	}
	if (!sf->subtyp) { /* OK no PCI found now check for an ISA card */	
		if ((!io_cfg) || (!irq_cfg)) {
			if (!sedl_cnt)
				printk(KERN_WARNING
					"Sedlbauer: No io/irq for ISA card\n");
			return(1);
		}
		sf->cfg = io_cfg;
		sf->irq = irq_cfg;
		sf->subtyp = SEDL_SPEEDFAX_ISA;
		bytecnt = 16;
	}
	
	if (check_region(sf->cfg, bytecnt)) {
		printk(KERN_WARNING
			"HiSax: %s config port %x-%x already in use\n",
			 Sedlbauer_Types[sf->subtyp],
			sf->cfg,
			sf->cfg + bytecnt);
			return (1);
	} else {
		request_region(sf->cfg, bytecnt, "sedlbauer speedfax+");
	}

	printk(KERN_INFO
	       "Sedlbauer: defined at 0x%x-0x%x IRQ %d\n",
	       sf->cfg,
	       sf->cfg + bytecnt,
	       sf->irq);

	printk(KERN_INFO "Sedlbauer: %s detected\n",
		Sedlbauer_Types[sf->subtyp]);

	sf->dch.readisac = &ReadISAC;
	sf->dch.writeisac = &WriteISAC;
	sf->dch.readisacfifo = &ReadISACfifo;
	sf->dch.writeisacfifo = &WriteISACfifo;
	if (sf->subtyp != SEDL_SPEEDFAX_ISA) {
		sf->addr = sf->cfg + SEDL_PCI_ADR;
		sf->isac = sf->cfg + SEDL_PCI_ISAC;
		sf->isar = sf->cfg + SEDL_PCI_ISAR;
		byteout(sf->cfg + TIGER_RESET_ADDR, 0xff);
		mdelay(1);
		byteout(sf->cfg + TIGER_RESET_ADDR, 0x00);
		mdelay(1);
		byteout(sf->cfg + TIGER_AUX_CTRL, SEDL_AUX_IOMASK);
		byteout(sf->cfg + TIGER_AUX_IRQMASK, SEDL_TIGER_IRQ_BIT);
		byteout(sf->cfg + TIGER_AUX_DATA, SEDL_PCI_RESET_ON);
		mdelay(1);
		byteout(sf->cfg + TIGER_AUX_DATA, SEDL_PCI_RESET_OFF);
		mdelay(1);
	} else {
		sf->addr = sf->cfg + SEDL_ISA_ADR;
		sf->isac = sf->cfg + SEDL_ISA_ISAC;
		sf->isar = sf->cfg + SEDL_ISA_ISAR;
	}
	sf->bch[0].hw.isar.reg = &sf->ir;
	sf->bch[1].hw.isar.reg = &sf->ir;
	lock_dev(sf);
#ifdef SPIN_DEBUG
	printk(KERN_ERR "lock_adr=%p now(%p)\n", &sf->lock_adr, sf->lock_adr);
#endif
	ISACVersion(&sf->dch, "Sedlbauer:");
	unlock_dev(sf);

	sf->bch[0].BC_Read_Reg = &ReadISAR;
	sf->bch[0].BC_Write_Reg = &WriteISAR;
	sf->bch[1].BC_Read_Reg = &ReadISAR;
	sf->bch[1].BC_Write_Reg = &WriteISAR;
	lock_dev(sf);
	ver = ISARVersion(&sf->bch[0], "Sedlbauer:");
	unlock_dev(sf);
	if (ver < 0) {
		printk(KERN_WARNING
			"Sedlbauer: wrong ISAR version (ret = %d)\n", ver);
		release_sedlbauer(sf);
		return (1);
	}
	return (0);
}

static void
release_card(sedl_fax *card) {
	lock_dev(card);
	card->in_irq = 1;
	free_irq(card->irq, card);
	free_isar(&card->bch[1]);
	free_isar(&card->bch[0]);
	free_isac(&card->dch);
	WriteISAR(card, 0, ISAR_IRQBIT, 0);
	WriteISAC(card, ISAC_MASK, 0xFF);
	reset_speedfax(card);
	WriteISAR(card, 0, ISAR_IRQBIT, 0);
	WriteISAC(card, ISAC_MASK, 0xFF);
	release_sedlbauer(card);
	free_bchannel(&card->bch[1]);
	free_bchannel(&card->bch[0]);
	free_dchannel(&card->dch);
	REMOVE_FROM_LISTBASE(card, ((sedl_fax *)speedfax.ilist));
	unlock_dev(card);
	kfree(card);
	sedl_cnt--;
	speedfax.refcnt--;
}

static int
speedfax_manager(void *data, u_int prim, void *arg) {
	sedl_fax *card = speedfax.ilist;
	hisaxinstance_t *inst=data;
	int channel = -1;
	int val;

	printk(KERN_DEBUG __FUNCTION__": data:%p prim:%x arg:%p\n", data, prim, arg);
	if (!data) {
		printk(KERN_ERR "speedfax_manager no data prim %x arg %p\n",
			prim, arg);
		return(-EINVAL);
	}
	while(card) {
		if (&card->dch.inst == inst) {
			channel = 2;
			break;
		}
		if (&card->bch[0].inst == inst) {
			channel = 0;
			break;
		}
		if (&card->bch[1].inst == inst) {
			channel = 1;
			break;
		}
		card = card->next;
	}
	if (channel<0) {
		printk(KERN_ERR "speedfax_manager no channel data %p prim %x arg %p\n",
			data, prim, arg);
		return(-EINVAL);
	}
	switch(prim) {
	    case MGR_REGLAYER | CONFIRM:
		if (!card) {
			printk(KERN_WARNING "speedfax_manager no card found\n");
			return(-ENODEV);
		}
		break;
	    case MGR_UNREGLAYER | REQUEST:
		if (!card) {
			printk(KERN_WARNING "speedfax_manager no card found\n");
			return(-ENODEV);
		} else {
			if (channel == 2) {
				val = HW_DEACTIVATE;
				inst->down.fdata = &card->dch;
				ISAC_l1hw(&inst->down, PH_CONTROL | REQUEST, 0,
					4, &val);
			} else {
				inst->down.fdata = &card->bch[channel];
				isar_down(&inst->down, MGR_DISCONNECT | REQUEST,
					0, 0, NULL);
				
			}
			speedfax.ctrl(inst->up.peer, MGR_DISCONNECT | REQUEST,
				&inst->up);
			speedfax.ctrl(inst, MGR_UNREGLAYER | REQUEST, NULL);
		}
		break;
	    case MGR_RELEASE | INDICATION:
		if (!card) {
			printk(KERN_WARNING "speedfax_manager no card found\n");
			return(-ENODEV);
		} else {
			if (channel == 2) {
				release_card(card);
			} else {
				speedfax.refcnt--;
			}
		}
		break;
	    case MGR_CONNECT | REQUEST:
		if (!card) {
			printk(KERN_WARNING __FUNCTION__": connect request failed\n");
			return(-ENODEV);
		}
		return(ConnectIF(inst, arg));
		break;
	    case MGR_SETIF | REQUEST:
	    case MGR_SETIF | INDICATION:
		if (!card) {
			printk(KERN_WARNING __FUNCTION__": setif failed\n");
			return(-ENODEV);
		}
		if (channel==2)
			return(SetIF(inst, arg, prim, ISAC_l1hw, NULL,
				&card->dch));
		else
			return(SetIF(inst, arg, prim, isar_down, NULL,
				&card->bch[channel]));
	    case MGR_DISCONNECT | REQUEST:
	    case MGR_DISCONNECT | INDICATION:
		if (!card) {
			printk(KERN_WARNING "speedfax_manager del interface request failed\n");
			return(-ENODEV);
		}
		return(DisConnectIF(inst, arg));
		break;
	    case MGR_LOADFIRM | REQUEST:
		if (!card) {
			printk(KERN_WARNING "speedfax_manager MGR_LOADFIRM no card\n");
			return(-ENODEV);
		} else {
			struct firm {
				int	len;
				void	*data;
			} *firm = arg;
			
			if (!arg)
				return(-EINVAL);
			return(isar_load_firmware(&card->bch[0], firm->data, firm->len));
		}
	    case MGR_SETSTACK | CONFIRM:
		if (!card) {
			printk(KERN_WARNING __FUNCTION__": setstack failed\n");
			return(-ENODEV);
		}
		if ((channel!=2) && (inst->pid.global == 2)) {
			inst->down.fdata = &card->bch[channel];
			isar_down(&inst->down, PH_ACTIVATE | REQUEST,
				0, 0, NULL);
			if (inst->pid.protocol[2] == ISDN_PID_L2_B_TRANS)
				val = DL_ESTABLISH;
			else
				val = PH_ACTIVATE;
			if (inst->up.func)
				inst->up.func(&inst->up, val | INDICATION,
					0, 0, NULL);
		}
		break;
	    default:
		printk(KERN_WARNING "speedfax_manager prim %x not handled\n", prim);
		return(-EINVAL);
	}
	return(0);
}

int
Speedfax_init(void)
{
	int err,i;
	sedl_fax *card;
	hisax_pid_t pid;

	speedfax.name = SpeedfaxName;
	speedfax.own_ctrl = speedfax_manager;
	speedfax.DPROTO.protocol[0] = ISDN_PID_L0_TE_S0;
	speedfax.BPROTO.protocol[1] = ISDN_PID_L1_B_64TRANS |
				      ISDN_PID_L1_B_TRANS_TT |
				      ISDN_PID_L1_B_TRANS_TTR |
				      ISDN_PID_L1_B_TRANS_TTS |
				      ISDN_PID_L1_B_64HDLC;
	speedfax.BPROTO.protocol[2] = ISDN_PID_L2_B_TRANS;
	speedfax.prev = NULL;
	speedfax.next = NULL;
	
	if ((err = HiSax_register(&speedfax))) {
		printk(KERN_ERR "Can't register Speedfax error(%d)\n", err);
		return(err);
	}
	while (sedl_cnt < MAX_CARDS) {
		if (!(card = kmalloc(sizeof(sedl_fax), GFP_ATOMIC))) {
			printk(KERN_ERR "No kmem for card\n");
			HiSax_unregister(&speedfax);
			return(-ENOMEM);
		}
		memset(card, 0, sizeof(sedl_fax));
		APPEND_TO_LIST(card, ((sedl_fax *)speedfax.ilist));
		card->dch.debug = debug;
		card->dch.inst.obj = &speedfax;
		spin_lock_init(&card->devlock);
		card->dch.inst.lock = lock_dev;
		card->dch.inst.unlock = unlock_dev;
		card->dch.inst.data = card;
		card->dch.inst.up.owner = &card->dch.inst;
		card->dch.inst.down.owner = &card->dch.inst;
		speedfax.ctrl(NULL, MGR_DISCONNECT | REQUEST,
			&card->dch.inst.down);
		set_dchannel_pid(&pid, protocol[sedl_cnt], layermask[sedl_cnt]);
		sprintf(card->dch.inst.name, "SFax%d", sedl_cnt+1);
		init_dchannel(&card->dch);
		for (i=0; i<2; i++) {
			card->bch[i].channel = i;
			card->bch[i].inst.obj = &speedfax;
			card->bch[i].inst.data = card;
			card->bch[i].inst.pid.layermask = 0;
			card->bch[i].inst.up.owner = &card->bch[i].inst;
			card->bch[i].inst.down.owner = &card->bch[i].inst;
			card->bch[i].inst.down.fdata = &card->bch[i];
			speedfax.ctrl(NULL, MGR_DISCONNECT | REQUEST,
				&card->bch[i].inst.down);
			card->bch[i].inst.lock = lock_dev;
			card->bch[i].inst.unlock = unlock_dev;
			card->bch[i].debug = debug;
			sprintf(card->bch[i].inst.name, "%s B%d",
				card->dch.inst.name, i+1);
			init_bchannel(&card->bch[i]);
		}
		printk(KERN_DEBUG "sfax card %p dch %p bch1 %p bch2 %p\n",
			card, &card->dch, &card->bch[0], &card->bch[1]);
		if (setup_speedfax(card, io[cfg_idx], irq[cfg_idx])) {
			err = 0;
			free_dchannel(&card->dch);
			free_bchannel(&card->bch[1]);
			free_bchannel(&card->bch[0]);
			REMOVE_FROM_LISTBASE(card, ((sedl_fax *)speedfax.ilist));
			kfree(card);
			card = NULL;
			if (!sedl_cnt) {
				HiSax_unregister(&speedfax);
				err = -ENODEV;
			} else
				printk(KERN_INFO "sedlfax %d cards installed\n",
					sedl_cnt);
			return(err);
		}
		if (card->subtyp == SEDL_SPEEDFAX_ISA)
			cfg_idx++;
		sedl_cnt++;
		if ((err = speedfax.ctrl(NULL, MGR_NEWSTACK | REQUEST, &card->dch.inst))) {
			printk(KERN_ERR  "MGR_ADDSTACK REQUEST dch err(%d)\n", err);
			release_card(card);
			if (!sedl_cnt)
				HiSax_unregister(&speedfax);
			else
				err = 0;
			return(err);
		}
		if ((err = speedfax.ctrl(card->dch.inst.st, MGR_SETSTACK | REQUEST, &pid))) {
			printk(KERN_ERR  "MGR_SETSTACK REQUEST dch err(%d)\n", err);
			speedfax.ctrl(card->dch.inst.st, MGR_DELSTACK | REQUEST, NULL);
			if (!sedl_cnt)
				HiSax_unregister(&speedfax);
			else
				err = 0;
			return(err);
		}
		for (i=0; i<2; i++) {
			if ((err = speedfax.ctrl(card->dch.inst.st,
				MGR_NEWSTACK | REQUEST, &card->bch[i].inst))) {
				printk(KERN_ERR "MGR_ADDSTACK bchan error %d\n", err);
				speedfax.ctrl(card->dch.inst.st, MGR_DELSTACK | REQUEST, NULL);
				if (!sedl_cnt)
					HiSax_unregister(&speedfax);
				else
					err = 0;
				return(err);
			}
		}
		if ((err = init_card(card))) {
			speedfax.ctrl(card->dch.inst.st, MGR_DELSTACK | REQUEST, NULL);
			if (!sedl_cnt)
				HiSax_unregister(&speedfax);
			else
				err = 0;
			return(err);
		}
	}
	printk(KERN_INFO "sedlfax %d cards installed\n", sedl_cnt);
	return(0);
}

#ifdef MODULE
int
cleanup_module(void)
{
	int err;
	if ((err = HiSax_unregister(&speedfax))) {
		printk(KERN_ERR "Can't unregister Speedfax PCI error(%d)\n", err);
		return(err);
	}
	while(speedfax.ilist) {
		printk(KERN_ERR "Speedfax PCI card struct not empty refs %d\n",
			speedfax.refcnt);
		release_card(speedfax.ilist);
	}
	return(0);
}
#endif
