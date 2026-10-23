 /*
  * UAE - The Un*x Amiga Emulator
  *
  * Gayle (A600/A1200) and A4000 IDE controller emulation
  *
  * This is a reduced C port of WinUAE's gayle.cpp and ide.cpp
  * (c) 2006 - 2015 Toni Wilen.
  *
  * Differences to WinUAE:
  *  - one IDE channel with up to two ATA hard disks (master/slave),
  *    backed by plain host files through hdf_open()/hdf_read()/hdf_write()
  *    in hardfile_unix.c
  *  - host I/O is done synchronously, no IDE thread
  *  - v16: an ATAPI CD-ROM drive (the CD image drive of blkdev-cdimage.c)
  *    can take the place of master or slave; PIO only
  *  - no PCMCIA card emulation (the PCMCIA status registers
  *    report an empty slot), no IDE splitter, no savestates
  *  - IDE_A4000 shares the IDE core and is mapped at $DD2000
  *
  * Configuration (see cfgfile.c):
  *   ide=none|a600/a1200|a4000
  *   ide0_hardfile=<path>   ide0_readonly=yes|no     (master)
  *   ide1_hardfile=<path>   ide1_readonly=yes|no     (slave)
  *   cdimage0=<image>  cdimage0_controller=ide0|ide1  (v16: ATAPI CD-ROM)
  */

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "memory.h"
#include "custom.h"
#include "newcpu.h"
#include "filesys.h"
#include "gui.h"
#include "gayle.h"
#ifdef SCSIEMU
#include "cdimage.h"
#endif

#define GAYLE_LOG 0
#define IDE_LOG 0

#ifndef FILESYS
/* Without hardfile support there is nothing to attach to the controller;
 * the registers are still emulated (no drives found). */
#define hdf_open(hfd, name) 0
#define hdf_close(hfd) do { } while (0)
#define hdf_read(hfd, buf, off, len) (-1)
#define hdf_write(hfd, buf, off, len) (-1)
#endif

/* IDE register numbers (after address decoding) */
#define IDE_DATA	0x00
#define IDE_ERROR	0x01
#define IDE_NSECTOR	0x02
#define IDE_SECTOR	0x03
#define IDE_LCYL	0x04
#define IDE_HCYL	0x05
#define IDE_SELECT	0x06
#define IDE_STATUS	0x07
#define IDE_SECONDARY	0x0400
#define IDE_DEVCON	0x0406
#define IDE_DRVADDR	0x0407

/* STATUS bits */
#define IDE_STATUS_ERR	0x01
#define IDE_STATUS_DRQ	0x08
#define IDE_STATUS_DSC	0x10
#define IDE_STATUS_DRDY	0x40
#define IDE_STATUS_BSY	0x80
/* ERROR bits */
#define IDE_ERR_IDNF	0x10
#define IDE_ERR_ABRT	0x04

/* Gayle register offsets inside the $DAxxxx / $DD3xxx area */
#define GAYLE_IRQ_4000	0x3020	/* A4000: WORD, MSB = IDE interrupt */
#define GAYLE_CS_1200	0x8000
#define GAYLE_IRQ_1200	0x9000
#define GAYLE_INT_1200	0xA000
#define GAYLE_CFG_1200	0xB000

#define GAYLE_CS_CCDET	0x40
#define GAYLE_CS_DIS	0x01

#define GAYLE_IRQ_IDE	0x80
#define GAYLE_IRQ_CCDET	0x40
#define GAYLE_IRQ_BVD1	0x20
#define GAYLE_IRQ_BVD2	0x10
#define GAYLE_IRQ_WR	0x08
#define GAYLE_IRQ_BSY	0x04
#define GAYLE_IRQ_RESET	0x02
#define GAYLE_IRQ_BERR	0x01

#define GAYLE_INT_BVD_LEV	0x02
#define GAYLE_INT_BSY_LEV	0x01

#define INTF_PORTS	0x0008
#define INTF_EXTER	0x2000

struct ide_regs {
    uae_u8 select, nsector, sector, lcyl, hcyl, devcon, error, feat;
    uae_u8 nsector2, sector2, lcyl2, hcyl2, feat2;
    uae_u8 status;
};

struct ide_drive {
    struct hardfiledata hfd;
    int present;		/* host file opened */
    struct ide_regs regs;
    struct ide_regs *regs0;	/* master registers */
    struct ide_regs *regs1;	/* slave registers */
    struct ide_drive *pair;
    uae_u8 *secbuf;
    int secbuf_size;
    int buffer_offset;
    int data_offset;
    int data_size;
    int data_multi;
    int direction;		/* 0 = read, 1 = write, 2 = format */
    int intdrq;
    int lba, lba48, lba48cmd;
    uae_u64 start_lba, max_lba;
    int start_nsec;
    int max_multiple_mode;
    uae_u8 multiple_mode;
    int irq_delay;
    int irq;
    int irq_new;
    int num;
    int blocksize;
    int maxtransferstate;
    int ata_level;
    int ide_drv;
    uae_u64 size;
    unsigned int cyls, heads, secs;
    unsigned int cyls_def, heads_def, secs_def;
    /* v16: ATAPI CD-ROM drive */
    int atapi;			/* this drive is the ATAPI CD-ROM */
    int atapi_state;		/* ATAPI_* */
    uae_u8 atapi_packet[12];
    int atapi_total;		/* data-in bytes of the command */
    int atapi_pos;		/* bytes handed over so far */
    int atapi_chunk;		/* bytes of the current DRQ block */
    int atapi_drdy;		/* DRDY: 0 after reset until IDENTIFY PACKET / PACKET */
};

#define ATAPI_IDLE    0
#define ATAPI_PACKET  1		/* waiting for the 12 command bytes */
#define ATAPI_DATAIN  2		/* device -> host */
#define ATAPI_DATAOUT 3		/* host -> device (MODE SELECT) */

/* ATAPI interrupt reason (sector count register) */
#define ATAPI_IR_COD  0x01
#define ATAPI_IR_IO   0x02

static struct ide_drive idedrive[2];

static int gayle_id_cnt;
static uae_u8 gayle_irq, gayle_int, gayle_cs, gayle_cs_mask, gayle_cfg;

/* ------------------------------------------------------------------ */
/* IDE core                                                           */
/* ------------------------------------------------------------------ */

static int ide_isdrive (struct ide_drive *ide)
{
    return ide && ide->present;
}

static void ide_grow_buffer (struct ide_drive *ide, int newsize)
{
    uae_u8 *nb;
    if (ide->secbuf_size >= newsize)
	return;
    nb = (uae_u8 *) realloc (ide->secbuf, newsize + 16384);
    if (!nb) {
	write_log ("IDE%d: out of memory (%d bytes)\n", ide->num, newsize);
	abort ();
    }
    memset (nb + ide->secbuf_size, 0, newsize + 16384 - ide->secbuf_size);
    ide->secbuf = nb;
    ide->secbuf_size = newsize + 16384;
}

/* Interrupts are delayed by a few hsyncs, status is built in
 * ide_interrupt_do() */
static void ide_arm_interrupt (struct ide_drive *ide, int delay)
{
    ide->regs.status |= IDE_STATUS_BSY;
    ide->regs.status &= ~IDE_STATUS_DRQ;
    ide->irq_delay = delay;
}

static void ide_interrupt (struct ide_drive *ide)
{
    ide_arm_interrupt (ide, 2);
}

static void ide_fast_interrupt (struct ide_drive *ide)
{
    ide_arm_interrupt (ide, 1);
}

static int ide_interrupt_do (struct ide_drive *ide)
{
    ide->regs.status &= ~IDE_STATUS_DRQ;
    if (ide->intdrq)
	ide->regs.status |= IDE_STATUS_DRQ;
    ide->regs.status &= ~IDE_STATUS_BSY;
    ide->intdrq = 0;
    ide->irq_delay = 0;
    if (ide->regs.devcon & 2)	/* nIEN */
	return 0;
    ide->irq_new = 1;
    ide->irq = 1;
    return 1;
}

