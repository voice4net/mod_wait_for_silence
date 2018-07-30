#include <switch.h>

#define WAIT_FOR_SILENCE_PARAMS (6)
#define WAIT_FOR_SILENCE_SYNTAX "<uuid> <start|stop> [<silence_thresh>] [<silence_hits>] [<listen_hits>] [<timeout_ms>]"
#define WAIT_FOR_SILENCE_EVENT_COMPLETE "wait_for_silence::complete"

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_wait_for_silence_shutdown);
SWITCH_STANDARD_API(wait_for_silence_api_main);
SWITCH_MODULE_LOAD_FUNCTION(mod_wait_for_silence_load);
SWITCH_MODULE_DEFINITION(mod_wait_for_silence, mod_wait_for_silence_load, mod_wait_for_silence_shutdown, NULL);

static struct {
	uint32_t silence_threshold;
	uint32_t silence_hits;
	uint32_t listen_hits;
	uint32_t timeout_ms;
} globals;

static switch_xml_config_item_t instructions[] = {
	SWITCH_CONFIG_ITEM(
		"silence_threshold",
		SWITCH_CONFIG_INT,
		CONFIG_RELOADABLE,
		&globals.silence_threshold,
		(void *) 256,
		NULL, NULL, NULL),

	SWITCH_CONFIG_ITEM(
		"silence_hits",
		SWITCH_CONFIG_INT,
		CONFIG_RELOADABLE,
		&globals.silence_hits,
		(void *) 100,
		NULL, NULL, NULL),

	SWITCH_CONFIG_ITEM(
		"listen_hits",
		SWITCH_CONFIG_INT,
		CONFIG_RELOADABLE,
		&globals.listen_hits,
		(void *) 15,
		NULL, NULL, NULL),

	SWITCH_CONFIG_ITEM(
		"timeout_ms",
		SWITCH_CONFIG_INT,
		CONFIG_RELOADABLE,
		&globals.timeout_ms,
		(void *) 60000,
		NULL, NULL, NULL),

	SWITCH_CONFIG_ITEM_END()
};

static switch_status_t do_config(switch_bool_t reload)
{
	memset(&globals, 0, sizeof(globals));

	if (switch_xml_config_parse_module_settings("wait_for_silence.conf", reload, instructions) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	return SWITCH_STATUS_SUCCESS;
}

static void wait_for_silence_send_complete_event(switch_channel_t *channel, switch_bool_t silence_detected)
{
	switch_event_t *event;
	switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, WAIT_FOR_SILENCE_EVENT_COMPLETE);
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Silence-Detected", silence_detected ? "true" : "false");
	switch_channel_event_set_data(channel, event);
	switch_event_fire(&event);
	switch_event_destroy(&event);
}

SWITCH_MODULE_LOAD_FUNCTION(mod_wait_for_silence_load)
{
	switch_api_interface_t *api_interface;

	*module_interface = switch_loadable_module_create_module_interface(pool, modname);

	do_config(SWITCH_FALSE);

	SWITCH_ADD_API(api_interface, "wait_for_silence", "Silence Detection (non-blocking)", wait_for_silence_api_main, WAIT_FOR_SILENCE_SYNTAX);

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_wait_for_silence_shutdown)
{
	switch_xml_config_cleanup(instructions);

	return SWITCH_STATUS_SUCCESS;
}

typedef enum
{
	SILENCE,
	VOICED,
	BADFRAME
} wait_for_silence_frame_classifier;

typedef struct wait_for_silence_t
{
	const switch_core_session_t *session;
	switch_channel_t *channel;
	uint32_t listening;
	uint32_t org_silence_hits;
	uint32_t silence_hits;
	uint32_t listen_hits;
	uint32_t silence_threshold;
	switch_codec_implementation_t read_impl;
	switch_codec_t read_codec;
	int32_t sample_count;
	int32_t samples_per_packet;
	switch_bool_t complete;
	switch_bool_t silence_detected;
} wait_for_silence_t;

typedef struct wait_for_silence_frame_analysis_t
{
	wait_for_silence_frame_classifier frame_type;
	uint32_t score;
	double energy;
	double decibels;
} wait_for_silence_frame_analysis_t;

