 /*
  * E-UAE - The portable Amiga Emulator
  *
  * CD-ROM image drives for uaescsi.device (v16, blkdev-cdimage.c)
  */

#ifndef CDIMAGE_H
#define CDIMAGE_H

extern struct device_functions devicefunc_cdimage;

/* (re)apply currprefs.cddrive[] / cdimage[] - at reset */
extern void cdimage_reset (void);
/* once per frame: media change from the GUI, eject, CD-DA timing */
extern void cdimage_vsync (void);
/* mix CD audio into a buffer of native signed 16-bit samples */
extern void cdimage_mix (uae_s16 *buf, int samples, int channels, int freq);
/* 2048-byte block access for CMD_READ / TD_READ64; returns io_Error */
extern int cdimage_read_blocks (int unitnum, uae_u64 offset, uae_u32 length, uaecptr data, uae_u32 *actual);
extern int cdimage_unit_present (int unitnum);
/* end of the emulation: stop audio, close the images */
extern void cdimage_shutdown (void);
/* built-in CD file system (filesys_cdfs.c) */
extern int cdimage_read_host (int unitnum, uae_u32 lba, int count, uae_u8 *buf);
extern int cdimage_change_count (int unitnum);
/* ATAPI drive on the IDE controller (gayle.c) */
extern void cdimage_start (void);
extern int cdimage_dataout_len (const uae_u8 *cmd);
extern int cdimage_datain_max (const uae_u8 *cmd);
extern int cdimage_scsi_cmd (int unitnum, const uae_u8 *cmd, int cmdlen,
			     const uae_u8 *dataout, int dataoutlen,
			     uae_u8 *datain, int datainmax, int *datainlen, uae_u8 *sensekey);


/* v17: CD32 Akiko drive (akiko.c) - WinUAE's TOC layout (struct cd_toc) */
#define CDIMAGE_MAX_TOC 103
struct cdimage_toc {
    uae_u8 adr, control;
    uae_u8 point;
    uae_u8 track;
    int paddress;          /* LSN */
};
struct cdimage_toc_head {
    int first_track, first_track_offset;
    int last_track, last_track_offset;
    int firstaddress;      /* LSN */
    int lastaddress;       /* LSN of the lead-out */
    int tracks;
    int points;
    struct cdimage_toc toc[CDIMAGE_MAX_TOC];
};
#define CDIMAGE_AUDIO_PLAYING   0x11
#define CDIMAGE_AUDIO_PAUSED    0x12
#define CDIMAGE_AUDIO_COMPLETED 0x13
#define CDIMAGE_AUDIO_ERROR     0x14
#define CDIMAGE_AUDIO_NONE      0x15

extern int  cdimage_hl_ismedia (int unitnum);
extern int  cdimage_hl_toc (int unitnum, struct cdimage_toc_head *th);
extern int  cdimage_hl_rawread (int unitnum, uae_u8 *buf, int lsn, int count);
extern int  cdimage_hl_play (int unitnum, int start, int end);
extern void cdimage_hl_pause (int unitnum, int paused);
extern void cdimage_hl_stop (int unitnum);
extern void cdimage_hl_volume (int unitnum, int left, int right);
extern int  cdimage_hl_audio_status (int unitnum, int *lsn);
extern int  cdimage_hl_qcode (int unitnum, uae_u8 *buf, int sector);
extern int  cdimage_hl_subchannel (int unitnum, uae_u8 *sub, int sector);

#endif
