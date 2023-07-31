// this plugin writes to a sound card using alsa
#ifndef AYLP_ALSA_H_
#define AYLP_ALSA_H_

#include <alsa/asoundlib.h>

#include "anyloop.h"

struct aylp_alsa_data {
	snd_pcm_t *handle;
	snd_output_t *output;
	snd_pcm_hw_params_t *hwparams;
	snd_pcm_sw_params_t *swparams;
	snd_pcm_channel_area_t *areas;
	// playback device from `aplay -L` (e.g. "front")
	char *device;
	// write access method (e.g. SND_PCM_ACCESS_MMAP_INTERLEAVED)
	snd_pcm_access_t access;
	// sample format (e.g. SND_PCM_FORMAT_S16)
	snd_pcm_format_t format;
	// number of channels
	unsigned channels;
	// sample rate [Hz]
	unsigned rate;
	// requested time and returned size of buffer
	unsigned buffer_time;
	snd_pcm_uframes_t buffer_size;
	// requested time and returned size of period
	unsigned period_time;
	snd_pcm_uframes_t period_size;
	// if the pcm needs to be started
	bool needs_start;
	// TODO: check this out
	signed short *samples;
	// how many bits in our format
	int format_bits;
	// maximum unsigned value in our format
	unsigned maxval;
	// physical bits per sample (usually same as format_bits)
	int phys_bps;
	// is the requested format big endian?
	bool big_endian;
	// is the requested format unsigned?
	bool to_unsigned;
};

// initialize alsa device
int aylp_alsa_init(struct aylp_device *self);

// write vector to alsa
int aylp_alsa_process(struct aylp_device *self, struct aylp_state *state);

// close alsa device when loop exits
int aylp_alsa_close(struct aylp_device *self);

#endif