static void ide_fail_err (struct ide_drive *ide, uae_u8 err)
{
    ide->regs.error |= err;
    if (ide->ide_drv == 1 && !ide_isdrive (ide->pair))
	ide->pair->regs.status |= IDE_STATUS_ERR;
    ide->regs.status |= IDE_STATUS_ERR;
    ide->intdrq = 0;
    ide->data_multi = 0;
    ide_interrupt (ide);
}

static void ide_fail (struct ide_drive *ide)
{
    ide_fail_err (ide, IDE_ERR_ABRT);
}

static void ide_data_ready (struct ide_drive *ide)
{
    memset (ide->secbuf, 0, ide->blocksize);
    ide->data_offset = 0;
    ide->data_size = ide->blocksize;
    ide->buffer_offset = 0;
    ide->data_multi = 1;
    ide->intdrq = 1;
    ide_interrupt (ide);
}

static void ide_recalibrate (struct ide_drive *ide)
{
    ide->regs.sector = 0;
    ide->regs.lcyl = ide->regs.hcyl = 0;
    ide_interrupt (ide);
}

/* IDENTIFY DEVICE data. The buffer is in "Amiga bus order": identify
 * words are stored little endian, the 68k reads them byteswapped and
 * scsi.device swaps them back. */
static void pw (struct ide_drive *ide, int offset, uae_u16 w)
{
    ide->secbuf[offset * 2 + 0] = (uae_u8) w;
    ide->secbuf[offset * 2 + 1] = w >> 8;
}

static void pl (struct ide_drive *ide, int offset, uae_u32 l)
{
    ide->secbuf[offset * 2 + 0] = l;
    ide->secbuf[offset * 2 + 1] = l >> 8;
    ide->secbuf[offset * 2 + 2] = l >> 16;
    ide->secbuf[offset * 2 + 3] = l >> 24;
}

static void pq (struct ide_drive *ide, int offset, uae_u64 q)
{
    pl (ide, offset + 0, (uae_u32) (q >> 0));
    pl (ide, offset + 2, (uae_u32) (q >> 32));
}

static void ps (struct ide_drive *ide, int offset, const char *src, int max)
{
    int i, len = strlen (src);

    for (i = 0; i < max; i += 2) {
	char c1 = ' ', c2 = ' ';
	if (i < len)
	    c1 = src[i];
	if (i + 1 < len)
	    c2 = src[i + 1];
	ide->secbuf[offset * 2 + 0] = c2;
	ide->secbuf[offset * 2 + 1] = c1;
	offset++;
    }
}

static void ide_identity_buffer (struct ide_drive *ide)
{
    char tmp[100];
    uae_u64 totalsecs;

    memset (ide->secbuf, 0, 512);

    pw (ide, 0, 1 << 6);
    pw (ide, 1, ide->cyls_def);
    pw (ide, 2, 0xc837);
    pw (ide, 3, ide->heads_def);
    pw (ide, 4, ide->blocksize * ide->secs_def);
    pw (ide, 5, ide->blocksize);
    pw (ide, 6, ide->secs_def);
    ps (ide, 10, "68000", 20);		/* serial */
    pw (ide, 20, 3);
    pw (ide, 21, ide->blocksize);
    pw (ide, 22, 4);
    ps (ide, 23, "0.7", 8);		/* firmware revision */
    sprintf (tmp, "UAE-IDE %s", ide->hfd.product_id);
    ps (ide, 27, tmp, 40);		/* model */
    pw (ide, 47, ide->max_multiple_mode ? (0x8000 | (ide->max_multiple_mode >> (ide->blocksize / 512 - 1))) : 0);
    pw (ide, 48, 1);
    pw (ide, 49, (ide->lba ? (1 << 9) : 0) | (1 << 8));	/* LBA and DMA supported */
    pw (ide, 51, 0x200);
    pw (ide, 52, 0x200);
    pw (ide, 53, 1 | (ide->lba ? 2 | 4 : 0));
    pw (ide, 54, ide->cyls);
    pw (ide, 55, ide->heads);
    pw (ide, 56, ide->secs);
    totalsecs = (uae_u64) ide->cyls * ide->heads * ide->secs;
    pl (ide, 57, (uae_u32) totalsecs);
    pw (ide, 59, ide->max_multiple_mode ? (0x100 | ide->max_multiple_mode >> (ide->blocksize / 512 - 1)) : 0);
    pw (ide, 62, 0x0f);
    pw (ide, 63, 0x0f);
    if (ide->lba) {
	totalsecs = ide->blocksize ? ide->size / ide->blocksize : 0;
	if (totalsecs > 0x0fffffff)
	    totalsecs = 0x0fffffff;
	pl (ide, 60, (uae_u32) totalsecs);
	if (ide->ata_level > 0) {
	    pw (ide, 64, 0x03);		/* PIO3 and PIO4 */
	    pw (ide, 65, 120);
	    pw (ide, 66, 120);
	    pw (ide, 67, 120);
	    pw (ide, 68, 120);
	    pw (ide, 80, (1 << 1) | (1 << 2) | (1 << 3) | (1 << 4) | (1 << 5) | (1 << 6));
	    pw (ide, 81, 0x1c);
	    pw (ide, 82, 1 << 14);
	    pw (ide, 83, (1 << 14) | (1 << 13) | (1 << 12) | (ide->lba48 ? (1 << 10) : 0));
	    pw (ide, 84, 1 << 14);
	    pw (ide, 85, 1 << 14);
	    pw (ide, 86, (1 << 14) | (1 << 13) | (1 << 12) | (ide->lba48 ? (1 << 10) : 0));
	    pw (ide, 87, 1 << 14);
	    pw (ide, 88, 0x3f);
	    pw (ide, 93, (1 << 14) | (1 << 13) | (1 << 0));
	    if (ide->lba48) {
		totalsecs = ide->size / ide->blocksize;
		pq (ide, 100, totalsecs);
	    }
	}
    }
    if (ide->multiple_mode > 0) {
	ide->secbuf[59 * 2 + 1] |= 0x01;
    }
    /* fixed disk: clear "removable" bit 15 of word 0 */
    ide->secbuf[0 * 2 + 1] &= ~0x80;
}

/* v16: IDENTIFY PACKET DEVICE data of the ATAPI CD-ROM (after ATA/ATAPI-5
 * and WinUAE ide.cpp), same byte order as the ATA identify data */
static void atapi_identity_buffer (struct ide_drive *ide)
{
    memset (ide->secbuf, 0, 512);
    /* ATAPI, CD-ROM (5), removable, DRQ within 50 us, 12 byte packets */
    pw (ide, 0, 0x8000 | (5 << 8) | 0x80 | 0x40);
    ps (ide, 10, "68000", 20);		/* serial */
    ps (ide, 23, "0.16", 8);		/* firmware revision */
    ps (ide, 27, "UAE-ATAPI CD-ROM", 40);	/* model */
    pw (ide, 49, 1 << 9);		/* LBA (no DMA) */
    pw (ide, 51, 0x200);
    pw (ide, 53, 2);			/* words 64-70 valid */
    pw (ide, 64, 0x03);			/* PIO3 and PIO4 */
    pw (ide, 65, 120);
    pw (ide, 66, 120);
    pw (ide, 67, 120);
    pw (ide, 68, 120);
    pw (ide, 71, 30);			/* PACKET to bus release */
    pw (ide, 72, 30);
    pw (ide, 80, (1 << 2) | (1 << 3) | (1 << 4) | (1 << 5));	/* ATA/ATAPI-2..5 */
    pw (ide, 82, (1 << 14) | (1 << 9) | (1 << 4));	/* NOP, DEVICE RESET, PACKET */
    pw (ide, 83, 1 << 14);
    pw (ide, 84, 1 << 14);
    pw (ide, 85, (1 << 14) | (1 << 9) | (1 << 4));
    pw (ide, 87, 1 << 14);
}

