#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "CONFIG.H"
#include "PLATFORM.H"
#include "DPMI.H"
#include "LINEAR.H"
#include "CDAUDIO.H"

#define CDDA_MAGIC 0xCDDA

/* Mailbox field offsets (matches shsucdx.nsm shsucdx_mailbox layout) */
#define MBOX_FLAG      0
#define MBOX_COMMAND   1
#define MBOX_ADDRMODE  2
#define MBOX_START_RAW 4
#define MBOX_NUM_SECT  8

/* Timer request field offsets (matches shsucdx.nsm shared_slot layout) */
#define TIMER_REQ_PENDING    0   /* 0=idle, 1=requested, 2=complete, 3=error */
#define TIMER_REQ_SECTORS    1
#define TIMER_REQ_DRIVE      2
#define TIMER_REQ_LBA        4
#define SHARED_SLOT_SEG    8
#define SHARED_SLOT_OFF   10

uint32_t shsucdx_mailbox_lin = 0;
uint32_t audio_read_req_lin  = 0;
uint16_t mod_shsucdx_seg     = 0;
int mod_shsucdx_detected = 0;

cd_audio_playstate_struct cd_audio_playstate = { AUDIO_STOPPED, 0, 0, 0 };

audio_buffer_struct audio_buffer = { NULL, 0, 0, 0, 0 };

/* Shared slot in conventional memory (real-mode addressable) */
uint16_t shared_slot_seg = 0;
uint32_t shared_slot_lin = 0;
static int shared_slot_sel = 0;  /* PM selector for DPMI free */



int audio_mixing_active = 0; /* set once prefill reached; cleared on stop/new play */


/* XMS direct-read state */
int xms_available = 0;
uint16_t xms_handle = 0;
uint16_t xms_sectsize = 0;
uint32_t xms_entry = 0;
uint32_t xms_reads_ok = 0;
uint32_t xms_reads_err = 0;
uint32_t xms_bytes_written = 0;
uint32_t xms_bytes_mixed = 0;
uint32_t xms_first_offset = 0;
uint32_t xms_first_total = 0;
uint8_t xms_first_data[8] = {0};
int xms_first_captured = 0;

uint32_t redbook_to_lba(uint32_t redbook)
{
    uint32_t min	= (redbook >> 16)	& 0xFF;
    uint32_t sec	= (redbook >> 8)	& 0xFF;
    uint32_t frame	=  redbook			& 0xFF;
    return min * 75 * 60 + sec * 75 + frame - 150;
}

