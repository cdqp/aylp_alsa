#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <time.h>
#include <errno.h>
#include <getopt.h>
#include <alsa/asoundlib.h>
#include <sys/time.h>
#include <math.h>

#define LIKELY(x)	__builtin_expect((x),1)
#define UNLIKELY(x)	__builtin_expect((x),0)

#ifndef ESTRPIPE
#define ESTRPIPE ESPIPE
#endif

static char *device = "front";	// playback device
static snd_pcm_format_t format = SND_PCM_FORMAT_S16;	// sample format
static unsigned int rate = 44100;	// stream rate
static unsigned int channels = 2;	// count of channels
static unsigned int buffer_time = 0;	// ring buffer length in us
static unsigned int period_time = 0;	// period time in us
static double freq = 400;		// sinusoidal wave frequency in Hz
static int resample = 1;	// enable alsa-lib resampling
static int period_event = 0;	// produce poll event after each period
static int acc = 0;

static snd_pcm_sframes_t buffer_size;
static snd_pcm_sframes_t period_size;
static snd_output_t *output = NULL;

static void generate_sine(const snd_pcm_channel_area_t *areas,
	snd_pcm_uframes_t offset, int count, double *_phase
){
	static double max_phase = 2. * M_PI;
	double phase = *_phase;
	double step = max_phase*freq/(double)rate;
	unsigned char *samples[channels];
	int steps[channels];
	unsigned int chn;
	int format_bits = snd_pcm_format_width(format);
	unsigned int maxval = (1 << (format_bits - 1)) - 1;
	int bps = format_bits / 8;  /* bytes per sample */
	int phys_bps = snd_pcm_format_physical_width(format) / 8;
	int big_endian = snd_pcm_format_big_endian(format) == 1;
	int to_unsigned = snd_pcm_format_unsigned(format) == 1;

	/* verify and prepare the contents of areas */
	for (chn = 0; chn < channels; chn++) {
		if ((areas[chn].first % 8) != 0) {
			printf("areas[%u].first == %u, aborting...\n",
				chn, areas[chn].first
			);
			exit(EXIT_FAILURE);
		}
		samples[chn] = (((unsigned char *)areas[chn].addr)
			+ (areas[chn].first / 8));
		if ((areas[chn].step % 16) != 0) {
			printf("areas[%u].step == %u, aborting...\n",
				chn, areas[chn].step
			);
			exit(EXIT_FAILURE);
		}
		steps[chn] = areas[chn].step / 8;
		samples[chn] += offset * steps[chn];
	}
	/* fill the channel areas */
	while (count-- > 0) {
		int res, i;
		//res = (0.5 + 0.5 * sin(phase)) * maxval;
		res = (0.7*sin(0.00001 * acc) + 0.1*sin(phase)) * maxval;
		acc++;
		if (to_unsigned) res ^= 1U << (format_bits - 1);
		for (chn = 0; chn < channels; chn++) {
			if (big_endian) {
				for (i = 0; i < bps; i++)
					*(samples[chn] + phys_bps - 1 - i)
						= (res >> i * 8) & 0xff;
			} else {
				for (i = 0; i < bps; i++)
					*(samples[chn] + i)
						= (res >>  i * 8) & 0xff;
			}
			samples[chn] += steps[chn];
		}
		phase += step;
		if (phase >= max_phase)
			phase -= max_phase;
	}
	*_phase = phase;
}

