#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <errno.h>
#include <getopt.h>
#include <sys/time.h>
#include <math.h>
#include <limits.h>
#include <sys/timerfd.h>

#include <spa/pod/filter.h>
#include <spa/control/control.h>

#include "alsa-utils.h"

#define CHECK(s,msg) if ((err = (s)) < 0) { spa_log_error(state->log, msg ": %s", snd_strerror(err)); return err; }

static int spa_alsa_open(struct state *state)
{
	int err;
	struct props *props = &state->props;

	if (state->opened)
		return 0;

	CHECK(snd_output_stdio_attach(&state->output, stderr, 0), "attach failed");

	spa_log_info(state->log, "%p: ALSA device open '%s'", state, props->device);
	CHECK(snd_pcm_open(&state->hndl,
			   props->device,
			   state->stream,
			   SND_PCM_NONBLOCK |
			   SND_PCM_NO_AUTO_RESAMPLE |
			   SND_PCM_NO_AUTO_CHANNELS | SND_PCM_NO_AUTO_FORMAT), "open failed");

	state->timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	state->opened = true;
	state->sample_count = 0;
	state->sample_time = 0;

	return 0;
}

int spa_alsa_close(struct state *state)
{
	int err = 0;

	if (!state->opened)
		return 0;

	spa_log_info(state->log, "%p: Device '%s' closing", state, state->props.device);
	CHECK(snd_pcm_close(state->hndl), "close failed");

	close(state->timerfd);
	state->opened = false;

	return err;
}

struct format_info {
	uint32_t spa_format;
	uint32_t spa_pformat;
	snd_pcm_format_t format;
};

static const struct format_info format_info[] = {
	{ SPA_AUDIO_FORMAT_UNKNOWN, SPA_AUDIO_FORMAT_UNKNOWN, SND_PCM_FORMAT_UNKNOWN},
	{ SPA_AUDIO_FORMAT_F32_LE, SPA_AUDIO_FORMAT_F32P, SND_PCM_FORMAT_FLOAT_LE},
	{ SPA_AUDIO_FORMAT_F32_BE, SPA_AUDIO_FORMAT_F32P, SND_PCM_FORMAT_FLOAT_BE},
	{ SPA_AUDIO_FORMAT_S32_LE, SPA_AUDIO_FORMAT_S32P, SND_PCM_FORMAT_S32_LE},
	{ SPA_AUDIO_FORMAT_S32_BE, SPA_AUDIO_FORMAT_S32P, SND_PCM_FORMAT_S32_BE},
	{ SPA_AUDIO_FORMAT_S24_32_LE, SPA_AUDIO_FORMAT_S24_32P, SND_PCM_FORMAT_S24_LE},
	{ SPA_AUDIO_FORMAT_S24_32_BE, SPA_AUDIO_FORMAT_S24_32P, SND_PCM_FORMAT_S24_BE},
	{ SPA_AUDIO_FORMAT_S16_LE, SPA_AUDIO_FORMAT_S16P, SND_PCM_FORMAT_S16_LE},
	{ SPA_AUDIO_FORMAT_S16_BE, SPA_AUDIO_FORMAT_S16P, SND_PCM_FORMAT_S16_BE},
	{ SPA_AUDIO_FORMAT_S24_LE, SPA_AUDIO_FORMAT_S24P, SND_PCM_FORMAT_S24_3LE},
	{ SPA_AUDIO_FORMAT_S24_BE, SPA_AUDIO_FORMAT_S24P, SND_PCM_FORMAT_S24_3BE},
	{ SPA_AUDIO_FORMAT_S8, SPA_AUDIO_FORMAT_UNKNOWN, SND_PCM_FORMAT_S8},
	{ SPA_AUDIO_FORMAT_U8, SPA_AUDIO_FORMAT_U8P, SND_PCM_FORMAT_U8},
	{ SPA_AUDIO_FORMAT_U16_LE, SPA_AUDIO_FORMAT_UNKNOWN, SND_PCM_FORMAT_U16_LE},
	{ SPA_AUDIO_FORMAT_U16_BE, SPA_AUDIO_FORMAT_UNKNOWN, SND_PCM_FORMAT_U16_BE},
	{ SPA_AUDIO_FORMAT_U24_32_LE, SPA_AUDIO_FORMAT_UNKNOWN, SND_PCM_FORMAT_U24_LE},
	{ SPA_AUDIO_FORMAT_U24_32_BE, SPA_AUDIO_FORMAT_UNKNOWN, SND_PCM_FORMAT_U24_BE},
	{ SPA_AUDIO_FORMAT_U24_LE, SPA_AUDIO_FORMAT_UNKNOWN, SND_PCM_FORMAT_U24_3LE},
	{ SPA_AUDIO_FORMAT_U24_BE, SPA_AUDIO_FORMAT_UNKNOWN, SND_PCM_FORMAT_U24_3BE},
	{ SPA_AUDIO_FORMAT_U32_LE, SPA_AUDIO_FORMAT_UNKNOWN, SND_PCM_FORMAT_U32_LE},
	{ SPA_AUDIO_FORMAT_U32_BE, SPA_AUDIO_FORMAT_UNKNOWN, SND_PCM_FORMAT_U32_BE},
	{ SPA_AUDIO_FORMAT_F64_LE, SPA_AUDIO_FORMAT_F64P, SND_PCM_FORMAT_FLOAT64_LE},
	{ SPA_AUDIO_FORMAT_F64_BE, SPA_AUDIO_FORMAT_F64P, SND_PCM_FORMAT_FLOAT64_BE},
};