int check_modified_shsucdx(void)
{
    __dpmi_regs r;
    memset(&r, 0, sizeof(r));
    r.x.ax = 0x15FF;
    r.x.bx = CDDA_MAGIC;
    r.x.ss = r.x.sp = 0;
    __dpmi_simulate_real_mode_interrupt(0x2F, &r);

    if (r.x.ax != CDDA_MAGIC) {
        printf("CD Audio: Modified SHSUCDX not found (AX=%04X)\n", (unsigned)r.x.ax);
        return -1;
    }

    mod_shsucdx_seg     = r.x.es;
    shsucdx_mailbox_lin = ((uint32_t)r.x.es << 4) + r.x.bx;
    audio_read_req_lin  = ((uint32_t)r.x.es << 4) + r.x.di;
    mod_shsucdx_detected    = 1;

    printf("CD Audio: SHSUCDX seg=%04X mbox_off=%04X timer_req_off=%04X\n",
           (unsigned)mod_shsucdx_seg, (unsigned)r.x.bx, (unsigned)r.x.di);
    printf("CD Audio: mailbox_lin=%04X%04X audio_read_req_lin=%04X%04X\n",
           (unsigned)(shsucdx_mailbox_lin >> 16),
           (unsigned)(shsucdx_mailbox_lin & 0xFFFF),
           (unsigned)(audio_read_req_lin >> 16),
           (unsigned)(audio_read_req_lin & 0xFFFF));
		   
	{
		int first_drive, num_drives, drv;
		int rh_sel, cb_sel;
		uint16_t rh_seg, cb_seg;
		uint32_t rh_lin, cb_lin;
		
		memset(&r, 0, sizeof(r));
		r.x.ax = 0x1500;
		r.x.bx = 0;
		r.x.ss = r.x.sp = 0;
		__dpmi_simulate_real_mode_interrupt(0x2F, &r);
		num_drives = r.x.bx;
		first_drive = r.x.cx;
		
		if(num_drives==0) {
			printf("Warning: No drives found for XMS query\n");
			goto skip_xms;
		}
		
		rh_seg = __dpmi_allocate_dos_memory(2, &rh_sel);
		if (rh_seg == 0xFFFF) goto skip_xms;
		cb_seg = __dpmi_allocate_dos_memory(1, &cb_sel);
		if (cb_seg == 0xFFFF) {
			__dpmi_free_dos_memory(rh_sel);
			goto skip_xms;
		}
		rh_lin = (uint32_t)rh_seg << 4;
		cb_lin = (uint32_t)cb_seg << 4;
		
		for (drv = first_drive; drv < first_drive+num_drives; drv++) {
			volatile uint8_t *rh_p = (volatile uint8_t *)NearPtr(rh_lin);
			volatile uint8_t *cb_p = (volatile uint8_t *)NearPtr(cb_lin);
			
			memset((void *)rh_p, 0, 32);
			memset((void *)cb_p, 0, 16);
			
			rh_p[0] = 26;
			rh_p[1] = 0;
			rh_p[2] = 3;
			rh_p[3] = 0;
			rh_p[4] = 0;
			
			*(volatile uint16_t *)&rh_p[14] = 0;
			*(volatile uint16_t *)&rh_p[16] = cb_seg;
			*(volatile uint16_t *)&rh_p[18] = 9;
			
			cb_p[0] = 0xFD;
			
			memset(&r, 0, sizeof(r));
			r.x.ax = 0x1510;
			r.d.ecx = drv;
			r.x.es = rh_seg;
			r.x.bx = 0;
			r.x.ss = r.x.sp = 0;
			__dpmi_simulate_real_mode_interrupt(0x2F, &r);
			
			{
				uint16_t status = *(volatile uint16_t *)&rh_p[3];
				if((status & 0x8100) != 0x0100)
					continue;
			}
			
			{
				uint16_t handle = *(volatile uint16_t *)&cb_p[1];
				uint16_t sectsize = *(volatile uint16_t *)&cb_p[3];
				uint32_t entry = *(volatile uint32_t *)&cb_p[5];
				
				if(handle!=0 && entry!=0) {
					xms_handle = handle;
					xms_sectsize = sectsize;
					xms_entry = entry;
					xms_available = 1;
					printf("XMS direct-read: drive=%c handle=%04X sectsize=%u entry=%04X:%04X\n",
						'A' + drv, (unsigned)handle, (unsigned)sectsize,
						(unsigned)(entry >> 16), (unsigned)(entry & 0xFFFF));
					break;
				}
			}
		}
		
		__dpmi_free_dos_memory(cb_sel);
		__dpmi_free_dos_memory(rh_sel);
		
		if(!xms_available)
			printf("XMS direct-read not available (no SHSUCDHD with subcmd 0xFD)\n");
    }
skip_xms:

    return 0;
}

void poll_shsucdx_mailbox(void)
{
    volatile uint8_t *mbox;
    uint8_t cmd, addrmode;
    uint32_t start_raw, num_sect;
    uint32_t start_lba, end_lba;

    if (!shsucdx_mailbox_lin)
        return;

    mbox = (volatile uint8_t *)NearPtr(shsucdx_mailbox_lin);

    if (mbox[MBOX_FLAG] != 1)
        return;

    cmd       = mbox[MBOX_COMMAND];
    addrmode  = mbox[MBOX_ADDRMODE];
    start_raw = *(volatile uint32_t *)&mbox[MBOX_START_RAW];
    num_sect  = *(volatile uint32_t *)&mbox[MBOX_NUM_SECT];

    mbox[MBOX_FLAG] = 0;  /* clear flag immediately */

    switch (cmd) {
    case 132: /* Play */
        start_lba = (addrmode == 1) ? redbook_to_lba(start_raw) : start_raw;
        end_lba   = start_lba + num_sect;

        {
            int is_bogus_start = (cd_audio_playstate.track_end == end_lba
                && (start_lba < cd_audio_playstate.track_start
                    || start_lba >= cd_audio_playstate.track_end));

            if ((cd_audio_playstate.state == AUDIO_PLAYING
                && cd_audio_playstate.track_start == start_lba)
                || is_bogus_start) {
                cd_audio_playstate.state = AUDIO_PLAYING;

                break;
            }
        }

        cd_audio_playstate.state       = AUDIO_PLAYING;
        cd_audio_playstate.track_start = start_lba;
        cd_audio_playstate.track_end   = end_lba;
        cd_audio_playstate.read_pos    = start_lba;

        audio_buffer_reset(&audio_buffer);
		
		/* discard any in-flight or completed audio read request so stale audio data isn't copied later */
		if (audio_read_req_lin)
			((volatile uint8_t *)NearPtr(audio_read_req_lin))[TIMER_REQ_PENDING] = 0;
        
		audio_mixing_active = 0; /* require prefill again for new track */

        break;

    case 133: /* Stop */
        cd_audio_playstate.state = AUDIO_STOPPED;
		audio_buffer_reset(&audio_buffer);  /* flush stale audio */
        if (audio_read_req_lin)
			((volatile uint8_t *)NearPtr(audio_read_req_lin))[TIMER_REQ_PENDING] = 0;
		audio_mixing_active = 0;

        break;

    case 136: /* Resume */
        if (cd_audio_playstate.state == AUDIO_PAUSED) {
            cd_audio_playstate.state = AUDIO_PLAYING;
        }
        break;
    }
}

