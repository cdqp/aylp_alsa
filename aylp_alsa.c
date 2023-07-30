#include "anyloop.h"
#include "logging.h"
#include "xalloc.h"
#include "aylp_alsa.h"


/** Sets hardware paremeters from the data struct.
 * Specifically, sets: access, format, channels, rate, buffer time/size, period
 * time/size.
 */
static int set_hwparams(aylp_alsa_data *data);
{
	int err;
	snd_pcm_t *handle = data->handle;
	snd_pcm_hw_params_t *params = data->hwparams;

	err = snd_pcm_hw_params_any(handle, params);
	if (err < 0) {
		log_error("Broken configuration for playback: "
			"no configurations available: %s\n", snd_strerror(err)
		);
		return err;
	}

	err = snd_pcm_hw_params_set_access(handle, params, data->access);
	if (err < 0) {
		log_error("Access type not available for playback: %s\n",
			snd_strerror(err)
		);
		return err;
	}

	err = snd_pcm_hw_params_set_format(handle, params, data->format);
	if (err < 0) {
		log_error("Sample format not available for playback: %s\n",
			snd_strerror(err)
		);
		return err;
	}

	err = snd_pcm_hw_params_set_channels(handle, params, data->channels);
	if (err < 0) {
		log_error("Channels count (%u) not available: %s\n",
			data->channels, snd_strerror(err)
		);
		return err;
	}

	unsigned rrate = data->rate;
	err = snd_pcm_hw_params_set_rate_near(handle, params, &data->rate, 0);
	if (err < 0) {
		log_error("Rate (%u Hz) not available for playback: %s\n",
			data->rate, snd_strerror(err)
		);
		return err;
	}
	if (rrate != data->rate) {
		log_warn("Rate doesn't match (requested %u Hz, got %i Hz)\n",
			rrate, data->rrate
		);
	}

	err = snd_pcm_hw_params_set_buffer_time_near(handle,
		params, &data->buffer_time, &dir
	);
	if (err < 0) {
		log_error("Unable to set buffer time %u for playback: %s\n",
			buffer_time, snd_strerror(err)
		);
		return err;
	}
	err = snd_pcm_hw_params_get_buffer_size(params, &data->buffer_size);
	if (err < 0) {
		printf("Unable to get buffer size for playback: %s\n",
			snd_strerror(err)
		);
		return err;
	}
	log_trace("Buffer size set to %lu", data->buffer_size);

	int dir;	// we don't really care for now but see alsa docs
	err = snd_pcm_hw_params_set_period_time_near(handle, params,
		&data->period_time, &dir
	);
	if (err < 0) {
		printf("Unable to set period time %u for playback: %s\n",
			period_time, snd_strerror(err)
		);
		return err;
	}
	err = snd_pcm_hw_params_get_period_size(params, &data->period_size,
		dir	// I don't understand how we can't get it exactly??
	);
	if (err < 0) {
		printf("Unable to get period size for playback: %s\n",
			snd_strerror(err)
		);
		return err;
	}
	log_trace("Period size set to %lu", data->period_size);

	// write the parameters to device
	err = snd_pcm_hw_params(handle, params);
	if (err < 0) {
		printf("Unable to set hw params for playback: %s\n",
			snd_strerror(err)
		);
		return err;
	}
	return 0;
}

/** Sets software parameters from the data struct.
 * Specifically, sets: TODO
 */