static snd_pcm_format_t spa_format_to_alsa(uint32_t format)
{
	size_t i;

	for (i = 0; i < SPA_N_ELEMENTS(format_info); i++) {
		if (format_info[i].spa_format == format)
			return format_info[i].format;
	}
	return SND_PCM_FORMAT_UNKNOWN;
}

struct chmap_info {
	enum snd_pcm_chmap_position pos;
	enum spa_audio_channel channel;
};

static const struct chmap_info chmap_info[] = {
	[SND_CHMAP_UNKNOWN] = { SND_CHMAP_UNKNOWN, SPA_AUDIO_CHANNEL_UNKNOWN },
	[SND_CHMAP_NA] = { SND_CHMAP_NA, SPA_AUDIO_CHANNEL_NA },
	[SND_CHMAP_MONO] = { SND_CHMAP_MONO, SPA_AUDIO_CHANNEL_MONO },
	[SND_CHMAP_FL] = { SND_CHMAP_FL, SPA_AUDIO_CHANNEL_FL },
	[SND_CHMAP_FR] = { SND_CHMAP_FR, SPA_AUDIO_CHANNEL_FR },
	[SND_CHMAP_RL] = { SND_CHMAP_RL, SPA_AUDIO_CHANNEL_RL },
	[SND_CHMAP_RR] = { SND_CHMAP_RR, SPA_AUDIO_CHANNEL_RR },
	[SND_CHMAP_FC] = { SND_CHMAP_FC, SPA_AUDIO_CHANNEL_FC },
	[SND_CHMAP_LFE] = { SND_CHMAP_LFE, SPA_AUDIO_CHANNEL_LFE },
	[SND_CHMAP_SL] = { SND_CHMAP_SL, SPA_AUDIO_CHANNEL_SL },
	[SND_CHMAP_SR] = { SND_CHMAP_SR, SPA_AUDIO_CHANNEL_SR },
	[SND_CHMAP_RC] = { SND_CHMAP_RC, SPA_AUDIO_CHANNEL_RC },
	[SND_CHMAP_FLC] = { SND_CHMAP_FLC, SPA_AUDIO_CHANNEL_FLC },
	[SND_CHMAP_FRC] = { SND_CHMAP_FRC, SPA_AUDIO_CHANNEL_FRC },
	[SND_CHMAP_RLC] = { SND_CHMAP_RLC, SPA_AUDIO_CHANNEL_RLC },
	[SND_CHMAP_RRC] = { SND_CHMAP_RRC, SPA_AUDIO_CHANNEL_RRC },
	[SND_CHMAP_FLW] = { SND_CHMAP_FLW, SPA_AUDIO_CHANNEL_FLW },
	[SND_CHMAP_FRW] = { SND_CHMAP_FRW, SPA_AUDIO_CHANNEL_FRW },
	[SND_CHMAP_FLH] = { SND_CHMAP_FLH, SPA_AUDIO_CHANNEL_FLH },
	[SND_CHMAP_FCH] = { SND_CHMAP_FCH, SPA_AUDIO_CHANNEL_FCH },
	[SND_CHMAP_FRH] = { SND_CHMAP_FRH, SPA_AUDIO_CHANNEL_FRH },
	[SND_CHMAP_TC] = { SND_CHMAP_TC, SPA_AUDIO_CHANNEL_TC },
	[SND_CHMAP_TFL] = { SND_CHMAP_TFL, SPA_AUDIO_CHANNEL_TFL },
	[SND_CHMAP_TFR] = { SND_CHMAP_TFR, SPA_AUDIO_CHANNEL_TFR },
	[SND_CHMAP_TFC] = { SND_CHMAP_TFC, SPA_AUDIO_CHANNEL_TFC },
	[SND_CHMAP_TRL] = { SND_CHMAP_TRL, SPA_AUDIO_CHANNEL_TRL },
	[SND_CHMAP_TRR] = { SND_CHMAP_TRR, SPA_AUDIO_CHANNEL_TRR },
	[SND_CHMAP_TRC] = { SND_CHMAP_TRC, SPA_AUDIO_CHANNEL_TRC },
	[SND_CHMAP_TFLC] = { SND_CHMAP_TFLC, SPA_AUDIO_CHANNEL_TFLC },
	[SND_CHMAP_TFRC] = { SND_CHMAP_TFRC, SPA_AUDIO_CHANNEL_TFRC },
	[SND_CHMAP_TSL] = { SND_CHMAP_TSL, SPA_AUDIO_CHANNEL_TSL },
	[SND_CHMAP_TSR] = { SND_CHMAP_TSR, SPA_AUDIO_CHANNEL_TSR },
	[SND_CHMAP_LLFE] = { SND_CHMAP_LLFE, SPA_AUDIO_CHANNEL_LLFE },
	[SND_CHMAP_RLFE] = { SND_CHMAP_RLFE, SPA_AUDIO_CHANNEL_RLFE },
	[SND_CHMAP_BC] = { SND_CHMAP_BC, SPA_AUDIO_CHANNEL_BC },
	[SND_CHMAP_BLC] = { SND_CHMAP_BLC, SPA_AUDIO_CHANNEL_BLC },
	[SND_CHMAP_BRC] = { SND_CHMAP_BRC, SPA_AUDIO_CHANNEL_BRC },
};

#define _M(ch)	(1LL << SND_CHMAP_ ##ch)

struct def_mask {
	int channels;
	uint64_t mask;
};