static void ide_identify_drive (struct ide_drive *ide)
{
    if (ide->atapi) {
	/* ATAPI devices abort IDENTIFY DEVICE and show their signature,
	 * that is how the driver finds out it is a packet device */
	ide_fail (ide);
	ide->regs.nsector = 1;
	ide->regs.sector = 1;
	ide->regs.lcyl = 0x14;
	ide->regs.hcyl = 0xeb;
	return;
    }
    if (!ide_isdrive (ide) || ide->ata_level < 0) {
	ide_fail (ide);
	return;
    }
    ide_data_ready (ide);
    ide->direction = 0;
    ide_identity_buffer (ide);
}

static void set_signature (struct ide_drive *ide)
{
    ide->regs.nsector = 1;
    ide->regs.sector = 1;
    if (ide->atapi) {		/* v16: packet device signature */
	ide->regs.lcyl = 0x14;
	ide->regs.hcyl = 0xeb;
	ide->atapi_state = ATAPI_IDLE;
	ide->data_size = 0;
	/* a packet device clears DRDY at reset and sets it again with
	 * IDENTIFY PACKET DEVICE (ATA/ATAPI-4/5); drivers tell ATA and
	 * ATAPI devices apart by DRDY and the signature */
	ide->atapi_drdy = 0;
    } else {
	ide->regs.lcyl = 0;
	ide->regs.hcyl = 0;
    }
    ide->regs.status = 0;
    ide->regs.error = 0x01;	/* device ok */
}

static void reset_device (struct ide_drive *ide, int both)
{
    set_signature (ide);
    if (both)
	set_signature (ide->pair);
}

static void ide_execute_drive_diagnostics (struct ide_drive *ide, int irq)
{
    reset_device (ide, irq);
    if (irq)
	ide_interrupt (ide);
    else
	ide->regs.status &= ~IDE_STATUS_BSY;
}

static void ide_initialize_drive_parameters (struct ide_drive *ide)
{
    if (ide->size) {
	ide->secs = ide->regs.nsector == 0 ? 256 : ide->regs.nsector;
	ide->heads = (ide->regs.select & 15) + 1;
	ide->cyls = (unsigned int) ((ide->size / ide->blocksize) / ((uae_u64) ide->secs * ide->heads));
	if ((uae_u64) ide->heads * ide->cyls * ide->secs > 16515072 || ide->lba48) {
	    ide->cyls = ide->cyls_def;
	    ide->heads = ide->heads_def;
	    ide->secs = ide->secs_def;
	}
    } else {
	ide->regs.error |= IDE_ERR_ABRT;
	ide->regs.status |= IDE_STATUS_ERR;
    }
    write_log ("IDE%d initialize drive parameters, CYL=%d,SPT=%d,HEAD=%d\n",
	       ide->num, ide->cyls, ide->secs, ide->heads);
    ide_interrupt (ide);
}

static void ide_set_multiple_mode (struct ide_drive *ide)
{
    if (ide->ata_level < 0) {
	ide_fail (ide);
	return;
    }
    write_log ("IDE%d drive multiple mode = %d\n", ide->num, ide->regs.nsector);
    if (ide->regs.nsector > (ide->max_multiple_mode >> (ide->blocksize / 512 - 1)))
	ide_fail (ide);
    else
	ide->multiple_mode = ide->regs.nsector;
    ide_interrupt (ide);
}

static void ide_set_features (struct ide_drive *ide)
{
    if (ide->ata_level < 0) {
	ide_fail (ide);
	return;
    }
    switch (ide->regs.feat) {
    case 1:		/* 8-bit mode */
    case 0x81:
    case 2:		/* write cache */
    case 0x82:
	ide_interrupt (ide);
	break;
    default:
	ide_fail (ide);
	break;
    }
}

static int get_nsec (struct ide_drive *ide)
{
    if (ide->lba48 && ide->lba48cmd)
	return (ide->regs.nsector == 0 && ide->regs.nsector2 == 0) ? 65536 : (ide->regs.nsector2 * 256 + ide->regs.nsector);
    else
	return ide->regs.nsector == 0 ? 256 : ide->regs.nsector;
}

static int dec_nsec (struct ide_drive *ide, int v)
{
    if (ide->lba48 && ide->lba48cmd) {
	uae_u16 nsec = (ide->regs.nsector2 << 8) | ide->regs.nsector;
	nsec -= v;
	ide->regs.nsector2 = nsec >> 8;
	ide->regs.nsector = nsec & 0xff;
	return (ide->regs.nsector2 << 8) | ide->regs.nsector;
    } else {
	ide->regs.nsector -= v;
	return ide->regs.nsector;
    }
}

static void get_lbachs (struct ide_drive *ide, uae_u64 *lbap, unsigned int *cyl, unsigned int *head, unsigned int *sec)
{
    if (ide->lba48 && ide->lba48cmd && (ide->regs.select & 0x40)) {
	uae_u64 lba;
	lba = (ide->regs.hcyl << 16) | (ide->regs.lcyl << 8) | ide->regs.sector;
	lba |= ((uae_u64) ((ide->regs.hcyl2 << 16) | (ide->regs.lcyl2 << 8) | ide->regs.sector2)) << 24;
	*lbap = lba;
    } else {
	if ((ide->regs.select & 0x40) && ide->lba) {
	    *lbap = ((ide->regs.select & 15) << 24) | (ide->regs.hcyl << 16) | (ide->regs.lcyl << 8) | ide->regs.sector;
	} else {
	    *cyl = (ide->regs.hcyl << 8) | ide->regs.lcyl;
	    *head = ide->regs.select & 15;
	    *sec = ide->regs.sector;
	    *lbap = (((uae_u64) (*cyl) * ide->heads + (*head)) * ide->secs) + (*sec) - 1;
	}
    }
}

static void put_lbachs (struct ide_drive *ide, uae_u64 lba, unsigned int cyl, unsigned int head, unsigned int sec, unsigned int inc)
{
    if (ide->lba48 && ide->lba48cmd) {
	lba += inc;
	ide->regs.hcyl = (lba >> 16) & 0xff;
	ide->regs.lcyl = (lba >> 8) & 0xff;
	ide->regs.sector = lba & 0xff;
	lba >>= 24;
	ide->regs.hcyl2 = (lba >> 16) & 0xff;
	ide->regs.lcyl2 = (lba >> 8) & 0xff;
	ide->regs.sector2 = lba & 0xff;
    } else {
	if ((ide->regs.select & 0x40) && ide->lba) {
	    lba += inc;
	    ide->regs.select &= ~15;
	    ide->regs.select |= (lba >> 24) & 15;
	    ide->regs.hcyl = (lba >> 16) & 0xff;
	    ide->regs.lcyl = (lba >> 8) & 0xff;
	    ide->regs.sector = lba & 0xff;
	} else {
	    sec += inc;
	    while (sec >= ide->secs) {
		sec -= ide->secs;
		head++;
		if (head >= ide->heads) {
		    head -= ide->heads;
		    cyl++;
		}
	    }
	    ide->regs.select &= ~15;
	    ide->regs.select |= head;
	    ide->regs.sector = sec;
	    ide->regs.hcyl = cyl >> 8;
	    ide->regs.lcyl = (uae_u8) cyl;
	}
    }
}

static void check_maxtransfer (struct ide_drive *ide, int state)
{
    if (state == 1) {
	if (ide->maxtransferstate < 2 && ide->regs.nsector == 0)
	    ide->maxtransferstate = 1;
	else if (ide->maxtransferstate == 2) {
	    write_log ("IDE maxtransfer check detected split >256 block transfer\n");
	    ide->maxtransferstate = 0;
	} else
	    ide->maxtransferstate = 0;
    } else if (state == 2) {
	if (ide->maxtransferstate == 1)
	    ide->maxtransferstate++;
	else
	    ide->maxtransferstate = 0;
    }
}

/* Host file I/O. Returns 1 on success. hdf_read()/hdf_write() abort the
 * emulator on out-of-range accesses, so check the range first. */
static int ide_hdf_io (struct ide_drive *ide, int write, uae_u8 *buf, uae_u64 lba, int nsec)
{
    uae_u64 off = lba * ide->blocksize;
    uae_u64 len = (uae_u64) nsec * ide->blocksize;
    int n;

    if (off + len > ide->hfd.size)
	return 0;
    gui_hd_led (write ? 2 : 1);
    if (write)
	n = hdf_write (&ide->hfd, buf, off, (int) len);
    else
	n = hdf_read (&ide->hfd, buf, off, (int) len);
    return n == (int) len;
}

