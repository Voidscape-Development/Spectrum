#include "audio-level.h"

#include <media-io/audio-io.h>
#include <util/dstr.h>
#include <util/threading.h>

#include <math.h>
#include <string.h>

// Onsets are looked for in blocks of this many frames (~5 ms at 48 kHz).
#define BLOCK_FRAMES 256
// Time constant of the running average that onsets are compared against.
#define AVERAGE_TIME 0.25
// Shortest time between two onsets.
#define ONSET_HOLD 0.12

struct spectrum_audio_meter {
	// Binding (serialised by bind_mutex).
	pthread_mutex_t bind_mutex;
	obs_weak_source_t *weak;
	struct dstr name;

	// Analysis (audio thread writes, video thread reads).
	pthread_mutex_t mutex;
	double floor_db, ceiling_db, onset_ratio;
	double average;    // running mean power
	double since;      // seconds since the last onset
	double block_pow;  // power accumulated in the current block
	double block_peak; // peak in the current block
	uint32_t block_n;
	double level; // loudest normalised level since the last read
	bool onset;   // onset since the last read
};

spectrum_audio_meter_t *spectrum_audio_meter_create(void)
{
	spectrum_audio_meter_t *m = bzalloc(sizeof(*m));
	pthread_mutex_init(&m->bind_mutex, NULL);
	pthread_mutex_init(&m->mutex, NULL);
	m->since = ONSET_HOLD;
	spectrum_audio_meter_configure(m, -50.0, -6.0, 0.5);
	return m;
}

void spectrum_audio_meter_destroy(spectrum_audio_meter_t *m)
{
	if (!m)
		return;
	spectrum_audio_meter_unbind(m);
	dstr_free(&m->name);
	pthread_mutex_destroy(&m->mutex);
	pthread_mutex_destroy(&m->bind_mutex);
	bfree(m);
}

void spectrum_audio_meter_configure(spectrum_audio_meter_t *m, double floor_db, double ceiling_db, double sensitivity)
{
	sensitivity = sensitivity < 0.0 ? 0.0 : (sensitivity > 1.0 ? 1.0 : sensitivity);
	// 0 needs a 10 dB jump over the running average, 1 only 1.5 dB.
	const double jump_db = 10.0 - 8.5 * sensitivity;
	pthread_mutex_lock(&m->mutex);
	m->floor_db = floor_db;
	m->ceiling_db = ceiling_db;
	m->onset_ratio = pow(10.0, jump_db / 10.0);
	pthread_mutex_unlock(&m->mutex);
}