static wait_for_silence_frame_analysis_t wait_for_silence_analyze_frame(const switch_core_session_t *session, const switch_frame_t *f, const switch_codec_implementation_t *codec, uint32_t silence_threshold)
{
	wait_for_silence_frame_analysis_t frame_analysis;

	frame_analysis.frame_type = SILENCE;

	int16_t *audio = f->data, sample;
	uint32_t count, j;
	int divisor;
	double energy = 0.0, amplitude = 0.0, sum = 0.0, rms = 0.0, logarithm = 0.0;

	divisor = codec->actual_samples_per_second / 8000;

	for (energy = 0, j = 0, count = 0; count < f->samples; count++)
	{
		sample = audio[j++];
		amplitude = sample / 32768.0;
		sum += amplitude * amplitude;
		energy += abs(audio[j++]);
		j += codec->number_of_channels;
	}

	frame_analysis.energy = energy;

	rms = sqrt(sum / f->samples);

	if (rms != 0.0)
	{
		logarithm = log10(rms);
		frame_analysis.decibels = 20 * logarithm;
	}

	frame_analysis.score = (uint32_t) ((energy * divisor) / f->samples);

	if (frame_analysis.score >= 5000)
	{
		frame_analysis.frame_type = BADFRAME;
	}

	if (frame_analysis.score >= silence_threshold)
	{
		frame_analysis.frame_type = VOICED;
	}

	return frame_analysis;
}

static switch_bool_t wait_for_silence_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
	if (type != SWITCH_ABC_TYPE_READ_REPLACE)
	{
		return SWITCH_TRUE;
	}

	struct wait_for_silence_t* wfs = (struct wait_for_silence_t *) user_data;

	if (wfs->complete)
	{
		return SWITCH_FALSE;
	}

	switch_core_session_t *session = switch_core_media_bug_get_session(bug);
	switch_bool_t complete = SWITCH_FALSE;
	switch_frame_t* frame = switch_core_media_bug_get_read_replace_frame(bug);

	if (frame->samples == 0)
	{
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "wait_for_silence: frame contains no samples.\n");
		return SWITCH_TRUE;
	}

	if (wfs->sample_count)
	{
		wfs->sample_count -= wfs->samples_per_packet;

		if (wfs->sample_count <= 0)
		{
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "wait_for_silence: TIMEOUT\n");
			switch_channel_set_variable(wfs->channel, "wait_for_silence_timeout", "true");
			switch_channel_set_variable_printf(wfs->channel, "wait_for_silence_listenhits", "%d", wfs->listening);
			switch_channel_set_variable_printf(wfs->channel, "wait_for_silence_silence_hits", "%d", wfs->silence_hits);
			complete = SWITCH_TRUE;
			goto end;
		}
	}

	wait_for_silence_frame_analysis_t frame_analysis = wait_for_silence_analyze_frame(session, frame, &wfs->read_impl, wfs->silence_threshold);

	if (frame_analysis.frame_type == VOICED)
	{
		wfs->listening++;
	}

	if (wfs->listening > wfs->listen_hits && frame_analysis.frame_type == SILENCE)
	{
		if (!--wfs->silence_hits)
		{
			switch_channel_set_variable(wfs->channel, "wait_for_silence_timeout", "false");
			wfs->silence_detected = SWITCH_TRUE;
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "wait_for_silence: SILENCE DETECTED\n");
			complete = SWITCH_TRUE;
			goto end;
		}
	}
	else
	{
		wfs->silence_hits = wfs->org_silence_hits;
	}

end:

	if (complete)
	{
		wait_for_silence_send_complete_event(wfs->channel, wfs->silence_detected);
		wfs->complete = SWITCH_TRUE;
		switch_channel_set_private(wfs->channel, "_wait_for_silence_bug_", NULL);
		return SWITCH_FALSE;
	}

	return SWITCH_TRUE;
}