static void do_process_rw_command (struct ide_drive *ide)
{
    unsigned int cyl = 0, head = 0, sec = 0, nsec, nsec_total;
    uae_u64 lba = 0;
    int last = 1;

    ide->data_offset = 0;

    if (ide->direction > 1)	/* FORMAT TRACK: don't write anything */
	goto end;

    nsec = get_nsec (ide);
    get_lbachs (ide, &lba, &cyl, &head, &sec);
    if (nsec > ide->max_lba - lba)
	nsec = (unsigned int) (ide->max_lba - lba);
    if (nsec <= 0 || lba >= ide->max_lba) {
	ide_data_ready (ide);
	ide_fail_err (ide, IDE_ERR_IDNF);
	return;
    }
    nsec_total = nsec;
    ide_grow_buffer (ide, nsec_total * ide->blocksize);

    if (nsec > (unsigned int) ide->data_multi)
	nsec = ide->data_multi;

    if (ide->buffer_offset == 0) {
	ide->start_lba = lba;
	ide->start_nsec = nsec_total;
    }

    if (!ide->direction) {
	if (ide->buffer_offset == 0) {
	    if (!ide_hdf_io (ide, 0, ide->secbuf, ide->start_lba, ide->start_nsec)) {
		ide_data_ready (ide);
		ide_fail_err (ide, IDE_ERR_IDNF);
		return;
	    }
	}
    }
    ide->intdrq = 1;
    last = dec_nsec (ide, nsec) == 0;
    if (ide->ata_level != 2 || !last)
	put_lbachs (ide, lba, cyl, head, sec, last ? nsec - 1 : nsec);
    if (last && ide->direction) {
	ide->intdrq = 0;
	if (ide->hfd.readonly || !ide_hdf_io (ide, 1, ide->secbuf, ide->start_lba, ide->start_nsec)) {
	    ide_data_ready (ide);
	    ide_fail_err (ide, IDE_ERR_IDNF);
	    return;
	}
    }

end:
    if (ide->direction) {
	if (last)
	    ide_fast_interrupt (ide);
	else
	    ide_interrupt (ide);
    } else {
	if (ide->buffer_offset == 0)
	    ide_fast_interrupt (ide);
	else
	    ide_interrupt (ide);
    }
}

static void process_rw_command (struct ide_drive *ide)
{
    ide->regs.status |= IDE_STATUS_BSY;
    ide->regs.status &= ~IDE_STATUS_DRQ;
    do_process_rw_command (ide);
}

static void ide_read_sectors (struct ide_drive *ide, int flags)
{
    unsigned int cyl, head, sec;
    uae_u64 lba = 0;
    int multi = flags & 1;

    ide->lba48cmd = (flags & 2) != 0;
    if (multi && (ide->multiple_mode == 0 || ide->ata_level < 0)) {
	ide_fail (ide);
	return;
    }
    check_maxtransfer (ide, 1);
    get_nsec (ide);
    get_lbachs (ide, &lba, &cyl, &head, &sec);
    if (lba >= ide->max_lba) {
	ide_data_ready (ide);
	ide_fail_err (ide, IDE_ERR_IDNF);
	return;
    }
    if (flags & 4) {	/* verify */
	ide_interrupt (ide);
	return;
    }
    ide->data_multi = multi ? ide->multiple_mode : 1;
    ide->data_offset = 0;
    ide->data_size = get_nsec (ide) * ide->blocksize;
    ide->direction = 0;
    ide->buffer_offset = 0;
    process_rw_command (ide);
}

static void ide_write_sectors (struct ide_drive *ide, int flags)
{
    unsigned int cyl, head, sec, nsec;
    uae_u64 lba = 0;
    int multi = flags & 1;

    ide->lba48cmd = (flags & 2) != 0;
    if (multi && (ide->multiple_mode == 0 || ide->ata_level < 0)) {
	ide_fail (ide);
	return;
    }
    check_maxtransfer (ide, 1);
    nsec = get_nsec (ide);
    get_lbachs (ide, &lba, &cyl, &head, &sec);
    if (lba >= ide->max_lba) {
	ide_data_ready (ide);
	ide_fail_err (ide, IDE_ERR_IDNF);
	return;
    }
    if (nsec > ide->max_lba - lba)
	nsec = (unsigned int) (ide->max_lba - lba);
    if (nsec <= 0) {
	ide_data_ready (ide);
	ide_fail_err (ide, IDE_ERR_IDNF);
	return;
    }
    ide->data_multi = multi ? ide->multiple_mode : 1;
    ide->data_offset = 0;
    ide->data_size = nsec * ide->blocksize;
    ide->direction = 1;
    ide->buffer_offset = 0;
    /* write start: set DRQ and clear BSY, no interrupt */
    ide->regs.status |= IDE_STATUS_DRQ;
    ide->regs.status &= ~IDE_STATUS_BSY;
}

static void ide_format_track (struct ide_drive *ide)
{
    unsigned int cyl, head;
    uae_u64 lba;

    cyl = (ide->regs.hcyl << 8) | ide->regs.lcyl;
    head = ide->regs.select & 15;
    lba = ((uae_u64) cyl * ide->heads + head) * ide->secs;
    if (lba >= ide->max_lba) {
	ide_interrupt (ide);
	return;
    }
    ide->data_multi = 1;
    ide->data_offset = 0;
    ide->data_size = ide->blocksize;
    ide->direction = 2;
    ide->buffer_offset = 0;
    ide->regs.status |= IDE_STATUS_DRQ;
    ide->regs.status &= ~IDE_STATUS_BSY;
}

/* ------------------------------------------------------------------ */
/* v16: ATAPI (PACKET command protocol, PIO)                          */
/* ------------------------------------------------------------------ */

#ifdef SCSIEMU
/* End of a packet command: status phase, interrupt */
static void atapi_complete (struct ide_drive *ide, int status, uae_u8 sensekey)
{
    ide->atapi_state = ATAPI_IDLE;
    ide->data_size = 0;
    ide->regs.nsector = ATAPI_IR_IO | ATAPI_IR_COD;
    if (status) {
	/* error register: sense key in bits 7-4, ABRT */
	ide_fail_err (ide, (uae_u8) ((sensekey << 4) | IDE_ERR_ABRT));
    } else {
	ide->intdrq = 0;
	ide_interrupt (ide);
    }
}

/* Next DRQ block of the data-in phase (size from the byte count limit) */
static void atapi_next_chunk (struct ide_drive *ide)
{
    int limit = ide->regs.lcyl | (ide->regs.hcyl << 8);
    int left = ide->atapi_total - ide->atapi_pos;
    int chunk;

    if (limit == 0 || limit == 0xffff)
	limit = 0xfffe;
    limit &= ~1;
    chunk = left < limit ? left : limit;
    ide->atapi_chunk = chunk;
    ide->regs.lcyl = chunk & 0xff;
    ide->regs.hcyl = chunk >> 8;
    ide->regs.nsector = ATAPI_IR_IO;
    ide->buffer_offset = ide->atapi_pos;
    ide->data_offset = 0;
    ide->data_size = (chunk + 1) & ~1;
    ide->intdrq = 1;
    ide_interrupt (ide);
}