static int set_hwparams(snd_pcm_t *handle,
		snd_pcm_hw_params_t *params,
		snd_pcm_access_t access)
{
	unsigned int rrate;
	snd_pcm_uframes_t size;
	int err, dir;

	// choose all parameters
	err = snd_pcm_hw_params_any(handle, params);
	if (err < 0) {
		printf("Broken configuration for playback: no "
			"configurations available: %s\n", snd_strerror(err)
		);
		return err;
	}
	// set the interleaved read/write format
	err = snd_pcm_hw_params_set_access(handle, params, access);
	if (err < 0) {
		printf("Access type not available for playback: %s\n",
			snd_strerror(err)
		);
		return err;
	}
	// set the sample format
	err = snd_pcm_hw_params_set_format(handle, params, format);
	if (err < 0) {
		printf("Sample format not available for playback: %s\n",
			snd_strerror(err)
		);
		return err;
	}
	// set the count of channels
	err = snd_pcm_hw_params_set_channels(handle, params, channels);
	if (err < 0) {
		printf("Channels count (%u) not available for playbacks: %s\n",
			channels, snd_strerror(err)
		);
		return err;
	}
	// set the stream rate
	rrate = rate;
	err = snd_pcm_hw_params_set_rate_near(handle, params, &rrate, 0);
	if (err < 0) {
		printf("Rate %uHz not available for playback: %s\n", rate,
			snd_strerror(err)
		);
		return err;
	}
	if (rrate != rate) {
		printf("Rate doesn't match (requested %uHz, get %iHz)\n",
			rate, err
		);
		return -EINVAL;
	}
	// set the buffer time
	err = snd_pcm_hw_params_set_buffer_time_near(handle,
		params, &buffer_time, &dir
	);
	if (err < 0) {
		printf("Unable to set buffer time %u for playback: %s\n",
			buffer_time, snd_strerror(err)
		);
		return err;
	}
	err = snd_pcm_hw_params_get_buffer_size(params, &size);
	if (err < 0) {
		printf("Unable to get buffer size for playback: %s\n",
			snd_strerror(err)
		);
		return err;
	}
	buffer_size = size;
	printf("buffer size: %ld\n", buffer_size);
	// set the period time
	err = snd_pcm_hw_params_set_period_time_near(handle, params,
		&period_time, &dir
	);
	if (err < 0) {
		printf("Unable to set period time %u for playback: %s\n",
			period_time, snd_strerror(err)
		);
		return err;
	}
	err = snd_pcm_hw_params_get_period_size(params, &size, &dir);
	if (err < 0) {
		printf("Unable to get period size for playback: %s\n",
			snd_strerror(err)
		);
		return err;
	}
	period_size = size;
	printf("period size: %ld\n", period_size);
	/* write the parameters to device */
	err = snd_pcm_hw_params(handle, params);
	if (err < 0) {
		printf("Unable to set hw params for playback: %s\n",
			snd_strerror(err)
		);
		return err;
	}
	return 0;
}