static switch_status_t wait_for_silence_start(switch_core_session_t *session, uint32_t silence_threshold, uint32_t silence_hits, uint32_t listen_hits, uint32_t timeout_ms)
{
	if (!session)
	{
		return SWITCH_STATUS_FALSE;
	}

	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_codec_implementation_t read_impl = { 0 };
	switch_status_t status;
	switch_media_bug_t *bug = NULL;
	struct wait_for_silence_t *wfs = NULL;

	if (switch_core_session_get_read_impl(session, &read_impl) != SWITCH_STATUS_SUCCESS)
	{
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "wait_for_silence: failed to get read codec implementation.\n");
		return SWITCH_STATUS_FALSE;
	}

	wfs = switch_core_session_alloc(session, sizeof(*wfs));

	wfs->channel = channel;
	wfs->session = session;
	wfs->read_impl = read_impl;
	wfs->samples_per_packet = read_impl.samples_per_packet;
	wfs->silence_threshold = silence_threshold;
	wfs->silence_hits = silence_hits;
	wfs->org_silence_hits = silence_hits;
	wfs->listen_hits = listen_hits;
	wfs->sample_count = (read_impl.actual_samples_per_second / 1000) * timeout_ms;

	if (strcmp(read_impl.iananame, "PCMU") != 0)
	{
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "wait_for_silence: raw codec requires initialization.\n");

		/*
		We are creating a new L16 (raw 16-bit samples) codec for the read end
		of our channel.  We'll use this to process the audio coming off of the
		channel so that we always know what we are dealing with.
		*/
		status = switch_core_codec_init(
			&wfs->read_codec,
			"L16",
			NULL,
			NULL,
			read_impl.actual_samples_per_second,
			read_impl.microseconds_per_packet / 1000,
			1,
			SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE,
			NULL,
			switch_core_session_get_pool(session));

		if (status != SWITCH_STATUS_SUCCESS)
		{
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Unable to initialize L16 (raw) codec.\n");
			return SWITCH_STATUS_FALSE;
		}

		switch_core_session_set_read_codec(session, &wfs->read_codec);
	}

	if (switch_core_media_bug_add(session, "wait_for_silence", NULL, wait_for_silence_callback, wfs, 0, SMBF_READ_REPLACE, &bug) != SWITCH_STATUS_SUCCESS)
	{
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Cannot attach bug\n");
		return SWITCH_STATUS_FALSE;
	}

	switch_channel_set_private(channel, "_wait_for_silence_bug_", bug);

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "wait_for_silence: silence detection initialized.\n");

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_STANDARD_API(wait_for_silence_api_main)
{
	switch_core_session_t *wfs_session = NULL;
	switch_media_bug_t *bug;
	switch_channel_t *channel;

	int argc;
	char *argv[WAIT_FOR_SILENCE_PARAMS];
	char *cmd_rw, *uuid, *command;

	if (zstr(cmd))
	{
		stream->write_function(stream, "-USAGE: %s\n", WAIT_FOR_SILENCE_SYNTAX);
		return SWITCH_STATUS_SUCCESS;
	}

	cmd_rw = strdup(cmd);

	/* Split the arguments */
	argc = switch_separate_string(cmd_rw, ' ', argv, WAIT_FOR_SILENCE_PARAMS);

	if (argc != 2 && argc != 6)
	{
		stream->write_function(stream, "-USAGE: %s\n", WAIT_FOR_SILENCE_SYNTAX);
		goto end;
	}

	uuid = argv[0];
	command = argv[1];

	if (strncasecmp(command, "start", sizeof("start") - 1) != 0 && strncasecmp(command, "stop", sizeof("stop") - 1) != 0)
	{
		stream->write_function(stream, "-USAGE: %s\n", WAIT_FOR_SILENCE_SYNTAX);
		goto end;
	}

	wfs_session = switch_core_session_locate(uuid);

	if (!wfs_session)
	{
		stream->write_function(stream, "-USAGE: %s\n", WAIT_FOR_SILENCE_SYNTAX);
		goto end;
	}

	channel = switch_core_session_get_channel(wfs_session);
	bug = (switch_media_bug_t *) switch_channel_get_private(channel, "_wait_for_silence_bug_");

	if (bug)
	{
		if (strncasecmp(command, "stop", sizeof("stop") - 1) == 0)
		{
			switch_core_media_bug_remove(wfs_session, &bug);
			switch_channel_set_private(channel, "_wait_for_silence_bug_", NULL);
			stream->write_function(stream, "+OK\n");
			goto end;
		}

		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "wait_for_silence: already running on channel.\n");
		goto end;
	}

	if (strncasecmp(command, "start", sizeof("start") - 1) == 0)
	{
		uint32_t silence_threshold = globals.silence_threshold;
		uint32_t silence_hits = globals.silence_hits;
		uint32_t listen_hits = globals.listen_hits;
		uint32_t timeout_ms = globals.timeout_ms;

		if (argc == 6)
		{
			silence_threshold = atoi(argv[2]);
			silence_hits = atoi(argv[3]);
			listen_hits = atoi(argv[4]);
			timeout_ms = atoi(argv[5]);
		}

		wait_for_silence_start(wfs_session, silence_threshold, silence_hits, listen_hits, timeout_ms);
		stream->write_function(stream, "+OK\n");
		goto end;
	}

end:

	if (wfs_session)
	{
		switch_core_session_rwunlock(wfs_session);
	}

	switch_safe_free(cmd_rw);

	return SWITCH_STATUS_SUCCESS;
}