/* All command bytes (and data-out bytes) are there: run the command */
static void atapi_execute (struct ide_drive *ide, int have_dataout)
{
    const uae_u8 *cmd = ide->atapi_packet;
    int outlen = cdimage_dataout_len (cmd);
    int inmax, inlen = 0, st;
    uae_u8 sensekey = 0;

    if (outlen > 0 && !have_dataout) {
	/* ask the host for the parameter data first */
	ide_grow_buffer (ide, outlen + 2);
	ide->atapi_state = ATAPI_DATAOUT;
	ide->regs.nsector = 0;			/* CoD = 0, IO = 0 */
	ide->regs.lcyl = outlen & 0xff;
	ide->regs.hcyl = outlen >> 8;
	ide->buffer_offset = 0;
	ide->data_offset = 0;
	ide->data_size = (outlen + 1) & ~1;
	ide->intdrq = 1;
	ide_interrupt (ide);
	return;
    }

    inmax = cdimage_datain_max (cmd);
    ide_grow_buffer (ide, inmax + 2);
    st = cdimage_scsi_cmd (0, cmd, 12, have_dataout ? ide->secbuf : NULL, have_dataout ? outlen : 0,
			   ide->secbuf, inmax, &inlen, &sensekey);
    if (IDE_LOG)
	write_log ("IDE%d ATAPI %02x -> status %d, %d bytes, sense key %d\n",
		   ide->num, cmd[0], st, inlen, sensekey);
    if (st < 0) {
	atapi_complete (ide, 2, 2);		/* no drive: not ready */
	return;
    }
    if (st != 0 || inlen <= 0) {
	atapi_complete (ide, st, sensekey);
	return;
    }
    ide->atapi_state = ATAPI_DATAIN;
    ide->atapi_total = inlen;
    ide->atapi_pos = 0;
    atapi_next_chunk (ide);
}

static void atapi_packet_command (struct ide_drive *ide)
{
    if (ide->regs.feat & 1) {			/* DMA: not offered */
	ide_fail (ide);
	return;
    }
    ide->atapi_drdy = 1;
    ide->atapi_state = ATAPI_PACKET;
    ide->regs.nsector = ATAPI_IR_COD;
    ide->buffer_offset = 0;
    ide->data_offset = 0;
    ide->data_size = 12;
    /* DRQ at once, no interrupt (accelerated DRQ, IDENTIFY word 0) */
    ide->regs.status &= ~IDE_STATUS_BSY;
    ide->regs.status |= IDE_STATUS_DRQ;
}

static void atapi_identify (struct ide_drive *ide)
{
    ide->atapi_drdy = 1;
    ide->blocksize = 512;
    ide_data_ready (ide);			/* 512 bytes, interrupt */
    ide->direction = 0;
    ide->atapi_state = ATAPI_IDLE;
    atapi_identity_buffer (ide);
}

static uae_u16 atapi_get_data (struct ide_drive *ide)
{
    uae_u16 v;
    int off;

    if (ide->data_size <= 0)
	return 0xffff;
    off = ide->buffer_offset + ide->data_offset;
    v = ide->secbuf[off + 1] | (ide->secbuf[off] << 8);
    ide->data_offset += 2;
    ide->data_size -= 2;
    if (ide->data_size <= 0) {
	ide->data_size = 0;
	ide->regs.status &= ~IDE_STATUS_DRQ;
	if (ide->atapi_state == ATAPI_DATAIN) {
	    ide->atapi_pos += ide->atapi_chunk;
	    if (ide->atapi_pos < ide->atapi_total)
		atapi_next_chunk (ide);
	    else
		atapi_complete (ide, 0, 0);
	}
    }
    return v;
}

static void atapi_put_data (struct ide_drive *ide, uae_u16 v)
{
    if (ide->data_size <= 0)
	return;
    if (ide->atapi_state == ATAPI_PACKET) {
	if (ide->data_offset < 12) {
	    ide->atapi_packet[ide->data_offset + 0] = v >> 8;
	    ide->atapi_packet[ide->data_offset + 1] = v & 0xff;
	}
	ide->data_offset += 2;
	ide->data_size -= 2;
	if (ide->data_size <= 0) {
	    ide->data_size = 0;
	    ide->regs.status &= ~IDE_STATUS_DRQ;
	    ide->regs.status |= IDE_STATUS_BSY;
	    atapi_execute (ide, 0);
	}
    } else if (ide->atapi_state == ATAPI_DATAOUT) {
	ide->secbuf[ide->data_offset + 0] = v >> 8;
	ide->secbuf[ide->data_offset + 1] = v & 0xff;
	ide->data_offset += 2;
	ide->data_size -= 2;
	if (ide->data_size <= 0) {
	    ide->data_size = 0;
	    ide->regs.status &= ~IDE_STATUS_DRQ;
	    ide->regs.status |= IDE_STATUS_BSY;
	    atapi_execute (ide, 1);
	}
    }
}

/* ATA commands an ATAPI device accepts */
static void atapi_do_command (struct ide_drive *ide, uae_u8 cmd)
{
    switch (cmd) {
    case 0xa0:			/* PACKET */
	atapi_packet_command (ide);
	break;
    case 0xa1:			/* IDENTIFY PACKET DEVICE */
	atapi_identify (ide);
	break;
    case 0xec:			/* IDENTIFY DEVICE: abort + signature */
	ide_identify_drive (ide);
	break;
    case 0x08:			/* DEVICE RESET */
	set_signature (ide);
	ide->regs.status &= ~IDE_STATUS_BSY;
	break;
    case 0x90:			/* EXECUTE DEVICE DIAGNOSTIC */
	ide_execute_drive_diagnostics (ide, 1);
	break;
    case 0xef:			/* SET FEATURES (transfer mode etc.) */
	ide_interrupt (ide);
	break;
    case 0xe5:			/* CHECK POWER MODE */
	ide->regs.nsector = 0xff;
	ide_interrupt (ide);
	break;
    case 0xe0: case 0xe1: case 0xe6: case 0xe7:	/* standby / idle / sleep / flush */
	ide_interrupt (ide);
	break;
    default:
	if (IDE_LOG)
	    write_log ("IDE%d: ATA command 0x%02x not for ATAPI\n", ide->num, cmd);
	ide_fail (ide);
	break;
    }
}
#endif /* SCSIEMU */

static void ide_do_command (struct ide_drive *ide, uae_u8 cmd)
{
    int lba48 = ide->lba48;

    ide->regs.status &= ~(IDE_STATUS_DRDY | IDE_STATUS_DRQ | IDE_STATUS_ERR);
    ide->regs.error = 0;
    ide->intdrq = 0;
    ide->lba48cmd = 0;
    ide->irq_delay = 0;

#ifdef SCSIEMU
    if (ide->atapi) {
	atapi_do_command (ide, cmd);
	return;
    }
#endif
    if (cmd == 0x10) {			/* recalibrate */
	ide_recalibrate (ide);
    } else if (cmd == 0xec) {		/* identify drive */
	ide_identify_drive (ide);
    } else if (cmd == 0x90) {		/* execute drive diagnostics */
	ide_execute_drive_diagnostics (ide, 1);
    } else if (cmd == 0x91) {		/* initialize drive parameters */
	ide_initialize_drive_parameters (ide);
    } else if (cmd == 0xc6) {		/* set multiple mode */
	ide_set_multiple_mode (ide);
    } else if (cmd == 0x20 || cmd == 0x21) {	/* read sectors */
	ide_read_sectors (ide, 0);
    } else if (cmd == 0x40 || cmd == 0x41) {	/* verify sectors */
	ide_read_sectors (ide, 4);
    } else if (cmd == 0x24 && lba48) {	/* read sectors ext */
	ide_read_sectors (ide, 2);
    } else if (cmd == 0xc4) {		/* read multiple */
	ide_read_sectors (ide, 1);
    } else if (cmd == 0x29 && lba48) {	/* read multiple ext */
	ide_read_sectors (ide, 1 | 2);
    } else if (cmd == 0x30 || cmd == 0x31) {	/* write sectors */
	ide_write_sectors (ide, 0);
    } else if (cmd == 0x34 && lba48) {	/* write sectors ext */
	ide_write_sectors (ide, 2);
    } else if (cmd == 0xc5) {		/* write multiple */
	ide_write_sectors (ide, 1);
    } else if (cmd == 0x39 && lba48) {	/* write multiple ext */
	ide_write_sectors (ide, 1 | 2);
    } else if (cmd == 0x50) {		/* format track */
	ide_format_track (ide);
    } else if (cmd == 0xef) {		/* set features */
	ide_set_features (ide);
    } else if (cmd == 0x00) {		/* nop */
	ide_fail (ide);
    } else if (cmd == 0x70) {		/* seek */
	ide_interrupt (ide);
    } else if (cmd == 0xe0 || cmd == 0xe1 || cmd == 0xe7 || cmd == 0xea) {
	/* standby now / idle / flush cache / flush cache ext */
	if (ide->ata_level < 0)
	    ide_fail (ide);
	else
	    ide_interrupt (ide);
    } else if (cmd == 0xe5) {		/* check power mode */
	ide->regs.nsector = 0xff;
	ide_interrupt (ide);
    } else {
	ide_fail (ide);
	write_log ("IDE%d: unknown ATA command 0x%02x\n", ide->num, cmd);
    }
}