static const struct def_mask default_layouts[] = {
	{ 0, 0 },
	{ 1, _M(MONO) },
	{ 2, _M(FL) | _M(FR) },
	{ 3, _M(FL) | _M(FR) | _M(LFE) },
	{ 4, _M(FL) | _M(FR) | _M(RL) |_M(RR) },
	{ 5, _M(FL) | _M(FR) | _M(RL) |_M(RR) | _M(FC) },
	{ 6, _M(FL) | _M(FR) | _M(RL) |_M(RR) | _M(FC) | _M(LFE) },
	{ 7, _M(FL) | _M(FR) | _M(RL) |_M(RR) | _M(SL) | _M(SR) | _M(FC) },
	{ 8, _M(FL) | _M(FR) | _M(RL) |_M(RR) | _M(SL) | _M(SR) | _M(FC) | _M(LFE) },
};

static enum spa_audio_channel chmap_position_to_channel(enum snd_pcm_chmap_position pos)
{
	return chmap_info[pos].channel;
}

static void sanitize_map(snd_pcm_chmap_t* map)
{
	uint64_t mask = 0, p, dup = 0;
	const struct def_mask *def;
	uint32_t i, j, pos;

	for (i = 0; i < map->channels; i++) {
		if (map->pos[i] < 0 || map->pos[i] > SND_CHMAP_LAST)
			map->pos[i] = SND_CHMAP_UNKNOWN;

		p = 1LL << map->pos[i];
		if (mask & p) {
			/* duplicate channel */
			for (j = 0; j <= i; j++)
				if (map->pos[j] == map->pos[i])
					map->pos[j] = SND_CHMAP_UNKNOWN;
			dup |= p;
			p = 1LL << SND_CHMAP_UNKNOWN;
		}
		mask |= p;
	}
	if ((mask & (1LL << SND_CHMAP_UNKNOWN)) == 0)
		return;

	def = &default_layouts[map->channels];

	/* remove duplicates */
	mask &= ~dup;
	/* keep unassigned channels */
	mask = def->mask & ~mask;

	pos = 0;
	for (i = 0; i < map->channels; i++) {
		if (map->pos[i] == SND_CHMAP_UNKNOWN) {
			do {
				mask >>= 1;
				pos++;
			}
			while (mask != 0 && (mask & 1) == 0);
			map->pos[i] = mask ? pos : 0;
		}

	}

}