static int set_swparams(snd_pcm_t *handle, snd_pcm_sw_params_t *swparams)
{
	int err;

	// get the current swparams
	err = snd_pcm_sw_params_current(handle, swparams);
	if (err < 0) {
		printf("Unable to determine current swparams for playback:"
			" %s\n", snd_strerror(err)
		);
		return err;
	}
	// start the transfer when the buffer is almost full:
	// (buffer_size / avail_min) * avail_min
	err = snd_pcm_sw_params_set_start_threshold(handle, swparams,
		(buffer_size / period_size) * period_size
	);
	if (err < 0) {
		printf("Unable to set start threshold mode for playback: %s\n",
			snd_strerror(err)
		);
		return err;
	}
	// allow the transfer when at least period_size samples can be processed
	// or disable this mechanism when period event is enabled (aka interrupt
	// like style processing)
	err = snd_pcm_sw_params_set_avail_min(handle, swparams,
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

static int xrun_recovery(snd_pcm_t *handle, int err)
{
	printf("stream recovery\n");
	if (err == -EPIPE) {    /* under-run */
		err = snd_pcm_prepare(handle);
		if (err < 0)
			printf("Can't recovery from underrun, prepare failed:"
				" %s\n", snd_strerror(err)
			);
		return 0;
	} else if (err == -ESTRPIPE) {
		while ((err = snd_pcm_resume(handle)) == -EAGAIN)
			// wait until the suspend flag is released
			sleep(1);
		if (err < 0) {
			err = snd_pcm_prepare(handle);
			if (err < 0)
				printf("Can't recovery from suspend, "
					"prepare failed: %s\n",
					snd_strerror(err)
				);
		}
		return 0;
	}
	return err;
}

static int direct_loop(snd_pcm_t *handle,
	signed short *samples ATTRIBUTE_UNUSED,
	snd_pcm_channel_area_t *areas ATTRIBUTE_UNUSED
){
	double phase = 0;
	const snd_pcm_channel_area_t *my_areas;
	snd_pcm_uframes_t offset, frames, size;
	snd_pcm_sframes_t avail, commitres;
	snd_pcm_state_t state;
	int err, first = 1;

	struct timespec ts0;
	struct timespec ts1;
	clock_gettime(CLOCK_MONOTONIC, &ts0);

	while (1) {
//printf("anotha loop 1\n");
		state = snd_pcm_state(handle);
		if (UNLIKELY(state == SND_PCM_STATE_XRUN)) {
			printf("xrun\n");
			err = xrun_recovery(handle, -EPIPE);
			if (err < 0) {
				printf("XRUN recovery failed: %s\n",
					snd_strerror(err)
				);
				return err;
			}
			first = 1;
		} else if (UNLIKELY(state == SND_PCM_STATE_SUSPENDED)) {
			printf("suspended\n");
			err = xrun_recovery(handle, -ESTRPIPE);
			if (err < 0) {
				printf("SUSPEND recovery failed: %s\n",
					snd_strerror(err)
				);
				return err;
			}
		}
		avail = snd_pcm_avail_update(handle);
		if (UNLIKELY(avail < 0)) {
			printf("unavail\n");
			err = xrun_recovery(handle, avail);
			if (err < 0) {
				printf("avail update failed: %s\n",
					snd_strerror(err)
				);
				return err;
			}
			first = 1;
			continue;
		}
		if (UNLIKELY(avail < period_size)) {
			if (first) {
				first = 0;
				err = snd_pcm_start(handle);
				if (err < 0) {
					printf("Start error: %s\n",
						snd_strerror(err)
					);
					exit(EXIT_FAILURE);
				}
			} else {
				err = snd_pcm_wait(handle, -1);
				if (err < 0) {
					printf("waiterr\n");
					err = xrun_recovery(handle, err);
					if (err < 0) {
						printf("snd_pcm_wait error: "
							"%s\n",
							snd_strerror(err)
						);
						exit(EXIT_FAILURE);
					}
					first = 1;
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
				printf("mmapbeginerr\n");
				if ((err = xrun_recovery(handle, err)) < 0) {
					printf("MMAP begin avail error: %s\n",
						snd_strerror(err)
					);
					exit(EXIT_FAILURE);
				}
				first = 1;
			}
//printf("anotha loop 2\n");
			generate_sine(my_areas, offset, frames, &phase);
			commitres = snd_pcm_mmap_commit(handle, offset, frames);
			if (UNLIKELY(commitres < 0
			|| (snd_pcm_uframes_t)commitres != frames)) {
				printf("commiterr\n");
				err = xrun_recovery(handle,
					commitres >= 0 ? -EPIPE : commitres
				);
				if (err < 0) {
					printf("MMAP commit error: %s\n",
						snd_strerror(err)
					);
					exit(EXIT_FAILURE);
				}
				first = 1;
			}
			size -= frames;
		}
clock_gettime(CLOCK_MONOTONIC, &ts1);
printf("took %ld ns\n",
1000000000*(ts1.tv_sec-ts0.tv_sec) + ts1.tv_nsec-ts0.tv_nsec);
ts0 = ts1;
	}
}


int main(int argc, char *argv[])
{
	int err;
	snd_pcm_t *handle;
	snd_pcm_hw_params_t *hwparams;
	snd_pcm_sw_params_t *swparams;
	signed short *samples;
	unsigned int chn;
	snd_pcm_channel_area_t *areas;

	snd_pcm_hw_params_alloca(&hwparams);
	snd_pcm_sw_params_alloca(&swparams);

	err = snd_output_stdio_attach(&output, stdout, 0);
	if (err < 0) {
		printf("Output failed: %s\n", snd_strerror(err));
		return 0;
	}

	printf("Playback device is %s\n", device);
	printf("Stream parameters are %uHz, %s, %u channels\n",
		rate, snd_pcm_format_name(format), channels
	);
	printf("Sine wave rate is %.4fHz\n", freq);

	err = snd_pcm_open(&handle, device, SND_PCM_STREAM_PLAYBACK, 0);
	if (err < 0) {
		printf("Playback open error: %s\n", snd_strerror(err));
		return 0;
	}

	err = set_hwparams(handle, hwparams, SND_PCM_ACCESS_MMAP_INTERLEAVED);
	if (err < 0) {
		printf("Setting of hwparams failed: %s\n", snd_strerror(err));
		exit(EXIT_FAILURE);
	}
	if ((err = set_swparams(handle, swparams)) < 0) {
		printf("Setting of swparams failed: %s\n", snd_strerror(err));
		exit(EXIT_FAILURE);
	}

	snd_pcm_dump(handle, output);

	samples = malloc((period_size * channels
		* snd_pcm_format_physical_width(format)) / 8);
	if (samples == NULL) {
		printf("No enough memory\n");
		exit(EXIT_FAILURE);
	}

	areas = calloc(channels, sizeof(snd_pcm_channel_area_t));
	if (areas == NULL) {
		printf("No enough memory\n");
		exit(EXIT_FAILURE);
	}
	for (chn = 0; chn < channels; chn++) {
		areas[chn].addr = samples;
		areas[chn].first = chn * snd_pcm_format_physical_width(format);
		areas[chn].step = channels
			* snd_pcm_format_physical_width(format);
	}

	err = direct_loop(handle, samples, areas);
	if (err < 0)
		printf("Transfer failed: %s\n", snd_strerror(err));

	free(areas);
	free(samples);
	snd_pcm_close(handle);
	return 0;
}