static uae_u16 ide_get_data (struct ide_drive *ide)
{
    uae_u16 v;

#ifdef SCSIEMU
    if (ide->atapi)
	return atapi_get_data (ide);
#endif
    if (ide->data_size == 0)
	return ide_isdrive (ide) ? 0 : 0xffff;

    v = ide->secbuf[ide->buffer_offset + ide->data_offset + 1]
	| (ide->secbuf[ide->buffer_offset + ide->data_offset + 0] << 8);
    ide->data_offset += 2;
    if (ide->data_size < 0) {
	ide->data_size += 2;
    } else {
	ide->data_size -= 2;
	if (((ide->data_offset % ide->blocksize) == 0) && ((ide->data_offset / ide->blocksize) % ide->data_multi) == 0) {
	    if (ide->data_size) {
		ide->buffer_offset += ide->data_offset;
		do_process_rw_command (ide);
	    }
	}
    }
    if (ide->data_size == 0)
	ide->regs.status &= ~IDE_STATUS_DRQ;
    return v;
}

static void ide_put_data (struct ide_drive *ide, uae_u16 v)
{
#ifdef SCSIEMU
    if (ide->atapi) {
	atapi_put_data (ide, v);
	return;
    }
#endif
    if (ide->data_size == 0)
	return;
    ide_grow_buffer (ide, ide->data_offset + ide->buffer_offset + 2);
    ide->secbuf[ide->buffer_offset + ide->data_offset + 1] = v & 0xff;
    ide->secbuf[ide->buffer_offset + ide->data_offset + 0] = v >> 8;
    ide->data_offset += 2;
    ide->data_size -= 2;
    if (ide->data_size == 0) {
	process_rw_command (ide);
    } else if (((ide->data_offset % ide->blocksize) == 0) && ((ide->data_offset / ide->blocksize) % ide->data_multi) == 0) {
	int off = ide->data_offset;
	do_process_rw_command (ide);
	ide->buffer_offset += off;
    }
}

static uae_u32 ide_read_reg (struct ide_drive *ide, int ide_reg)
{
    uae_u8 v = 0;
    int isdrv = ide_isdrive (ide);

    if (!ide)
	return 0;

    if (ide->regs.status & IDE_STATUS_BSY)
	ide_reg = IDE_STATUS;
    if (!isdrv) {
	if (ide_reg == IDE_STATUS || ide_reg == IDE_DEVCON) {
	    if (ide->pair->irq)
		ide->pair->irq = 0;
	    if (ide_isdrive (ide->pair))
		v = 0x01;
	    else
		v = 0xff;
	}
	return v;
    }

    switch (ide_reg) {
    case IDE_SECONDARY:
    case IDE_SECONDARY + 1:
    case IDE_SECONDARY + 2:
    case IDE_SECONDARY + 3:
    case IDE_SECONDARY + 4:
    case IDE_SECONDARY + 5:
	v = 0xff;
	break;
    case IDE_DRVADDR:
	v = ((ide->ide_drv ? 2 : 1) | ((ide->regs.select & 15) << 2)) ^ 0xff;
	break;
    case IDE_DATA:
	break;
    case IDE_ERROR:
	v = ide->regs.error;
	break;
    case IDE_NSECTOR:
	v = (ide->regs.devcon & 0x80) ? ide->regs.nsector2 : ide->regs.nsector;
	break;
    case IDE_SECTOR:
	v = (ide->regs.devcon & 0x80) ? ide->regs.sector2 : ide->regs.sector;
	check_maxtransfer (ide, 2);
	break;
    case IDE_LCYL:
	v = (ide->regs.devcon & 0x80) ? ide->regs.lcyl2 : ide->regs.lcyl;
	break;
    case IDE_HCYL:
	v = (ide->regs.devcon & 0x80) ? ide->regs.hcyl2 : ide->regs.hcyl;
	break;
    case IDE_SELECT:
	v = ide->regs.select;
	break;
    case IDE_STATUS:
	ide->irq = 0;
	ide->irq_new = 0;
	/* fall through */
    case IDE_DEVCON:		/* ALTSTATUS when reading */
	v = ide->regs.status;
	if (ide->atapi) {
	    /* v16: DRDY only when the packet device is ready, no DSC (SERV) */
	    if (ide->atapi_drdy)
		v |= IDE_STATUS_DRDY;
	} else
	    v |= IDE_STATUS_DRDY | IDE_STATUS_DSC;
	break;
    }
    return v;
}

static void ide_write_reg (struct ide_drive *ide, int ide_reg, uae_u32 val)
{
    if (!ide)
	return;

    ide->regs1->devcon &= ~0x80;	/* clear HOB */
    ide->regs0->devcon &= ~0x80;

    switch (ide_reg) {
    case IDE_DRVADDR:
	break;
    case IDE_DEVCON:
	if ((ide->regs.devcon & 4) == 0 && (val & 4) != 0)
	    reset_device (ide, 1);	/* SRST */
	ide->regs0->devcon = val;
	ide->regs1->devcon = val;
	break;
    case IDE_DATA:
	break;
    case IDE_ERROR:
	ide->regs0->feat2 = ide->regs0->feat;
	ide->regs0->feat = val;
	ide->regs1->feat2 = ide->regs1->feat;
	ide->regs1->feat = val;
	break;
    case IDE_NSECTOR:
	ide->regs0->nsector2 = ide->regs0->nsector;
	ide->regs0->nsector = val;
	ide->regs1->nsector2 = ide->regs1->nsector;
	ide->regs1->nsector = val;
	break;
    case IDE_SECTOR:
	ide->regs0->sector2 = ide->regs0->sector;
	ide->regs0->sector = val;
	ide->regs1->sector2 = ide->regs1->sector;
	ide->regs1->sector = val;
	break;
    case IDE_LCYL:
	ide->regs0->lcyl2 = ide->regs0->lcyl;
	ide->regs0->lcyl = val;
	ide->regs1->lcyl2 = ide->regs1->lcyl;
	ide->regs1->lcyl = val;
	break;
    case IDE_HCYL:
	ide->regs0->hcyl2 = ide->regs0->hcyl;
	ide->regs0->hcyl = val;
	ide->regs1->hcyl2 = ide->regs1->hcyl;
	ide->regs1->hcyl = val;
	break;
    case IDE_SELECT:
	ide->regs0->select = val;
	ide->regs1->select = val;
	ide->pair->ide_drv = ide->ide_drv = (val & 0x10) ? 1 : 0;
	break;
    case IDE_STATUS:
	ide->irq = 0;
	ide->irq_new = 0;
	if (ide_isdrive (ide)) {
	    ide->regs.status |= IDE_STATUS_BSY;
	    ide_do_command (ide, val);
	}
	break;
    }
}

/* Advance delayed interrupts of both drives. Returns true if an
 * interrupt is pending. */
static int ide_interrupt_hsync (struct ide_drive *idep)
{
    int i, irq = 0;

    for (i = 0; i < 2; i++) {
	struct ide_drive *ide = i == 0 ? idep : idep->pair;
	if (ide->irq_delay > 0) {
	    int delay = ide->irq_delay;
	    ide->irq_delay = delay - 1;
	    if (delay == 1)
		ide_interrupt_do (ide);
	}
	if (ide->irq && !(ide->regs.devcon & 2))
	    irq = 1;
    }
    return irq;
}

/* ------------------------------------------------------------------ */
/* Drive setup                                                        */
/* ------------------------------------------------------------------ */

