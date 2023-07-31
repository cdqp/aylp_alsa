#include <unistd.h>
#include <alsa/asoundlib.h>

#include "anyloop.h"
#include "logging.h"
#include "xalloc.h"
#include "aylp_alsa.h"


/** Sets hardware parameters from the data struct.
 * Specifically, sets: access, format, channels, rate, buffer time/size, period
 * time/size.
 */
static int set_hwparams(struct aylp_alsa_data *data)
{
	int err;
	int dir;	// we don't really care for now but see alsa docs
	snd_pcm_t *handle = data->handle;
	snd_pcm_hw_params_t *params;
	snd_pcm_hw_params_alloca(&params);

	err = snd_pcm_hw_params_any(handle, params);
	if (err < 0) {
		log_error("Broken configuration for playback: "
			"no configurations available: %s", snd_strerror(err)
		);
		return err;
	}

	err = snd_pcm_hw_params_set_access(handle, params, data->access);
	if (err < 0) {
		log_error("Access type not available for playback: %s",
			snd_strerror(err)
		);
		return err;
	}

	err = snd_pcm_hw_params_set_format(handle, params, data->format);
	if (err < 0) {
		log_error("Sample format not available for playback: %s",
			snd_strerror(err)
		);
		return err;
	}

	err = snd_pcm_hw_params_set_channels(handle, params, data->channels);
	if (err < 0) {
		log_error("Channels count (%u) not available: %s",
			data->channels, snd_strerror(err)
		);
		return err;
	}

	unsigned rrate = data->rate;
	err = snd_pcm_hw_params_set_rate_near(handle, params, &data->rate, 0);
	if (err < 0) {
		log_error("Rate (%u Hz) not available for playback: %s",
			data->rate, snd_strerror(err)
		);
		return err;
	}
	if (rrate != data->rate) {
		log_warn("Rate doesn't match (requested %u Hz, got %i Hz)",
			rrate, data->rate
		);
	}

	err = snd_pcm_hw_params_set_buffer_time_near(handle,
		params, &data->buffer_time, &dir
	);
	if (err < 0) {
		log_error("Unable to set buffer time %u for playback: %s",
			data->buffer_time, snd_strerror(err)
		);
		return err;
	}
	err = snd_pcm_hw_params_get_buffer_size(params, &data->buffer_size);
	if (err < 0) {
		log_error("Unable to get buffer size for playback: %s",
			snd_strerror(err)
		);
		return err;
	}
	log_trace("Buffer size set to %lu", data->buffer_size);

	err = snd_pcm_hw_params_set_period_time_near(handle, params,
		&data->period_time, &dir
	);
	if (err < 0) {
		log_error("Unable to set period time %u for playback: %s",
			data->period_time, snd_strerror(err)
		);
		return err;
	}
	err = snd_pcm_hw_params_get_period_size(params, &data->period_size,
		&dir	// I don't understand how we can't get it exactly??
	);
	if (err < 0) {
		log_error("Unable to get period size for playback: %s",
			snd_strerror(err)
		);
		return err;
	}
	log_trace("Period size set to %lu", data->period_size);

	// write the parameters to device
	err = snd_pcm_hw_params(handle, params);
	if (err < 0) {
		log_error("Unable to set hw params for playback: %s",
			snd_strerror(err)
		);
		return err;
	}

	// https://github.com/alsa-project/alsa-lib/issues/341
	//snd_pcm_hw_params_free(params);
	return 0;
}

/** Sets software parameters based on hardware parameters. */
static int set_swparams(struct aylp_alsa_data *data)
{
	int err;
	snd_pcm_t *handle = data->handle;
	snd_pcm_sw_params_t *params;
	snd_pcm_sw_params_alloca(&params);

	// get the current swparams
	err = snd_pcm_sw_params_current(handle, params);
	if (err < 0) {
		log_error("Unable to determine current swparams for playback: "
			"%s", snd_strerror(err)
		);
		return err;
	}

	// start the transfer when the buffer is almost full:
	// (buffer_size / avail_min) * avail_min
	err = snd_pcm_sw_params_set_start_threshold(handle, params,
		(data->buffer_size / data->period_size) * data->period_size
	);
	if (err < 0) {
		log_error("Unable to set start threshold mode for playback: "
			"%s", snd_strerror(err)
		);
		return err;
	}

	// allow the transfer when at least period_size samples can be processed
	err = snd_pcm_sw_params_set_avail_min(handle, params,
		data->period_size
	);
	if (err < 0) {
		log_error("Unable to set avail min for playback: %s",
			snd_strerror(err)
		);
		return err;
	}

	// since we expect to underrun arbitrarily often, disable xrun check
	snd_pcm_uframes_t boundary;
	snd_pcm_sw_params_get_boundary(params, &boundary);
	snd_pcm_sw_params_set_stop_threshold(handle, params, boundary);

	// write the parameters to the playback device
	err = snd_pcm_sw_params(handle, params);
	if (err < 0) {
		log_error("Unable to set sw params for playback: %s",
			snd_strerror(err)
		);
		return err;
	}

	// https://github.com/alsa-project/alsa-lib/issues/341
	//snd_pcm_sw_params_free(params);
	return 0;
}


