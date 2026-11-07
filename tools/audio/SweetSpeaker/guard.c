/* The speaker's guard: see guard.h. */
#include "guard.h"

#include <math.h>
#include <string.h>

#define HPF_HZ          200.0f
#define PEAK_DBFS       (-3.0f)
#define RELEASE_S       0.043f
#define SHORT_S         0.010f
#define POWER_RELEASE_S 0.100f
#define LA_PEAK_DBFS    0.0f                    /* guard_amp_protects(): only what the 16-bit conversion would clip */
#ifndef LA_RELEASE_S
#define LA_RELEASE_S    0.2f                    /* after the hold; 0.4 s was slow to come back after a burst */
#endif

static const float kTau[3] = { 0.2f, 3.0f, 60.0f };
static const float kLimitDB[3] = { -3.0f, -6.0f, -9.0f };     /* under the power of a full-scale sine (0.5) */

void
guard_init(struct ss_guard *g, float rate, float makeup_db)
{
	memset(g, 0, sizeof(*g));
	g->rate = rate;
	g->makeup = powf(10.0f, makeup_db / 20.0f);
	/* RBJ cookbook high-pass, Q = 1/sqrt(2) (Butterworth) */
	float w0 = 2.0f * (float)M_PI * HPF_HZ / rate, c = cosf(w0), alpha = sinf(w0) / (2.0f * (float)M_SQRT1_2);
	float a0 = 1.0f + alpha;
	g->b0 = (1.0f + c) / 2.0f / a0;
	g->b1 = -(1.0f + c) / a0;
	g->b2 = (1.0f + c) / 2.0f / a0;
	g->a1 = -2.0f * c / a0;
	g->a2 = (1.0f - alpha) / a0;
	for (int k = 0; k < 3; k++) {
		g->tau[k] = kTau[k];
		g->coef[k] = 1.0f - expf(-1.0f / (rate * kTau[k]));
		g->limit[k] = 0.5f * powf(10.0f, kLimitDB[k] / 10.0f);
	}
	g->c_short = 1.0f - expf(-1.0f / (rate * SHORT_S));
	g->power_gain = 1.0f;
	g->power_release = 1.0f - expf(-1.0f / (rate * POWER_RELEASE_S));
	g->peak_gain = 1.0f;
	g->release = 1.0f - expf(-1.0f / (rate * RELEASE_S));
	g->ceiling = powf(10.0f, PEAK_DBFS / 20.0f);
	g->min_gain = 1.0f;
}

static void
la_reset(struct ss_guard *g)
{
	memset(g->la_delay, 0, sizeof(g->la_delay));
	for (unsigned k = 0; k < GUARD_LOOKAHEAD; k++) {
		g->la_env[k] = 1.0f;
	}
	for (unsigned k = 0; k < GUARD_HOLD_BLOCKS; k++) {
		g->la_blk[k] = 1.0f;
	}
	g->la_cur = 1.0f;
	g->la_cur_n = 0;
	g->la_blk_pos = 0;
	g->la_sum = GUARD_LOOKAHEAD;
	g->la_gain = 1.0f;
	g->la_pos = 0;
}

void
guard_amp_protects(struct ss_guard *g)
{
	g->amp_protects = 1;
	g->la_ceiling = powf(10.0f, LA_PEAK_DBFS / 20.0f);
	g->la_release = 1.0f - expf(-1.0f / (g->rate * LA_RELEASE_S));
	la_reset(g);
}

/*
 * One sample through the look-ahead peak stage. The output is the input of GUARD_LOOKAHEAD - 1 samples ago, times the
 * mean of the released gain over the last GUARD_LOOKAHEAD samples. Each released gain is at most the lowest gain needed
 * over the last 30 to 31 ms (the hold), which always covers the last GUARD_LOOKAHEAD samples, so all the gains averaged
 * for a sample were at most what that sample needs: no output passes the ceiling. The hold keeps the gain still between
 * the peaks of a low note, whose waveform would otherwise be reshaped (2026-11-07: two tones 100 Hz apart, everything
 * but the tones at -28 dB without a hold).
 */