static double normalise(const spectrum_audio_meter_t *m, double peak)
{
	const double db = peak > 0.0 ? 20.0 * log10(peak) : -200.0;
	const double v = (db - m->floor_db) / fmax(m->ceiling_db - m->floor_db, 0.1);
	return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

// Ends a block: updates the level, the running average and onsets.
static void finish_block(spectrum_audio_meter_t *m, double block_seconds)
{
	const double power = m->block_pow / (double)m->block_n;
	const double level = normalise(m, m->block_peak);
	if (level > m->level)
		m->level = level;

	m->since += block_seconds;
	const double power_db = power > 0.0 ? 10.0 * log10(power) : -200.0;
	if (power_db > m->floor_db && power > m->average * m->onset_ratio && m->since >= ONSET_HOLD) {
		m->onset = true;
		m->since = 0.0;
	}
	const double k = 1.0 - exp(-block_seconds / AVERAGE_TIME);
	m->average += (power - m->average) * k;

	m->block_pow = 0.0;
	m->block_peak = 0.0;
	m->block_n = 0;
}

void spectrum_audio_meter_feed(spectrum_audio_meter_t *m, const float *const *planes, size_t channels, uint32_t frames,
			       uint32_t sample_rate)
{
	if (!sample_rate)
		sample_rate = 48000;
	const double block_seconds = (double)BLOCK_FRAMES / (double)sample_rate;
	pthread_mutex_lock(&m->mutex);
	for (uint32_t i = 0; i < frames; i++) {
		double pow_sum = 0.0;
		size_t used = 0;
		for (size_t ch = 0; ch < channels; ch++) {
			if (!planes[ch])
				continue;
			const double v = planes[ch][i];
			pow_sum += v * v;
			const double a = fabs(v);
			if (a > m->block_peak)
				m->block_peak = a;
			used++;
		}
		m->block_pow += used ? pow_sum / (double)used : 0.0;
		if (++m->block_n >= BLOCK_FRAMES)
			finish_block(m, block_seconds);
	}
	pthread_mutex_unlock(&m->mutex);
}

void spectrum_audio_meter_read(spectrum_audio_meter_t *m, double *level, bool *onset)
{
	pthread_mutex_lock(&m->mutex);
	*level = m->level;
	*onset = m->onset;
	m->level = 0.0;
	m->onset = false;
	pthread_mutex_unlock(&m->mutex);
}

double spectrum_envelope_step(double env, double target, double seconds, double attack, double release)
{
	const double tc = target > env ? attack : release;
	const double k = tc > 0.0 ? 1.0 - exp(-seconds / tc) : 1.0;
	return env + (target - env) * k;
}

/* ------------------------------------------------------------------------
 * Audio capture
 * --------------------------------------------------------------------- */

static void capture_callback(void *param, obs_source_t *source, const struct audio_data *audio, bool muted)
{
	UNUSED_PARAMETER(source);
	spectrum_audio_meter_t *m = param;
	if (muted) {
		// Muted sources read as silence (keeps the average decaying).
		static const float zero[BLOCK_FRAMES] = {0.0f};
		const float *planes[1] = {zero};
		for (uint32_t done = 0; done < audio->frames; done += BLOCK_FRAMES) {
			const uint32_t n = audio->frames - done < BLOCK_FRAMES ? audio->frames - done : BLOCK_FRAMES;
			spectrum_audio_meter_feed(m, planes, 1, n, audio_output_get_sample_rate(obs_get_audio()));
		}
		return;
	}
	const size_t channels = audio_output_get_channels(obs_get_audio());
	const float *planes[MAX_AV_PLANES] = {0};
	for (size_t ch = 0; ch < channels && ch < MAX_AV_PLANES; ch++)
		planes[ch] = (const float *)audio->data[ch];
	spectrum_audio_meter_feed(m, planes, channels < MAX_AV_PLANES ? channels : MAX_AV_PLANES, audio->frames,
				  audio_output_get_sample_rate(obs_get_audio()));
}

static void unbind_locked(spectrum_audio_meter_t *m)
{
	if (m->weak) {
		obs_source_t *source = obs_weak_source_get_source(m->weak);
		if (source) {
			obs_source_remove_audio_capture_callback(source, capture_callback, m);
			obs_source_release(source);
		}
		obs_weak_source_release(m->weak);
		m->weak = NULL;
	}
	dstr_free(&m->name);
}

void spectrum_audio_meter_unbind(spectrum_audio_meter_t *m)
{
	pthread_mutex_lock(&m->bind_mutex);
	unbind_locked(m);
	pthread_mutex_unlock(&m->bind_mutex);
}

void spectrum_audio_meter_bind(spectrum_audio_meter_t *m, const char *name)
{
	pthread_mutex_lock(&m->bind_mutex);
	if (!name || !*name) {
		unbind_locked(m);
		pthread_mutex_unlock(&m->bind_mutex);
		return;
	}
	if (m->weak && m->name.array && strcmp(m->name.array, name) == 0) {
		obs_source_t *source = obs_weak_source_get_source(m->weak);
		const bool alive = source != NULL && !obs_source_removed(source);
		obs_source_release(source);
		if (alive) {
			pthread_mutex_unlock(&m->bind_mutex);
			return;
		}
	}
	unbind_locked(m);
	obs_source_t *source = obs_get_source_by_name(name);
	if (source) {
		obs_source_add_audio_capture_callback(source, capture_callback, m);
		m->weak = obs_source_get_weak_source(source);
		dstr_copy(&m->name, name);
	}
	pthread_mutex_unlock(&m->bind_mutex);
	obs_source_release(source);
}