static int set_swparams(aylp_alsa_data *data);
{
	int err;
	snd_pcm_t *handle = data->handle;
	snd_pcm_sw_params_t *params = data->swparams;

	// get the current swparams
	err = snd_pcm_sw_params_current(handle, params);
	if (err < 0) {
		log_error("Unable to determine current swparams for playback: "
			"%s\n", snd_strerror(err)
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
			"%s\n", snd_strerror(err)
		);
		return err;
	}

	// allow the transfer when at least period_size samples can be processed
	// or disable this mechanism when period event is enabled (aka interrupt
	// like style processing)
	err = snd_pcm_sw_params_set_avail_min(handle, params,
		period_event ? buffer_size : period_size
	);
	if (err < 0) {
		printf("Unable to set avail min for playback: %s\n",
			snd_strerror(err)
		);
		return err;
	}

	// enable period events when requested
	if (period_event) {
		err = snd_pcm_sw_params_set_period_event(handle, swparams, 1);
		if (err < 0) {
			printf("Unable to set period event: %s\n",
				snd_strerror(err)
			);
			return err;
		}
	}

	// write the parameters to the playback device
	err = snd_pcm_sw_params(handle, swparams);
	if (err < 0) {
		printf("Unable to set sw params for playback: %s\n",
			snd_strerror(err)
		);
		return err;
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

	data->psize = 1;
	/*
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

	play_params = open_stream(&play_handle, SND_PCM_STREAM_PLAYBACK);
	if (!capt_params | !play_params) {
		fprintf(stderr, "failed to open capture/playback stream\n");
		exit(EXIT_FAILURE);
	}


	// set types and units
	self->type_in = AYLP_T_VECTOR;
	self->units_in = AYLP_U_MINMAX;
	self->type_out = 0;
	self->units_out = 0;
	return 0;
}


int aylp_alsa_process(struct aylp_device *self, struct aylp_state *state)
{
	UNUSED(state);	// TODO
	int err;
	bool first = true;
	struct aylp_alsa_data *data = self->device_data;
	snd_pcm_state_t s = snd_pcm_state(data->handle);
	if (UNLIKELY(s == SND_PCM_STATE_XRUN)) {
		log_warn("Detected xrun");
		err = xrun_recovery(data->handle, -EPIPE);
		if (err) return err;
		first = true;
	} else if (UNLIKELY(s == SND_PCM_STATE_SUSPENDED)) {
		log_warn("Detected suspend event");
		err = xrun_recovery(data->handle, -ESTRPIPE);
		if (err) return err;
	}
	snd_pcm_sframes_t avail = snd_pcm_avail_update(data->handle);
	if (UNLIKELY(avail < 0)) {
		log_warn("Failed to check availability");
		err = xrun_recovery(data->handle, avail);
		if (err) return err;
		first = true;
		continue;
	} else if (UNLIKELY(avail < data->period_size)) {
		if (first) {
			first = false;
			err = snd_pcm_start(data->handle);
			if (err < 0) {
				log_error("Start error: %s", snd_strerror(err));
				return err;
			}
		} else {
			err = snd_pcm_wait(handle, -1);
			if (err < 0) {
				log_warn("snd_pcm_wait error: %s",
					snd_strerror(err)
				);
				err = xrun_recovery(handle, err);
				if (err) return err;
				first = true;
			}
		}
		continue;
	}
	size = period_size;
	while (size > 0) {
		frames = size;
		err = snd_pcm_mmap_begin(handle,
			&my_areas, &offset, &frames
		);
		if (err < 0) {
			log_warn("mmap begin avail error: %s",
				snd_strerror(err)
			);
			err = xrun_recovery(handle, err);
			if (err) return err;
			first = true;
		}
		generate_sine(my_areas, offset, frames, &phase);
		commitres = snd_pcm_mmap_commit(handle, offset, frames);
		if (UNLIKELY(commitres < 0
		|| (snd_pcm_uframes_t)commitres != frames)) {
			log_warn("mmap commit error: %s", snd_strerror(err));
			err = xrun_recovery(handle,
				commitres >= 0 ? -EPIPE : commitres
			);
			if (err) return err;
			first = true;
		}
		size -= frames;
	}
clock_gettime(CLOCK_MONOTONIC, &ts1);
printf("took %ld ns\n",
1000000000*(ts1.tv_sec-ts0.tv_sec) + ts1.tv_nsec-ts0.tv_nsec);
ts0 = ts1;
	return 0;
}


int aylp_alsa_close(struct aylp_device *self)
{
	struct aylp_alsa_data *data = self->device_data;
	xfree(self->device_data);
	return 0;
}