static void
la_step(struct ss_guard *g, const float v[2], float peak, float *out)
{
	unsigned pos = g->la_pos;
	g->la_cur = fminf(g->la_cur, peak > g->la_ceiling ? g->la_ceiling / peak : 1.0f);
	g->la_delay[pos][0] = v[0];
	g->la_delay[pos][1] = v[1];
	float hold = g->la_cur;
	for (unsigned k = 0; k < GUARD_HOLD_BLOCKS; k++) {
		hold = fminf(hold, g->la_blk[k]);
	}
	if (++g->la_cur_n == GUARD_LOOKAHEAD) {
		g->la_blk[g->la_blk_pos] = g->la_cur;
		g->la_blk_pos = (g->la_blk_pos + 1) % GUARD_HOLD_BLOCKS;
		g->la_cur = 1.0f;
		g->la_cur_n = 0;
	}
	float e = fminf(hold, g->la_gain + (1.0f - g->la_gain) * g->la_release);
	g->la_gain = e;
	g->la_sum += (double)e - (double)g->la_env[pos];
	g->la_env[pos] = e;
	float s = fminf(1.0f, (float)(g->la_sum / GUARD_LOOKAHEAD));
	unsigned old = (pos + 1) % GUARD_LOOKAHEAD;     /* written GUARD_LOOKAHEAD - 1 samples ago */
	out[0] = g->la_delay[old][0] * s;
	out[1] = g->la_delay[old][1] * s;
	g->la_pos = old;
	g->min_gain = fminf(g->min_gain, s);
}

void
guard_run(struct ss_guard *g, float *io, unsigned frames)
{
	for (unsigned i = 0; i < frames; i++) {
		float v[2], p = 0.0f;
		for (int ch = 0; ch < 2; ch++) {
			float x = io[2 * i + ch] * g->makeup;
			if (g->amp_protects) {          /* the amps' program shapes the speaker's low end: no high-pass here */
				v[ch] = x;
				p = fmaxf(p, x * x);
				continue;
			}
			float y = g->b0 * x + g->b1 * g->x1[ch] + g->b2 * g->x2[ch] - g->a1 * g->y1[ch] - g->a2 * g->y2[ch];
			g->x2[ch] = g->x1[ch];
			g->x1[ch] = x;
			g->y2[ch] = g->y1[ch];
			g->y1[ch] = y;
			v[ch] = y;
			p = fmaxf(p, y * y);            /* one speaker: the louder channel is what it may get */
		}
		if (g->amp_protects) {                  /* the amps' own protection does the power stage's job */
			la_step(g, v, sqrtf(p), &io[2 * i]);
			continue;
		}
		/* the largest gain whose output keeps each average at or under its limit: avg + c (g^2 p - avg) <= limit */
		g->p_short += g->c_short * (p - g->p_short);
		float target = 1.0f;
		for (int k = 0; k < 3; k++) {
			float room = g->limit[k] - (1.0f - g->coef[k]) * g->avg[k];
			float need = g->coef[k] * g->p_short;
			if (need > 0.0f && room < need) {
				target = fminf(target, room > 0.0f ? sqrtf(room / need) : 0.0f);
			}
		}
		if (target < g->power_gain) {
			g->power_gain = target;
		} else {
			g->power_gain += (target - g->power_gain) * g->power_release;
		}
		float gp = g->power_gain, o0 = v[0] * gp, o1 = v[1] * gp, peak = fmaxf(fabsf(o0), fabsf(o1));
		for (int k = 0; k < 3; k++) {         /* the same smoothed power as the bound: an average cannot overshoot */
			g->avg[k] += g->coef[k] * (gp * gp * g->p_short - g->avg[k]);
		}
		g->peak_gain += (1.0f - g->peak_gain) * g->release;
		if (peak * g->peak_gain > g->ceiling) {
			g->peak_gain = g->ceiling / peak;
		}
		io[2 * i] = o0 * g->peak_gain;
		io[2 * i + 1] = o1 * g->peak_gain;
		g->min_gain = fminf(g->min_gain, gp * g->peak_gain);
	}
	/* the filter's tail after a sound: no denormals */
	for (int ch = 0; ch < 2; ch++) {
		if (fabsf(g->y1[ch]) < 1e-20f && fabsf(g->y2[ch]) < 1e-20f) {
			g->y1[ch] = g->y2[ch] = 0.0f;
		}
	}
}

void
guard_rest(struct ss_guard *g, float seconds)
{
	for (int k = 0; k < 3; k++) {
		g->avg[k] *= expf(-seconds / g->tau[k]);
	}
	memset(g->x1, 0, sizeof(g->x1));
	memset(g->x2, 0, sizeof(g->x2));
	memset(g->y1, 0, sizeof(g->y1));
	memset(g->y2, 0, sizeof(g->y2));
	g->p_short *= expf(-seconds / SHORT_S);
	g->peak_gain = 1.0f;
	if (g->amp_protects) {
		la_reset(g);
	}
}

float
guard_take_min_db(struct ss_guard *g)
{
	float db = 20.0f * log10f(fmaxf(g->min_gain, 1e-6f));
	g->min_gain = 1.0f;
	return db;
}