static void ide_close_drive (struct ide_drive *ide)
{
    if (ide->present && !ide->atapi)
	hdf_close (&ide->hfd);
    ide->atapi = 0;
    ide->present = 0;
    free (ide->secbuf);
    ide->secbuf = 0;
    ide->secbuf_size = 0;
}

static void ide_default_geometry (struct ide_drive *ide)
{
    uae_u64 totalsecs = ide->size / ide->blocksize;
    uae_u64 cyls;

    ide->heads_def = 16;
    ide->secs_def = 63;
    cyls = totalsecs / (ide->heads_def * ide->secs_def);
    if (cyls > 16383)
	cyls = 16383;
    if (cyls < 1)
	cyls = 1;
    ide->cyls_def = (unsigned int) cyls;
    ide->cyls = ide->cyls_def;
    ide->heads = ide->heads_def;
    ide->secs = ide->secs_def;
}

static void ide_open_drive (struct ide_drive *ide, int unit)
{
    const char *path = currprefs.ide_hardfile[unit];

#ifdef SCSIEMU
    /* v16: the CD-ROM image drive as ATAPI device on this position */
    if (currprefs.cddrive[0] && !CD_ON_AKIKO (&currprefs)
	&& currprefs.cd_controller == (unit == 0 ? CD_CTRL_IDE0 : CD_CTRL_IDE1)) {
	if (path[0]) {
	    write_log ("IDE%d: hardfile and CD-ROM on the same position, using the hardfile\n", unit);
	} else {
	    cdimage_start ();
	    ide->present = 1;
	    ide->atapi = 1;
	    ide->blocksize = 512;
	    ide->ata_level = 0;
	    ide->max_multiple_mode = 0;
	    write_log ("IDE%d ATAPI CD-ROM, image '%s'\n", unit, currprefs.cdimage[0]);
	    return;
	}
    }
#endif
    if (!path[0])
	return;

    memset (&ide->hfd, 0, sizeof ide->hfd);
    ide->hfd.readonly = currprefs.ide_readonly[unit];
    ide->hfd.handle = -1;
    if (!hdf_open (&ide->hfd, path)) {
	write_log ("IDE%d: failed to open hardfile '%s'\n", unit, path);
	return;
    }
    ide->present = 1;
    ide->blocksize = 512;
    ide->hfd.blocksize = 512;
    ide->size = ide->hfd.size & ~((uae_u64) 511);
    ide->hfd.size = ide->hfd.size2 = ide->size;
    ide->max_lba = ide->size / ide->blocksize;
    ide->max_multiple_mode = 128;
    ide->lba = 1;
    ide->lba48 = ide->size >= 128 * (uae_u64) 0x40000000 ? 1 : 0;
    ide->ata_level = (ide->size >= 4 * (uae_u64) 0x40000000) ? 1 : 0;
    if (ide->lba48 && !ide->ata_level)
	ide->ata_level = 1;
    ide_default_geometry (ide);
    write_log ("IDE%d HD '%s', CHS=%d/%d/%d %uM LBA48=%d%s\n", unit, path,
	       ide->cyls, ide->heads, ide->secs, (unsigned int) (ide->size / (1024 * 1024)),
	       ide->lba48, ide->hfd.readonly ? " (read-only)" : "");
}

void gayle_cleanup (void)
{
    int i;
    for (i = 0; i < 2; i++)
	ide_close_drive (&idedrive[i]);
}

void gayle_reset (void)
{
    int i;

    gayle_cleanup ();
    memset (idedrive, 0, sizeof idedrive);
    gayle_irq = gayle_int = gayle_cs = gayle_cs_mask = gayle_cfg = 0;
    gayle_id_cnt = 0;

    if (currprefs.cs_ide == IDE_NONE)
	return;

    for (i = 0; i < 2; i++) {
	struct ide_drive *ide = &idedrive[i];
	ide->num = i;
	ide->hfd.handle = -1;
	ide->blocksize = 512;
	ide->regs0 = &idedrive[0].regs;
	ide->regs1 = &idedrive[1].regs;
	ide->pair = &idedrive[1 - i];
	ide_grow_buffer (ide, 1024);
    }
    for (i = 0; i < 2; i++)
	ide_open_drive (&idedrive[i], i);

    reset_device (&idedrive[0], 1);
}

/* ------------------------------------------------------------------ */
/* Gayle                                                              */
/* ------------------------------------------------------------------ */

static uae_u8 check_gayle_ide_irq (void)
{
    int i, irq = 0;

    for (i = 0; i < 2; i++) {
	struct ide_drive *ide = &idedrive[i];
	if (!(ide->regs.devcon & 2) && ide->irq)
	    irq = 1;
	/* "IDE killer": do not eat interrupt of a non-existing drive */
	if (ide->irq && !ide_isdrive (ide))
	    ide->irq = 0;
    }
    return irq ? GAYLE_IRQ_IDE : 0;
}

static void rethink_gayle (void)
{
    int lev2 = 0, lev6 = 0;
    uae_u8 mask;

    if (currprefs.cs_ide == IDE_A4000) {
	gayle_irq |= check_gayle_ide_irq ();
	if (gayle_irq & GAYLE_IRQ_IDE)
	    INTREQ_0 (0x8000 | INTF_PORTS);
	return;
    }
    if (currprefs.cs_ide != IDE_A600A1200)
	return;
    gayle_irq |= check_gayle_ide_irq ();
    mask = gayle_int & gayle_irq;
    if (mask & (GAYLE_IRQ_IDE | GAYLE_IRQ_WR))
	lev2 = 1;
    if (mask & GAYLE_IRQ_CCDET)
	lev6 = 1;
    if (mask & (GAYLE_IRQ_BVD1 | GAYLE_IRQ_BVD2)) {
	if (gayle_int & GAYLE_INT_BVD_LEV)
	    lev6 = 1;
	else
	    lev2 = 1;
    }
    if (mask & GAYLE_IRQ_BSY) {
	if (gayle_int & GAYLE_INT_BSY_LEV)
	    lev6 = 1;
	else
	    lev2 = 1;
    }
    if (lev2)
	INTREQ_0 (0x8000 | INTF_PORTS);
    if (lev6)
	INTREQ_0 (0x8000 | INTF_EXTER);
}

void gayle_hsync_handler (void)
{
    if (currprefs.cs_ide == IDE_NONE)
	return;
    ide_interrupt_hsync (&idedrive[0]);
    rethink_gayle ();
}

static void write_gayle_irq (uae_u8 val)
{
    gayle_irq = (gayle_irq & val) | (val & (GAYLE_IRQ_RESET | GAYLE_IRQ_BERR));
}

static uae_u8 read_gayle_irq (void)
{
    rethink_gayle ();
    return gayle_irq;
}

static void write_gayle_cs (uae_u8 val)
{
    gayle_cs_mask = val & ~3;
    gayle_cs &= ~3;
    gayle_cs |= val & 3;
}

static uae_u8 read_gayle_cs (void)
{
    uae_u8 v = gayle_cs_mask | gayle_cs;
    v &= ~GAYLE_CS_CCDET;		/* no PCMCIA card inserted */
    v |= check_gayle_ide_irq ();
    return v;
}

static int get_gayle_ide_reg (uaecptr addr, struct ide_drive **ide)
{
    *ide = NULL;
    addr &= 0xffff;
    if (currprefs.cs_ide == IDE_A4000 && addr >= GAYLE_IRQ_4000 && addr <= GAYLE_IRQ_4000 + 1)
	return -1;
    addr &= ~0x2020;
    addr >>= 2;
    *ide = &idedrive[idedrive[0].ide_drv];
    return addr;
}

/* A600/A1200: only $DA0000-$DBFFFF is decoded */
static int gayle_decoded (uaecptr addr)
{
    if (currprefs.cs_ide == IDE_A600A1200)
	return (addr & 0xA0000) == 0xA0000;
    return 1;
}