/** Process one period according to data and state. */
static int process_period(struct aylp_alsa_data *data, struct aylp_state *state)
{
	int err;
	// check for suspend event
	if (UNLIKELY(snd_pcm_state(data->handle) == SND_PCM_STATE_SUSPENDED)) {
		log_warn("Detected suspend event");
		// wait until suspend flag is released
		while ((err = snd_pcm_resume(data->handle)) == -EAGAIN)
			sleep(1);
		if (err < 0) {
			err = snd_pcm_prepare(data->handle);
			if (err < 0) {
				log_error("Can't recover from suspend; prepare "
					"failed: %s", snd_strerror(err)
				);
				return err;
			}
		}
	}

	// make sure we have a period available
	snd_pcm_uframes_t avail = snd_pcm_avail_update(data->handle);
	if (UNLIKELY((snd_pcm_sframes_t)avail < 0)) {
		log_warn("Failed to check availability: %s",
			snd_strerror(avail)
		);
		data->needs_start = true;
		return avail;
	} else if (UNLIKELY(avail < data->period_size)) {
		if (data->needs_start) {
			data->needs_start = false;
			log_trace("Starting pcm");
			err = snd_pcm_start(data->handle);
			if (err < 0) {
				log_error("Start error: %s", snd_strerror(err));
				return err;
			}
		} else {
			err = snd_pcm_wait(data->handle, -1);
			if (err < 0) {
				log_warn("snd_pcm_wait error: %s",
					snd_strerror(err)
				);
				data->needs_start = true;
				return err;
			}
		}
		return 0;
		// Instead of return 0 above, we could put this whole thing in a
		// loop and simply `continue`. Depends on if we want to wait for
		// ALSA to be ready to take our data or if we just want to get
		// on with the loop.
	}

	// write frames
	snd_pcm_uframes_t offset, frames, size;
	const snd_pcm_channel_area_t *my_areas;
	size = data->period_size;
	while (size > 0) {
		frames = size;
		err = snd_pcm_mmap_begin(data->handle,
			&my_areas, &offset, &frames
		);
		if (err < 0) {
			log_error("mmap_begin error: %s", snd_strerror(err));
			data->needs_start = true;
			return err;
		}
		unsigned char *samples[data->channels];
		// verify and prepare the contents of areas
		for (unsigned c = 0; c < data->channels; c++) {
			// check that offset to first sample is integer number
			// of bytes
			if (my_areas[c].first & 0x8) {
				log_error("areas[%u].first == %u, aborting",
					c, my_areas[c].first
				);
				return -1;
			}
			samples[c] = (unsigned char *)my_areas[c].addr
				+ (my_areas[c].first / 8);
			// check that step size is integer number of shorts
			if (my_areas[c].step & 0xF) {
				log_error("areas[%u].step == %u, aborting",
					c, my_areas[c].step
				);
				return -1;
			}
			samples[c] += offset * my_areas[c].step/8;
		}
		// fill the channel areas
		for (int count = frames-1; count >= 0; count--) {
			for (unsigned c = 0; c < data->channels; c++) {
				double f = state->vector->data[c];
				int res = data->maxval;
				if (data->to_unsigned) res *= f/2+0.5;
				else res *= f/2;
				if (UNLIKELY(data->big_endian)) {
					for (int i=0; i < data->format_bits/8;
					i++) {
						*(samples[c]
						+ data->phys_bps - 1 - i)
							= (res >> i*8) & 0xFF;
					}
				} else {
					for (int i=0; i < data->format_bits/8;
					i++) {
						*(samples[c] + i)
							= (res >> i*8) & 0xFF;
					}
				}
				samples[c] += my_areas[c].step/8;
			}
		}

		snd_pcm_sframes_t err = snd_pcm_mmap_commit(data->handle,
			offset, frames
		);
		if (UNLIKELY(err < 0 || (snd_pcm_uframes_t)err != frames)) {
			log_warn("mmap_commit error: %s", snd_strerror(err));
			data->needs_start = true;
			return err;
		}
		size -= frames;
	}
	return 0;
}


int aylp_alsa_init(struct aylp_device *self)
{
	int err;
	self->device_data = xcalloc(1, sizeof(struct aylp_alsa_data));
	struct aylp_alsa_data *data = self->device_data;
	// attach methods
	self->process = &aylp_alsa_process;
	self->close = &aylp_alsa_close;

	// default params
	data->device = "front";
	data->access = SND_PCM_ACCESS_MMAP_INTERLEAVED;
	data->format = SND_PCM_FORMAT_S16;
	data->channels = 2;
	data->rate = 44100;
	data->buffer_time = 0;
	data->period_time = 0;
	/* TODO
	// parse the params json into our data struct
	if (!self->params) {
		log_error("No params object found.");
		return -1;
	}
	json_object_object_foreach(self->params, key, val) {
		if (key[0] == '_') {
			// keys starting with _ are comments
		} else if (!strcmp(key, "?")) {
			// ?
		} else {
			log_warn("Unknown parameter \"%s\"", key);
		}
	}
	// enforce required params
	if (! ?) {
		log_error("You must provide at least the ? param.");
		return -1;
	}
	*/

	err = snd_output_stdio_attach(&data->output, stdout, 0);
	if (err < 0) {
		log_error("Output failed: %s", snd_strerror(err));
		return 0;
	}

	log_trace("Stream parameters are %u Hz, %s, %u channels",
		data->rate, snd_pcm_format_name(data->format), data->channels
	);

	err = snd_pcm_open(&data->handle, data->device,
		SND_PCM_STREAM_PLAYBACK, 0
	);
	if (err < 0) {
		log_error("Playback open error: %s", snd_strerror(err));
		return 0;
	}

	err = set_hwparams(data);
	if (err) {
		log_error("Setting of hwparams failed: %s", snd_strerror(err));
		exit(EXIT_FAILURE);
	}
	err = set_swparams(data);
	if (err) {
		log_error("Setting of swparams failed: %s", snd_strerror(err));
		exit(EXIT_FAILURE);
	}

	if (log_get_level() >= LOG_TRACE)
		snd_pcm_dump(data->handle, data->output);

	data->samples = xmalloc((data->period_size * data->channels
		* snd_pcm_format_physical_width(data->format)) / 8
	);
	data->areas = xcalloc(data->channels, sizeof(snd_pcm_channel_area_t));

	for (unsigned c = 0; c < data->channels; c++) {
		data->areas[c].addr = data->samples;
		data->areas[c].first = c
			* snd_pcm_format_physical_width(data->format);
		data->areas[c].step = data->channels
			* snd_pcm_format_physical_width(data->format);
	}

	data->needs_start = true;
	data->format_bits = snd_pcm_format_width(data->format);
	data->maxval = (1 << (data->format_bits - 1)) - 1;
	data->phys_bps = snd_pcm_format_physical_width(data->format) / 8;
	data->big_endian = snd_pcm_format_big_endian(data->format);
	data->to_unsigned = snd_pcm_format_unsigned(data->format);

	// set types and units
	self->type_in = AYLP_T_VECTOR;
	self->units_in = AYLP_U_MINMAX;
	self->type_out = 0;
	self->units_out = 0;
	return 0;
}


int aylp_alsa_process(struct aylp_device *self, struct aylp_state *state)
{
	int err;
	struct aylp_alsa_data *data = self->device_data;
	for (unsigned p = 0; p < data->buffer_size / data->period_size; p++) {
		log_trace("Processing period %u", p);
		err = process_period(data, state);
		if (err) return err;
	}
	return 0;
}


int aylp_alsa_close(struct aylp_device *self)
{
	struct aylp_alsa_data *data = self->device_data;
	snd_pcm_close(data->handle);
	xfree(data->areas);
	xfree(data->samples);
	xfree(self->device_data);
	return 0;
}