void check_and_fill_audio_buffer(void)
{
    volatile uint8_t *timer_req;
    uint8_t pending;
    uint32_t sectors_remaining, sectors_to_read;
    uint32_t free_space;

    if (!audio_read_req_lin || !shared_slot_seg)
        return;
    if (cd_audio_playstate.state != AUDIO_PLAYING)
        return;

    timer_req = (volatile uint8_t *)NearPtr(audio_read_req_lin);
    pending = timer_req[TIMER_REQ_PENDING];

    if (pending == 2) {
        /* Read complete - copy staging buffer into audio buffer */
        uint32_t bytes = (uint32_t)timer_req[TIMER_REQ_SECTORS] * AUDIO_TRACK_SECTOR_SIZE;
        uint8_t *staging = (uint8_t *)NearPtr(shared_slot_lin);

        audio_buffer_write(&audio_buffer, staging, bytes);
        cd_audio_playstate.read_pos += timer_req[TIMER_REQ_SECTORS];

        timer_req[TIMER_REQ_PENDING] = 0;  /* release slot */

        /* Check if we've read to the end of the track */
        if (cd_audio_playstate.read_pos >= cd_audio_playstate.track_end) {
            cd_audio_playstate.state = AUDIO_STOPPED;
            return;
        }
    } else if (pending == 3) {
        /* Error - release slot, retry on next cycle */
        timer_req[TIMER_REQ_PENDING] = 0;
        return;
    } else if (pending == 1) {
        /* Still in progress - nothing to do */
        return;
    }

    /* pending == 0: slot is idle - post a new read if audio buffer has space */
    /*if(audio_buffer.fill_level >= audio_buffer.size / 2)
        return;*/

    free_space = audio_buffer.size - audio_buffer.fill_level;
    if (free_space < SHARED_SLOT_BYTES)
        return;

    sectors_remaining = cd_audio_playstate.track_end - cd_audio_playstate.read_pos;
    if (sectors_remaining == 0) {
        cd_audio_playstate.state = AUDIO_STOPPED;
        return;
    }

    /* Read up to SHARED_SLOT_SECTORS, capped by remaining and audio buffer space */
    sectors_to_read = SHARED_SLOT_SECTORS;
    if (sectors_to_read > sectors_remaining)
        sectors_to_read = sectors_remaining;
    if (sectors_to_read > free_space / AUDIO_TRACK_SECTOR_SIZE)
        sectors_to_read = free_space / AUDIO_TRACK_SECTOR_SIZE;

    /* Fill timer_req fields, set pending LAST */
    timer_req[TIMER_REQ_SECTORS] = (uint8_t)sectors_to_read;
    *(volatile uint32_t *)&timer_req[TIMER_REQ_LBA] = cd_audio_playstate.read_pos;
    *(volatile uint16_t *)&timer_req[SHARED_SLOT_SEG] = shared_slot_seg;
    *(volatile uint16_t *)&timer_req[SHARED_SLOT_OFF] = 0;
    timer_req[TIMER_REQ_PENDING] = 1;  /* signal SHSUCDX timer to read */
}

/* --- XMS related --- */
#define XMSMOVE_LENGTH 0
#define XMSMOVE_SRC_HANDLE 4
#define XMSMOVE_SRC_OFS 6
#define XMSMOVE_DST_HANDLE 10
#define XMSMOVE_DST_OFS 12