int
spa_alsa_enum_format(struct state *state, int seq, uint32_t start, uint32_t num,
		     const struct spa_pod *filter)
{
	snd_pcm_t *hndl;
	snd_pcm_hw_params_t *params;
	snd_pcm_format_mask_t *fmask;
	snd_pcm_access_mask_t *amask;
	snd_pcm_chmap_query_t **maps;
	size_t i, j;
	int err, dir;
	unsigned int min, max;
	uint8_t buffer[4096];
	struct spa_pod_builder b = { 0 };
	struct spa_pod_choice *choice;
	struct spa_pod *fmt;
	int res;
	bool opened;
	struct spa_pod_frame f[2];
	struct spa_result_node_params result;
	uint32_t count = 0;

	opened = state->opened;
	if ((err = spa_alsa_open(state)) < 0)
		return err;

	result.id = SPA_PARAM_EnumFormat;
	result.next = start;

      next:
	result.index = result.next++;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	hndl = state->hndl;
	snd_pcm_hw_params_alloca(&params);
	CHECK(snd_pcm_hw_params_any(hndl, params), "Broken configuration: no configurations available");

	spa_pod_builder_push_object(&b, &f[0], SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
	spa_pod_builder_add(&b,
			SPA_FORMAT_mediaType,    SPA_POD_Id(SPA_MEDIA_TYPE_audio),
			SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
			0);

	snd_pcm_format_mask_alloca(&fmask);
	snd_pcm_hw_params_get_format_mask(params, fmask);

	snd_pcm_access_mask_alloca(&amask);
	snd_pcm_hw_params_get_access_mask(params, amask);

	spa_pod_builder_prop(&b, SPA_FORMAT_AUDIO_format, 0);

	spa_pod_builder_push_choice(&b, &f[1], SPA_CHOICE_None, 0);
	choice = (struct spa_pod_choice*)spa_pod_builder_frame(&b, &f[1]);

	for (i = 1, j = 0; i < SPA_N_ELEMENTS(format_info); i++) {
		const struct format_info *fi = &format_info[i];

		if (snd_pcm_format_mask_test(fmask, fi->format)) {
			if (snd_pcm_access_mask_test(amask, SND_PCM_ACCESS_MMAP_INTERLEAVED)) {
				if (j++ == 0)
					spa_pod_builder_id(&b, fi->spa_format);
				spa_pod_builder_id(&b, fi->spa_format);
			}
			if (snd_pcm_access_mask_test(amask, SND_PCM_ACCESS_MMAP_NONINTERLEAVED) &&
					fi->spa_pformat != SPA_AUDIO_FORMAT_UNKNOWN) {
				if (j++ == 0)
					spa_pod_builder_id(&b, fi->spa_pformat);
				spa_pod_builder_id(&b, fi->spa_pformat);
			}
		}
	}
	if (j > 1)
		choice->body.type = SPA_CHOICE_Enum;
	spa_pod_builder_pop(&b, &f[1]);


	CHECK(snd_pcm_hw_params_get_rate_min(params, &min, &dir), "get_rate_min");
	CHECK(snd_pcm_hw_params_get_rate_max(params, &max, &dir), "get_rate_max");

	spa_pod_builder_prop(&b, SPA_FORMAT_AUDIO_rate, 0);

	spa_pod_builder_push_choice(&b, &f[1], SPA_CHOICE_None, 0);
	choice = (struct spa_pod_choice*)spa_pod_builder_frame(&b, &f[1]);

	spa_pod_builder_int(&b, SPA_CLAMP(DEFAULT_RATE, min, max));
	if (min != max) {
		spa_pod_builder_int(&b, min);
		spa_pod_builder_int(&b, max);
		choice->body.type = SPA_CHOICE_Range;
	}
	spa_pod_builder_pop(&b, &f[1]);

	CHECK(snd_pcm_hw_params_get_channels_min(params, &min), "get_channels_min");
	CHECK(snd_pcm_hw_params_get_channels_max(params, &max), "get_channels_max");

	spa_pod_builder_prop(&b, SPA_FORMAT_AUDIO_channels, 0);

	if (false) {
	// if ((maps = snd_pcm_query_chmaps(hndl)) != NULL) {
		uint32_t channel;
		snd_pcm_chmap_t* map;

		if (maps[result.index] == NULL) {
			snd_pcm_free_chmaps(maps);
			goto enum_end;
		}
		map = &maps[result.index]->map;

		spa_log_debug(state->log, "map %d channels", map->channels);
		sanitize_map(map);
		spa_pod_builder_int(&b, map->channels);

		spa_pod_builder_prop(&b, SPA_FORMAT_AUDIO_position, 0);
		spa_pod_builder_push_array(&b, &f[1]);
		for (j = 0; j < map->channels; j++) {
			spa_log_debug(state->log, "position %zd %d", j, map->pos[j]);
			channel = chmap_position_to_channel(map->pos[j]);
			spa_pod_builder_id(&b, channel);
		}
		spa_pod_builder_pop(&b, &f[1]);

		snd_pcm_free_chmaps(maps);
	}
	else {
		if (result.index > 0)
			goto enum_end;

		spa_pod_builder_push_choice(&b, &f[1], SPA_CHOICE_None, 0);
		choice = (struct spa_pod_choice*)spa_pod_builder_frame(&b, &f[1]);
		spa_pod_builder_int(&b, SPA_CLAMP(DEFAULT_CHANNELS, min, max));
		if (min != max) {
			spa_pod_builder_int(&b, min);
			spa_pod_builder_int(&b, max);
			choice->body.type = SPA_CHOICE_Range;
		}
		spa_pod_builder_pop(&b, &f[1]);
	}

	fmt = spa_pod_builder_pop(&b, &f[0]);

	if ((res = spa_pod_filter(&b, &result.param, fmt, filter)) < 0)
		goto next;

	spa_node_emit_result(&state->hooks, seq, 0, &result);

	if (++count != num)
		goto next;

      enum_end:
	res = 0;
	if (!opened)
		spa_alsa_close(state);
	return res;
}

int spa_alsa_set_format(struct state *state, struct spa_audio_info *fmt, uint32_t flags)
{
	unsigned int rrate, rchannels;
	snd_pcm_uframes_t period_size;
	int err, dir;
	snd_pcm_hw_params_t *params;
	snd_pcm_format_t format;
	struct spa_audio_info_raw *info = &fmt->info.raw;
	snd_pcm_t *hndl;
	unsigned int periods;

	if ((err = spa_alsa_open(state)) < 0)
		return err;

	hndl = state->hndl;

	snd_pcm_hw_params_alloca(&params);
	/* choose all parameters */
	CHECK(snd_pcm_hw_params_any(hndl, params), "Broken configuration for playback: no configurations available");
	/* set hardware resampling */
	CHECK(snd_pcm_hw_params_set_rate_resample(hndl, params, 0), "set_rate_resample");
	/* set the interleaved read/write format */
	CHECK(snd_pcm_hw_params_set_access(hndl, params, SND_PCM_ACCESS_MMAP_INTERLEAVED), "set_access");

	/* disable ALSA wakeups, we use a timer */
	if (snd_pcm_hw_params_can_disable_period_wakeup(params))
		CHECK(snd_pcm_hw_params_set_period_wakeup(hndl, params, 0), "set_period_wakeup");

	/* set the sample format */
	format = spa_format_to_alsa(info->format);
	if (format == SND_PCM_FORMAT_UNKNOWN) {
		spa_log_warn(state->log, "%p: unknown format %u", state, info->format);
		return -EINVAL;
	}

	spa_log_info(state->log, "%p: Stream parameters are %iHz, %s, %i channels",
			state, info->rate, snd_pcm_format_name(format), info->channels);
	CHECK(snd_pcm_hw_params_set_format(hndl, params, format), "set_format");

	/* set the count of channels */
	rchannels = info->channels;
	CHECK(snd_pcm_hw_params_set_channels_near(hndl, params, &rchannels), "set_channels");
	if (rchannels != info->channels) {
		spa_log_warn(state->log, "Channels doesn't match (requested %u, get %u", info->channels, rchannels);
		if (flags & SPA_NODE_PARAM_FLAG_NEAREST)
			info->channels = rchannels;
		else
			return -EINVAL;
	}

	/* set the stream rate */
	rrate = info->rate;
	CHECK(snd_pcm_hw_params_set_rate_near(hndl, params, &rrate, 0), "set_rate_near");
	if (rrate != info->rate) {
		spa_log_warn(state->log, "Rate doesn't match (requested %iHz, get %iHz)", info->rate, rrate);
		if (flags & SPA_NODE_PARAM_FLAG_NEAREST)
			info->rate = rrate;
		else
			return -EINVAL;
	}

	state->format = format;
	state->channels = info->channels;
	state->rate = info->rate;
	state->frame_size = info->channels * (snd_pcm_format_physical_width(format) / 8);

	dir = 0;
	period_size = 1024;
	CHECK(snd_pcm_hw_params_set_period_size_near(hndl, params, &period_size, &dir), "set_period_size_near");
	CHECK(snd_pcm_hw_params_get_buffer_size_max(params, &state->buffer_frames), "get_buffer_size_max");
	CHECK(snd_pcm_hw_params_set_buffer_size_near(hndl, params, &state->buffer_frames), "set_buffer_size_near");
	state->period_frames = period_size;
	periods = state->buffer_frames / state->period_frames;

	spa_log_info(state->log, "%p: buffer frames %zd, period frames %zd, periods %u, frame_size %zd",
			state, state->buffer_frames, state->period_frames,
			periods, state->frame_size);

	/* write the parameters to device */
	CHECK(snd_pcm_hw_params(hndl, params), "set_hw_params");

	return 0;
}

static int set_swparams(struct state *state)
{
	snd_pcm_t *hndl = state->hndl;
	int err = 0;
	snd_pcm_sw_params_t *params;

	snd_pcm_sw_params_alloca(&params);

	/* get the current params */
	CHECK(snd_pcm_sw_params_current(hndl, params), "sw_params_current");

	CHECK(snd_pcm_sw_params_set_tstamp_mode(hndl, params, SND_PCM_TSTAMP_ENABLE), "sw_params_set_tstamp_mode");

#if 0
	snd_pcm_uframes_t boundary;
	CHECK(snd_pcm_sw_params_get_boundary(params, &boundary), "get_boundary");

	CHECK(snd_pcm_sw_params_set_stop_threshold(hndl, params, boundary), "set_stop_threshold");
#endif

	/* start the transfer */
	CHECK(snd_pcm_sw_params_set_start_threshold(hndl, params, LONG_MAX), "set_start_threshold");

	CHECK(snd_pcm_sw_params_set_period_event(hndl, params, 0), "set_period_event");

	/* write the parameters to the playback device */
	CHECK(snd_pcm_sw_params(hndl, params), "sw_params");

	return 0;
}

static int set_timeout(struct state *state, uint64_t time)
{
	struct itimerspec ts;

	if (!state->slaved) {
		ts.it_value.tv_sec = time / SPA_NSEC_PER_SEC;
		ts.it_value.tv_nsec = time % SPA_NSEC_PER_SEC;
		ts.it_interval.tv_sec = 0;
		ts.it_interval.tv_nsec = 0;
		timerfd_settime(state->timerfd, TFD_TIMER_ABSTIME, &ts, NULL);
	}

	return 0;
}

static int alsa_recover(struct state *state, int err)
{
	int res, st;
	snd_pcm_status_t *status;

	snd_pcm_status_alloca(&status);
	if ((res = snd_pcm_status(state->hndl, status)) < 0) {
		spa_log_error(state->log, "snd_pcm_status error: %s", snd_strerror(res));
		return res;
	}

	st = snd_pcm_status_get_state(status);
	switch (st) {
	case SND_PCM_STATE_XRUN:
	{
		struct timeval now, trigger, diff;
		uint64_t xrun, missing;

	        snd_pcm_status_get_tstamp (status, &now);
		snd_pcm_status_get_trigger_tstamp (status, &trigger);
                timersub(&now, &trigger, &diff);

		xrun = SPA_TIMEVAL_TO_USEC(&diff);
		missing = xrun * state->rate / SPA_USEC_PER_SEC;

		spa_log_error(state->log, "%p: xrun of %"PRIu64" usec %"PRIu64" %f",
				state, xrun, missing, state->safety);
		break;
	}
	default:
		spa_log_error(state->log, "recover from error state %d", st);
		break;
	}

	if ((res = snd_pcm_recover(state->hndl, err, true)) < 0) {
		spa_log_error(state->log, "snd_pcm_recover error: %s", snd_strerror(res));
		return res;
	}
	dll_init(&state->dll, DLL_BW_MAX);

	if (state->stream == SND_PCM_STREAM_CAPTURE) {
		if ((res = snd_pcm_start(state->hndl)) < 0) {
			spa_log_error(state->log, "snd_pcm_start: %s", snd_strerror(res));
			return res;
		}
		state->alsa_started = true;
	} else {
		state->alsa_started = false;
		spa_alsa_write(state, state->threshold * 2, true);
	}
	return 0;
}

static int get_status(struct state *state, snd_pcm_sframes_t *delay)
{
	snd_pcm_sframes_t av;
	int res;

	if ((av = snd_pcm_avail(state->hndl)) < 0) {
		if ((res = alsa_recover(state, av)) < 0)
			return res;
		if ((av = snd_pcm_avail(state->hndl)) < 0)
			return av;
	}

	if (delay) {
		if (state->stream == SND_PCM_STREAM_PLAYBACK)
			*delay = state->buffer_frames - av;
		else
			*delay = av;
	}
	return 0;
}

static int update_time(struct state *state, uint64_t nsec, snd_pcm_sframes_t delay, bool slaved)
{
	uint64_t sample_time, elapsed;
	double tw = 0.0, extra = 0.0;
	int64_t sdelay;

	sample_time = state->sample_count;
	if (!slaved) {
		elapsed = sample_time - state->sample_time;
	} else {
		elapsed = state->threshold;
	}

	if (state->stream == SND_PCM_STREAM_CAPTURE) {
		elapsed = state->threshold;
		extra = (double) elapsed / state->rate;
		sdelay = (int64_t)(delay - elapsed);
	} else {
		if (elapsed == 0) {
			elapsed = state->threshold / 2;
			delay = state->threshold / 2;
		}
		state->sample_time = sample_time;
		sdelay = -delay;
	}


	/* we try to match the delay with the number of delayed samples */
	tw = nsec * 1e-9 - (double)sdelay / state->rate - state->safety;
	tw = dll_update(&state->dll, tw, (double)elapsed / state->rate);
	state->next_time = (tw + extra - state->safety) * 1e9;

	if (state->dll.bw > DLL_BW_MIN && tw > state->dll.base + DLL_BW_PERIOD)
		dll_bandwidth(&state->dll, DLL_BW_MIN);

	if (state->clock) {
		state->clock->nsec = state->last_time;
		state->clock->rate = SPA_FRACTION(1, state->rate);
		state->clock->position = state->sample_count;
		state->clock->delay = sdelay;
		state->clock->rate_diff = state->dll.dt;
	}

	state->old_dt = SPA_CLAMP(state->dll.dt, 0.95, 1.05);

#if 0
	if (slaved && state->notify) {
		struct spa_pod_builder b = { 0 };
	        spa_pod_builder_init(&b, state->notify, 1024);
		spa_pod_builder_push_sequence(&b, 0);
		spa_pod_builder_control(&b, 0, SPA_CONTROL_Properties);
		spa_pod_builder_push_object(&b, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
		spa_pod_builder_prop(&b, SPA_PROP_rate, 0);
		spa_pod_builder_double(&b, state->old_dt);
		spa_pod_builder_pop(&b);
		spa_pod_builder_pop(&b);
	}
#endif

	spa_log_trace_fp(state->log, "%"PRIu64" %f %"PRIi64" %"PRIi64" %"PRIi64" %d %"PRIu64" %f %f", nsec,
			state->old_dt, delay, elapsed, (int64_t)(nsec - state->last_time),
			state->threshold, state->next_time, tw, extra);

	state->last_time = nsec;

	return 0;
}

int spa_alsa_write(struct state *state, snd_pcm_uframes_t silence, bool start)
{
	snd_pcm_t *hndl = state->hndl;
	const snd_pcm_channel_area_t *my_areas;
	snd_pcm_uframes_t written, frames, offset, off, to_write, total_written;
	int res;

	if (state->position && state->threshold != state->position->size)
		state->threshold = state->position->size;

	if (state->slaved) {
		uint64_t nsec, master;
		snd_pcm_sframes_t delay;

		master = state->position->clock.position + state->position->clock.delay;
		nsec = master * SPA_NSEC_PER_SEC / state->rate;

		if ((res = get_status(state, &delay)) < 0)
			return res;

		if ((res = update_time(state, nsec, delay, true)) < 0)
			return res;

		spa_log_trace_fp(state->log, "slave %f %"PRIi64" %"PRIu64" %d",
				state->dll.dt, nsec, delay, state->rate);

		if (delay > state->threshold * 2) {
			snd_pcm_rewind(state->hndl, state->threshold);
			delay -= state->threshold;
		}
	}

	total_written = 0;
again:
	frames = state->buffer_frames;
	if ((res = snd_pcm_mmap_begin(hndl, &my_areas, &offset, &frames)) < 0) {
		spa_log_error(state->log, "snd_pcm_mmap_begin error: %s", snd_strerror(res));
		return res;
	}
	spa_log_trace_fp(state->log, "begin %ld %ld %d", offset, frames, state->threshold);

	silence = SPA_MIN(silence, frames);
	to_write = frames;
	off = offset;
	written = 0;

	while (!spa_list_is_empty(&state->ready) && to_write > 0) {
		uint8_t *dst, *src;
		size_t n_bytes, n_frames;
		struct buffer *b;
		struct spa_data *d;
		uint32_t index, offs, avail, size, maxsize, l0, l1;

		b = spa_list_first(&state->ready, struct buffer, link);
		d = b->buf->datas;

		dst = SPA_MEMBER(my_areas[0].addr, off * state->frame_size, uint8_t);
		src = d[0].data;

		size = d[0].chunk->size;
		maxsize = d[0].maxsize;

		index = d[0].chunk->offset + state->ready_offset;
		avail = size - state->ready_offset;
		avail /= state->frame_size;

		n_frames = SPA_MIN(avail, to_write);
		n_bytes = n_frames * state->frame_size;

		offs = index % maxsize;
		l0 = SPA_MIN(n_bytes, maxsize - offs);
		l1 = n_bytes - l0;

		spa_memcpy(dst, src + offs, l0);
		if (l1 > 0)
			spa_memcpy(dst + l0, src, l1);

		state->ready_offset += n_bytes;

		if (state->ready_offset >= size) {
			spa_list_remove(&b->link);
			SPA_FLAG_SET(b->flags, BUFFER_FLAG_OUT);
			state->io->buffer_id = b->id;
			spa_log_trace_fp(state->log, "alsa-util %p: reuse buffer %u", state, b->id);
			state->callbacks->reuse_buffer(state->callbacks_data, 0, b->id);
			state->ready_offset = 0;
		}
		written += n_frames;
		off += n_frames;
		to_write -= n_frames;
		if (silence > n_frames)
			silence -= n_frames;
		else
			silence = 0;
	}

	if (silence > 0) {
		spa_log_trace_fp(state->log, "silence %ld", silence);
		snd_pcm_areas_silence(my_areas, off, state->channels, silence, state->format);
		written += silence;
	}

	spa_log_trace_fp(state->log, "commit %ld %ld %"PRIi64, offset, written, state->sample_count);
	total_written += written;

	if ((res = snd_pcm_mmap_commit(hndl, offset, written)) < 0) {
		spa_log_error(state->log, "snd_pcm_mmap_commit error: %s", snd_strerror(res));
		if (res != -EPIPE && res != -ESTRPIPE)
			return res;
	}

	if (!spa_list_is_empty(&state->ready) && written > 0)
		goto again;

	state->sample_count += total_written;

	if (!state->alsa_started && written > 0 && start) {
		spa_log_trace(state->log, "snd_pcm_start %lu", written);
		if ((res = snd_pcm_start(hndl)) < 0) {
			spa_log_error(state->log, "snd_pcm_start: %s", snd_strerror(res));
			return res;
		}
		state->alsa_started = true;
	}
	return 0;
}

static snd_pcm_uframes_t
push_frames(struct state *state,
	    const snd_pcm_channel_area_t *my_areas,
	    snd_pcm_uframes_t offset,
	    snd_pcm_uframes_t frames)
{
	snd_pcm_uframes_t total_frames = 0;

	if (spa_list_is_empty(&state->free)) {
		spa_log_warn(state->log, "%p: no more buffers", state);
		total_frames = state->threshold;
	} else {
		uint8_t *src;
		size_t n_bytes;
		struct buffer *b;
		struct spa_data *d;
		uint32_t index, offs, avail, l0, l1;
		struct spa_io_buffers *io;

		b = spa_list_first(&state->free, struct buffer, link);
		spa_list_remove(&b->link);

		if (b->h) {
			b->h->seq = state->sample_count;
			b->h->pts = SPA_TIMESPEC_TO_NSEC(&state->now);
			b->h->dts_offset = 0;
		}

		d = b->buf->datas;

		src = SPA_MEMBER(my_areas[0].addr, offset * state->frame_size, uint8_t);

		avail = d[0].maxsize / state->frame_size;
		index = 0;
		total_frames = SPA_MIN(avail, frames);
		n_bytes = total_frames * state->frame_size;

		offs = index % d[0].maxsize;
		l0 = SPA_MIN(n_bytes, d[0].maxsize - offs);
		l1 = n_bytes - l0;

		spa_memcpy(SPA_MEMBER(d[0].data, offs, void), src, l0);
		if (l1 > 0)
			spa_memcpy(d[0].data, src + l0, l1);

		d[0].chunk->offset = index;
		d[0].chunk->size = n_bytes;
		d[0].chunk->stride = state->frame_size;

		SPA_FLAG_SET(b->flags, BUFFER_FLAG_OUT);

		io = state->io;
		if (io != NULL && io->status != SPA_STATUS_HAVE_BUFFER) {
			io->buffer_id = b->id;
			io->status = SPA_STATUS_HAVE_BUFFER;
		}
		else {
			spa_list_append(&state->ready, &b->link);
		}

		state->callbacks->ready(state->callbacks_data, SPA_STATUS_HAVE_BUFFER);
	}
	return total_frames;
}

static int handle_play(struct state *state)
{
	int res;
	snd_pcm_sframes_t delay;
	uint64_t nsec;

	if (state->position && state->threshold != state->position->size)
		state->threshold = state->position->size;

	clock_gettime(CLOCK_MONOTONIC, &state->now);
	if ((res = get_status(state, &delay)) < 0)
		return 0;

	nsec = SPA_TIMESPEC_TO_NSEC(&state->now);
	spa_log_trace_fp(state->log, "timeout %ld %"PRIu64" %"PRIu64" %"PRIi64" %d %ld", delay,
			nsec, state->next_time, nsec - state->next_time,
			state->threshold, state->sample_count);

	if (delay >= state->threshold * 2) {
		spa_log_trace(state->log, "early wakeup %ld %d", delay, state->threshold);
		state->next_time = nsec + (state->threshold / 2) * SPA_NSEC_PER_SEC / state->rate;
		return 0;
	}

	if ((res = update_time(state, nsec, delay, false)) < 0)
		return 0;

	if (spa_list_is_empty(&state->ready)) {
		struct spa_io_buffers *io = state->io;

		spa_log_trace_fp(state->log, "alsa-util %p: %d", state, io->status);

		io->status = SPA_STATUS_NEED_BUFFER;
		if (state->range) {
			state->range->offset = state->sample_count * state->frame_size;
			state->range->min_size = state->threshold * state->frame_size;
			state->range->max_size = state->threshold * state->frame_size;
		}
		state->callbacks->ready(state->callbacks_data, SPA_STATUS_NEED_BUFFER);
	}
	else {
		spa_alsa_write(state, 0, true);
	}
	return 0;
}

static void alsa_on_playback_timeout_event(struct spa_source *source)
{
	struct state *state = source->data;
	uint64_t expire;

	if (state->started && read(state->timerfd, &expire, sizeof(uint64_t)) != sizeof(uint64_t))
		spa_log_warn(state->log, "error reading timerfd: %s", strerror(errno));

	handle_play(state);

	set_timeout(state, state->next_time);
}

static void alsa_on_capture_timeout_event(struct spa_source *source)
{
	uint64_t expire, nsec;
	int res;
	struct state *state = source->data;
	snd_pcm_t *hndl = state->hndl;
	snd_pcm_sframes_t delay;
	snd_pcm_uframes_t total_read = 0, to_read;
	const snd_pcm_channel_area_t *my_areas;

	if (state->started && read(state->timerfd, &expire, sizeof(uint64_t)) != sizeof(uint64_t))
		spa_log_warn(state->log, "error reading timerfd: %s", strerror(errno));

	if (state->position)
		state->threshold = state->position->size;

	clock_gettime(CLOCK_MONOTONIC, &state->now);
	if ((res = get_status(state, &delay)) < 0)
		return;

	nsec = SPA_TIMESPEC_TO_NSEC(&state->now);
	spa_log_trace_fp(state->log, "timeout %ld %"PRIu64" %"PRIu64" %"PRIi64" %d %ld", delay,
			nsec, state->next_time, nsec - state->next_time,
			state->threshold, state->sample_count);

	if (delay < state->threshold) {
		spa_log_trace(state->log, "early wakeup %ld %d", delay, state->threshold);
		state->next_time = nsec + (state->threshold - delay) * SPA_NSEC_PER_SEC / state->rate;
		goto next;
	}

	if ((res = update_time(state, nsec, delay, false)) < 0)
		return;

	to_read = SPA_MIN(delay, state->threshold);

	while (total_read < to_read) {
		snd_pcm_uframes_t read, frames, offset;

		frames = to_read - total_read;
		spa_log_trace_fp(state->log, "begin %ld %ld %ld %ld", offset, frames, to_read, total_read);
		if ((res = snd_pcm_mmap_begin(hndl, &my_areas, &offset, &frames)) < 0) {
			spa_log_error(state->log, "snd_pcm_mmap_begin error: %s", snd_strerror(res));
			return;
		}

		read = push_frames(state, my_areas, offset, frames);
		if (read < frames)
			to_read = 0;

		spa_log_trace_fp(state->log, "commit %ld %ld", offset, read);
		if ((res = snd_pcm_mmap_commit(hndl, offset, read)) < 0) {
			spa_log_error(state->log, "snd_pcm_mmap_commit error: %s", snd_strerror(res));
			if (res != -EPIPE && res != -ESTRPIPE)
				return;
		}
		total_read += read;
	}
	state->sample_count += total_read;

next:
	set_timeout(state, state->next_time);
}

static void reset_buffers(struct state *this)
{
	uint32_t i;

	spa_list_init(&this->free);
	spa_list_init(&this->ready);

	for (i = 0; i < this->n_buffers; i++) {
		struct buffer *b = &this->buffers[i];
		if (this->stream == SND_PCM_STREAM_PLAYBACK) {
			SPA_FLAG_SET(b->flags, BUFFER_FLAG_OUT);
		} else {
			spa_list_append(&this->free, &b->link);
			SPA_FLAG_UNSET(b->flags, BUFFER_FLAG_OUT);
		}
	}
}

int spa_alsa_start(struct state *state)
{
	int err;
	struct itimerspec ts;

	if (state->started)
		return 0;

	if (state->position)
		state->threshold = state->position->size;
	else
		state->threshold = state->props.min_latency;

	state->slaved = false;
	if (state->position && state->clock) {
		if (state->position->clock.id != state->clock->id)
			state->slaved = true;
	}

	dll_init(&state->dll, DLL_BW_MAX);
	state->old_dt = 1.0;
	state->safety = 0.0;

	spa_log_debug(state->log, "alsa %p: start %d %d", state, state->threshold, state->slaved);

	CHECK(set_swparams(state), "swparams");
	snd_pcm_dump(state->hndl, state->output);

	if ((err = snd_pcm_prepare(state->hndl)) < 0) {
		spa_log_error(state->log, "snd_pcm_prepare error: %s", snd_strerror(err));
		return err;
	}

	if (!state->slaved) {
		if (state->stream == SND_PCM_STREAM_PLAYBACK) {
			state->source.func = alsa_on_playback_timeout_event;
		} else {
			state->source.func = alsa_on_capture_timeout_event;
		}
		state->source.data = state;
		state->source.fd = state->timerfd;
		state->source.mask = SPA_IO_IN;
		state->source.rmask = 0;
		spa_loop_add_source(state->data_loop, &state->source);
	}

	reset_buffers(state);

	if (state->stream == SND_PCM_STREAM_PLAYBACK) {
		state->alsa_started = false;
		spa_alsa_write(state, state->threshold * 2, true);
	} else {
		if ((err = snd_pcm_start(state->hndl)) < 0) {
			spa_log_error(state->log, "snd_pcm_start: %s", snd_strerror(err));
			return err;
		}
		state->alsa_started = true;
	}

	if (!state->slaved) {
		clock_gettime(CLOCK_MONOTONIC, &state->now);
		ts.it_value.tv_sec = 0;
		ts.it_value.tv_nsec = 1;
		ts.it_interval.tv_sec = 0;
		ts.it_interval.tv_nsec = 0;
		timerfd_settime(state->timerfd, 0, &ts, NULL);
	}

	state->io->status = SPA_STATUS_OK;
	state->io->buffer_id = SPA_ID_INVALID;

	state->started = true;

	return 0;
}

static int do_remove_source(struct spa_loop *loop,
			    bool async,
			    uint32_t seq,
			    const void *data,
			    size_t size,
			    void *user_data)
{
	struct state *state = user_data;
	struct itimerspec ts;

	if (!state->slaved) {
		spa_loop_remove_source(state->data_loop, &state->source);
		ts.it_value.tv_sec = 0;
		ts.it_value.tv_nsec = 0;
		ts.it_interval.tv_sec = 0;
		ts.it_interval.tv_nsec = 0;
		timerfd_settime(state->timerfd, 0, &ts, NULL);
	}

	return 0;
}

int spa_alsa_pause(struct state *state)
{
	int err;

	if (!state->started)
		return 0;

	spa_log_debug(state->log, "alsa %p: pause", state);

	spa_loop_invoke(state->data_loop, do_remove_source, 0, NULL, 0, true, state);

	if ((err = snd_pcm_drop(state->hndl)) < 0)
		spa_log_error(state->log, "snd_pcm_drop %s", snd_strerror(err));

	state->started = false;

	return 0;
}