static uae_u32 gayle_read2 (uaecptr addr)
{
    struct ide_drive *ide = NULL;
    int ide_reg;

    addr &= 0xffff;
    if (currprefs.cs_ide == IDE_NONE)
	return 0xff;
    if (addr >= GAYLE_IRQ_4000 && addr <= GAYLE_IRQ_4000 + 1 && currprefs.cs_ide == IDE_A4000) {
	uae_u8 v;
	rethink_gayle ();
	v = gayle_irq;
	gayle_irq = 0;
	return v;
    }
    if (addr >= 0x4000) {
	if (currprefs.cs_ide == IDE_A600A1200) {
	    if (addr == GAYLE_IRQ_1200)
		return read_gayle_irq ();
	    if (addr == GAYLE_INT_1200)
		return gayle_int;
	    if (addr == GAYLE_CS_1200)
		return read_gayle_cs ();
	    if (addr == GAYLE_CFG_1200)
		return gayle_cfg & 0x0f;
	}
	return 0;
    }
    ide_reg = get_gayle_ide_reg (addr, &ide);
    /* Emulated "ide killer". Prevents long KS boot delay if no drives installed */
    if (!ide_isdrive (&idedrive[0]) && !ide_isdrive (&idedrive[1])) {
	if (ide_reg == IDE_STATUS)
	    return 0x7f;
	return 0xff;
    }
    return ide_read_reg (ide, ide_reg);
}

static void gayle_write2 (uaecptr addr, uae_u32 val)
{
    struct ide_drive *ide = NULL;
    int ide_reg;

    addr &= 0xffff;
    if (currprefs.cs_ide == IDE_NONE)
	return;
    if (currprefs.cs_ide == IDE_A600A1200) {
	if (addr == GAYLE_IRQ_1200) {
	    write_gayle_irq (val);
	    return;
	}
	if (addr == GAYLE_INT_1200) {
	    gayle_int = val;
	    return;
	}
	if (addr == GAYLE_CS_1200) {
	    write_gayle_cs (val);
	    return;
	}
	if (addr == GAYLE_CFG_1200) {
	    gayle_cfg = val;
	    return;
	}
    }
    if (addr >= 0x4000)
	return;
    ide_reg = get_gayle_ide_reg (addr, &ide);
    ide_write_reg (ide, ide_reg, val);
}

static uae_u32 REGPARAM2 gayle_lget (uaecptr addr);
static uae_u32 REGPARAM2 gayle_wget (uaecptr addr);
static uae_u32 REGPARAM2 gayle_bget (uaecptr addr);
static void REGPARAM2 gayle_lput (uaecptr addr, uae_u32 value);
static void REGPARAM2 gayle_wput (uaecptr addr, uae_u32 value);
static void REGPARAM2 gayle_bput (uaecptr addr, uae_u32 value);

static uae_u32 REGPARAM2 gayle_lget (uaecptr addr)
{
    struct ide_drive *ide = NULL;
    int ide_reg;
    uae_u32 v;

    if (gayle_decoded (addr)) {
	ide_reg = get_gayle_ide_reg (addr, &ide);
	if (ide_reg == IDE_DATA && (addr & 0xffff) < 0x4000) {
	    v = ide_get_data (ide) << 16;
	    v |= ide_get_data (ide);
	    return v;
	}
    }
    v = gayle_wget (addr) << 16;
    v |= gayle_wget (addr + 2);
    return v;
}

static uae_u32 REGPARAM2 gayle_wget (uaecptr addr)
{
    struct ide_drive *ide = NULL;
    int ide_reg;
    uae_u32 v;

    if (gayle_decoded (addr)) {
	ide_reg = get_gayle_ide_reg (addr, &ide);
	if (ide_reg == IDE_DATA && (addr & 0xffff) < 0x4000)
	    return ide_get_data (ide);
    }
    v = gayle_bget (addr) << 8;
    v |= gayle_bget (addr + 1);
    return v;
}

static uae_u32 REGPARAM2 gayle_bget (uaecptr addr)
{
    if (!gayle_decoded (addr))
	return 0;
    return gayle_read2 (addr);
}

static void REGPARAM2 gayle_lput (uaecptr addr, uae_u32 value)
{
    struct ide_drive *ide = NULL;
    int ide_reg;

    if (gayle_decoded (addr)) {
	ide_reg = get_gayle_ide_reg (addr, &ide);
	if (ide_reg == IDE_DATA && (addr & 0xffff) < 0x4000) {
	    ide_put_data (ide, value >> 16);
	    ide_put_data (ide, value & 0xffff);
	    return;
	}
    }
    gayle_wput (addr, value >> 16);
    gayle_wput (addr + 2, value & 0xffff);
}

static void REGPARAM2 gayle_wput (uaecptr addr, uae_u32 value)
{
    struct ide_drive *ide = NULL;
    int ide_reg;

    if (gayle_decoded (addr)) {
	ide_reg = get_gayle_ide_reg (addr, &ide);
	if (ide_reg == IDE_DATA && (addr & 0xffff) < 0x4000) {
	    ide_put_data (ide, value);
	    return;
	}
    }
    gayle_bput (addr, value >> 8);
    gayle_bput (addr + 1, value & 0xff);
}

static void REGPARAM2 gayle_bput (uaecptr addr, uae_u32 value)
{
    if (!gayle_decoded (addr))
	return;
    gayle_write2 (addr, value);
}

addrbank gayle_bank = {
    gayle_lget, gayle_wget, gayle_bget,
    gayle_lput, gayle_wput, gayle_bput,
    default_xlate, default_check, NULL
};

/* Gayle ID register at $DE1000: a bit is shifted out on every read.
 * Gayle = $D0, AA Gayle = $D1. Any write restarts the sequence. */
static uae_u32 REGPARAM2 gayle2_lget (uaecptr addr);
static uae_u32 REGPARAM2 gayle2_wget (uaecptr addr);
static uae_u32 REGPARAM2 gayle2_bget (uaecptr addr);
static void REGPARAM2 gayle2_lput (uaecptr addr, uae_u32 value);
static void REGPARAM2 gayle2_wput (uaecptr addr, uae_u32 value);
static void REGPARAM2 gayle2_bput (uaecptr addr, uae_u32 value);

static uae_u32 REGPARAM2 gayle2_bget (uaecptr addr)
{
    uae_u8 v = 0;

    addr &= 0xffff;
    if (addr == 0x1000) {
	if (gayle_id_cnt == 0 || gayle_id_cnt == 1 || gayle_id_cnt == 3
	    || ((currprefs.chipset_mask & CSMASK_AGA) && gayle_id_cnt == 7))
	    v = 0x80;
	gayle_id_cnt++;
    }
    return v;
}

static uae_u32 REGPARAM2 gayle2_wget (uaecptr addr)
{
    uae_u16 v = gayle2_bget (addr) << 8;
    v |= gayle2_bget (addr + 1);
    return v;
}

static uae_u32 REGPARAM2 gayle2_lget (uaecptr addr)
{
    uae_u32 v = gayle2_wget (addr) << 16;
    v |= gayle2_wget (addr + 2);
    return v;
}

static void REGPARAM2 gayle2_bput (uaecptr addr, uae_u32 value)
{
    gayle_id_cnt = 0;
}

static void REGPARAM2 gayle2_wput (uaecptr addr, uae_u32 value)
{
    gayle2_bput (addr, value >> 8);
    gayle2_bput (addr + 1, value & 0xff);
}

static void REGPARAM2 gayle2_lput (uaecptr addr, uae_u32 value)
{
    gayle2_wput (addr, value >> 16);
    gayle2_wput (addr + 2, value & 0xffff);
}

addrbank gayle2_bank = {
    gayle2_lget, gayle2_wget, gayle2_bget,
    gayle2_lput, gayle2_wput, gayle2_bput,
    default_xlate, default_check, NULL
};

void gayle_map_banks (void)
{
    if (currprefs.cs_ide == IDE_A600A1200) {
	map_banks (&gayle_bank, 0xD8, 4, 0);	/* $D80000-$DBFFFF, $DC = clock */
	map_banks (&gayle2_bank, 0xDD, 2, 0);	/* $DD0000-$DEFFFF, ID at $DE1000 */
    } else if (currprefs.cs_ide == IDE_A4000) {
	map_banks (&gayle_bank, 0xDD, 1, 0);	/* IDE at $DD2000, IRQ at $DD3020 */
    }
}