void xms_read_cycle(void)
{
	uint32_t sectors_remaining, sectors_to_read;
	uint32_t free_space, byte_offset, total_bytes;
	volatile uint8_t *ss;
	__dpmi_regs r;
	
	if(cd_audio_playstate.state != AUDIO_PLAYING)
		return;
	if(!shared_slot_seg)
		return;
	
	free_space = audio_buffer.size - audio_buffer.fill_level;
	if(free_space < SHARED_SLOT_BYTES)
		return;
	
	sectors_remaining = cd_audio_playstate.track_end - cd_audio_playstate.read_pos;
	if(sectors_remaining==0) {
		cd_audio_playstate.state = AUDIO_STOPPED;
		return;
	}
	
	sectors_to_read = SHARED_SLOT_SECTORS;
	if(sectors_to_read > sectors_remaining)
		sectors_to_read = sectors_remaining;
	if(sectors_to_read > free_space / AUDIO_TRACK_SECTOR_SIZE)
		sectors_to_read = free_space / AUDIO_TRACK_SECTOR_SIZE;
	
	byte_offset = cd_audio_playstate.read_pos * (uint32_t)xms_sectsize;
	total_bytes = sectors_to_read * (uint32_t)xms_sectsize;
	
	ss = (volatile uint8_t *)NearPtr(shared_slot_lin);
	
	*(volatile uint32_t *)&ss[XMSMOVE_LENGTH] = total_bytes;
	*(volatile uint16_t *)&ss[XMSMOVE_SRC_HANDLE] = xms_handle;
	*(volatile uint32_t *)&ss[XMSMOVE_SRC_OFS] = byte_offset;
	*(volatile uint16_t *)&ss[XMSMOVE_DST_HANDLE] = 0;
	*(volatile uint16_t *)&ss[XMSMOVE_DST_OFS] = 16;
	*(volatile uint16_t *)&ss[XMSMOVE_DST_OFS+2] = shared_slot_seg;
	
	memset(&r, 0, sizeof(r));
	r.h.ah = 0x0B;
	r.x.ds = shared_slot_seg;
	r.x.si = 0;
	r.x.flags = 0;
	r.x.cs = (uint16_t)(xms_entry >> 16);
	r.x.ip = (uint16_t)(xms_entry & 0xFFFF);
	r.x.ss = r.x.sp = 0;
	
	__dpmi_simulate_real_mode_procedure_retf(&r);
	
	if(r.x.ax != 1) {
		xms_reads_err++;
		return;
	}
	xms_reads_ok++;
	
	if(!xms_first_captured) {
		const uint8_t *p = (const uint8_t *)NearPtr(shared_slot_lin + 16);
		xms_first_offset = byte_offset;
		xms_first_total = total_bytes;
		memcpy(xms_first_data, p, 8);
		xms_first_captured = 1;
	}
	
	{
		uint32_t written = audio_buffer_write(&audio_buffer, 
			(const uint8_t *)NearPtr(shared_slot_lin+16),
			total_bytes);
		xms_bytes_written += written;
	}
	cd_audio_playstate.read_pos += sectors_to_read;
	
	if (cd_audio_playstate.read_pos >= cd_audio_playstate.track_end)
		cd_audio_playstate.state = AUDIO_STOPPED;
}

/* Audio buffer operations */

void audio_buffer_reset(audio_buffer_struct *rb)
{
    if (!rb) return;
    rb->write_pos  = 0;
    rb->read_pos   = 0;
    rb->fill_level = 0;
}

uint32_t audio_buffer_write(audio_buffer_struct *rb, const uint8_t *data, uint32_t len)
{
    uint32_t free_space, to_write, first_chunk;

    if (!rb || !rb->data || !data || len == 0)
        return 0;

    free_space = rb->size - rb->fill_level;
    to_write = (len <= free_space) ? len : free_space;
    if (to_write == 0)
        return 0;

    first_chunk = rb->size - rb->write_pos;
    if (first_chunk >= to_write) {
        memcpy(rb->data + rb->write_pos, data, to_write);
    } else {
        memcpy(rb->data + rb->write_pos, data, first_chunk);
        memcpy(rb->data, data + first_chunk, to_write - first_chunk);
    }
    rb->write_pos = (rb->write_pos + to_write) % rb->size;
    rb->fill_level += to_write;
    return to_write;
}

uint32_t audio_buffer_read(audio_buffer_struct *rb, uint8_t *dest, uint32_t len)
{
    uint32_t available, to_read, first_chunk;

    if (!rb || !rb->data || !dest || len == 0)
        return 0;

    available = rb->fill_level;
    to_read = (len <= available) ? len : available;
    if (to_read == 0)
        return 0;

    first_chunk = rb->size - rb->read_pos;
    if (first_chunk >= to_read) {
        memcpy(dest, rb->data + rb->read_pos, to_read);
    } else {
        memcpy(dest, rb->data + rb->read_pos, first_chunk);
        memcpy(dest + first_chunk, rb->data, to_read - first_chunk);
    }
    rb->read_pos = (rb->read_pos + to_read) % rb->size;
    rb->fill_level -= to_read;
    return to_read;
}

int cdaudio_alloc_buffers(void)
{
    int sel;
    unsigned short seg;
    int paras;

    /* Shared slot: conventional memory for SHSUCDX timer_req destination */
    paras = (SHARED_SLOT_BYTES + 16 + 15) / 16;
    seg = __dpmi_allocate_dos_memory(paras, &sel);
    if (seg == 0xFFFF) {
        printf("CD Audio: failed to allocate staging buffer (%u bytes)\n",
               (unsigned)SHARED_SLOT_BYTES);
        return -1;
    }
    shared_slot_seg = seg;
    shared_slot_sel = sel;
    shared_slot_lin = (uint32_t)seg << 4;

    printf("CD Audio: Shared slot seg=%04X lin=%04X%04X (%u bytes)\n",
           (unsigned)shared_slot_seg,
           (unsigned)(shared_slot_lin >> 16),
           (unsigned)(shared_slot_lin & 0xFFFF),
           (unsigned)SHARED_SLOT_BYTES);

    /* Audio buffer: extended memory via malloc */
    audio_buffer.data = (uint8_t *)malloc(AUDIO_BUFFER_SIZE);
    if (!audio_buffer.data) {
        printf("CD Audio: Failed to allocate buffer (%u bytes)\n",
               (unsigned)AUDIO_BUFFER_SIZE);
        __dpmi_free_dos_memory(shared_slot_sel);
        shared_slot_seg = 0;
        shared_slot_lin = 0;
        return -1;
    }
    audio_buffer.size = AUDIO_BUFFER_SIZE;
    audio_buffer_reset(&audio_buffer);

    printf("CD Audio: Audio buffer %u bytes (%u sectors)\n",
           (unsigned)AUDIO_BUFFER_SIZE,
           (unsigned)(AUDIO_BUFFER_SIZE / AUDIO_TRACK_SECTOR_SIZE));

    return 0;
}

/* SHSUCDX /A:AUTO sends PLAY to the drive (analog) again */
void cdaudio_unregister(void)
{
    __dpmi_regs r;
    if (!mod_shsucdx_detected)
        return;
    memset(&r, 0, sizeof(r));
    r.x.ax = 0x15FF;
    r.x.bx = CDDA_MAGIC;
    r.x.cx = 3;
    __dpmi_simulate_real_mode_interrupt(0x2F, &r);
    mod_shsucdx_detected = 0;
}

void free_all_buffers(void)
{
    cdaudio_unregister();
	/* Diagnostics */
	/*
	if(xms_available) {
		printf("XMS diag:\n");
		printf("  reads ok=%u err=%u\n", (unsigned)xms_reads_ok, (unsigned)xms_reads_err);
		printf("  sectsize=%u\n", (unsigned)xms_sectsize);
		printf("  first: ofs=%u len=%u\n", (unsigned)xms_first_offset, (unsigned)xms_first_total);
		printf("  forst data: %02X %02X %02X %02X %02X %02X %02X %02X\n",
			xms_first_data[0], xms_first_data[1],
			xms_first_data[2], xms_first_data[3],
			xms_first_data[4], xms_first_data[5],
			xms_first_data[6], xms_first_data[7]);
		printf("  bytes written=%u mixed=%u\n", (unsigned)xms_bytes_written, (unsigned)xms_bytes_mixed);
		printf("  audiobuf: fill=%u size=%u\n", (unsigned)audio_buffer.fill_level, (unsigned)audio_buffer.size);
		printf("  mixing=%u state=%d\n", (unsigned)audio_mixing_active, cd_audio_playstate.state);
	}
	*/
    if (audio_buffer.data) {
        free(audio_buffer.data);
        audio_buffer.data = NULL;
        audio_buffer.size = 0;
    }
    if (shared_slot_seg) {
        __dpmi_free_dos_memory(shared_slot_sel);
        shared_slot_seg = 0;
        shared_slot_lin = 0;
    }
}
