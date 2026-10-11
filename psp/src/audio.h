// Capture of the game's sound by hooking sceAudio (resolved at runtime).
#ifndef PSPCAP_AUDIO_H
#define PSPCAP_AUDIO_H

#include <psptypes.h>

#include "pspcap_protocol.h"

// Hooks the sceAudio output and reserve syscalls. The hooks always call the
// original; they only copy PCM while capture is on, and then call `on_data`
// (on the game's audio thread, with k1 = 0). Returns the number of hooked
// functions, 0 if sceAudio_Driver is not loaded (yet).
int audio_hooks_install(void (*on_data)(void));
void audio_hooks_remove(void);
int audio_hooks_installed(void);

void audio_set_capture(int on);

// Takes the oldest captured block: fills `info` and copies the PCM into
// `pcm` (at most `max` bytes). Returns the PCM size, or 0 if nothing is queued.
int audio_pop(pspcap_audio_info *info, void *pcm, int max);

u32 audio_dropped(void);

// Fills `out->src` and `out->ring_used`; restarts the per-report fields
// (max_gap_us, min/max_samples).
void audio_diag(pspcap_audio_diag *out);

#endif
