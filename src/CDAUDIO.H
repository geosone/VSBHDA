#ifndef _CDAUDIO_H_
#define _CDAUDIO_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* CD audio-related constants */
#define AUDIO_TRACK_SECTOR_SIZE		2352	/* Sector size for mixed mode CD audio track */
#define SHARED_SLOT_SECTORS			16		/* sectors per bg_read (keep small to avoid timer blocking */
#define SHARED_SLOT_BYTES			(AUDIO_TRACK_SECTOR_SIZE * SHARED_SLOT_SECTORS)
#define AUDIO_BUFFER_SIZE			(AUDIO_TRACK_SECTOR_SIZE * 300)	/* ~6 seconds at 75 sect/s */
#define AUDIO_BUFFER_PREFILL_BYTES	(AUDIO_TRACK_SECTOR_SIZE * 75)
/*	^^ How many minimum bytes in audio buffer before starting playback?
	44.1k samples/s with 4 bytes/sample = 176.4k bytes/s
	2352 bytes/sector -> 75 sectors/s. We're prefilling 176.4k bytes, i.e.
	one second worth of audio before we want to start playback. Just being conservative
	and this value worked well enough for me in testing (though may not be optimum)
*/

/* CD audio playback states */
enum {
	AUDIO_STOPPED = 0,
	AUDIO_PLAYING = 1,
	AUDIO_PAUSED  = 2
};

/* Play state tracked by ISR */
typedef struct {
	int      state;        /* PLAYING/PAUSED/STOPPED */
	uint32_t track_start;  /* LBA of play start */
	uint32_t track_end;    /* LBA of play end */
	uint32_t read_pos;     /* Next LBA to read from CD */
} cd_audio_playstate_struct;

/* Buffer for CD audio PCM data */
typedef struct {
    uint8_t  *data;
    uint32_t  size;
    uint32_t  write_pos;
    uint32_t  read_pos;
    uint32_t  fill_level;
} audio_buffer_struct;

/* linear addresses of SHSUCDX shared memory */
extern uint32_t shsucdx_mailbox_lin;
extern uint32_t audio_read_req_lin;
extern uint16_t mod_shsucdx_seg;
extern int mod_shsucdx_detected;

/* play state - written by ISR (mailbox poll), read by future mixer code */
extern cd_audio_playstate_struct cd_audio_playstate;

/* shared slot in conventional memory */
extern uint16_t shared_slot_seg;  /* real-mode segment */
extern uint32_t shared_slot_lin;  /* linear address    */

/* audio buffer for decoded CD audio */
extern audio_buffer_struct audio_buffer;

extern int audio_mixing_active;  /* hysteresis flag: 1 once prefill met, 0 on stop/new play */

extern int xms_available;
extern uint16_t xms_handle;
extern uint16_t xms_sectsize;
extern uint32_t xms_entry;
extern uint32_t xms_reads_ok;
extern uint32_t xms_reads_err;
extern uint32_t xms_bytes_written;
extern uint32_t xms_bytes_mixed;

/* Discover modified SHSUCDX via Int 2F AX=15FFh BX=CDDAh.
 * Returns 0 on success, -1 if not found.
 */
int check_modified_shsucdx(void);

/* Convert Red Book address (MM:SS:FF packed in DWORD) to LBA */
uint32_t redbook_to_lba(uint32_t redbook);

/* Poll the SHSUCDX mailbox for play/stop/resume commands.
 * Called from ISR context - must be fast and ISR-safe.
 */
void poll_shsucdx_mailbox(void);

/* Post a request to SHSUCDX and poll for completion.
   Called from ISR context after poll_mailbox.
   Posts new reads when PLAYING and audio buffer has space;
   copies completed reads into audio buffer. */
void check_and_fill_audio_buffer(void);

/* Allocate shared slot (conventional memory) and audio buffer (extended memory).
   Call after check_modified_shsucdx() succeeds. Returns 0 on success, -1 on failure. */
int cdaudio_alloc_buffers(void);
void cdaudio_unregister(void);

/* Free shared slot buffer and CD audio buffers */
void free_all_buffers(void);

void xms_read_cycle(void);

/* Audio buffer operations - write from main code, read from ISR */
void     audio_buffer_reset(audio_buffer_struct *rb);
uint32_t audio_buffer_write(audio_buffer_struct *rb, const uint8_t *data, uint32_t len);
uint32_t audio_buffer_read(audio_buffer_struct *rb, uint8_t *dest, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif

