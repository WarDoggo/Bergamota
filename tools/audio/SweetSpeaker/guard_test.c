/*
 * guard_test: the speaker's guard (guard.c) on synthetic signals, on the Mac:
 *   cc -O2 -o /tmp/guard_test tools/audio/SweetSpeaker/guard_test.c tools/audio/SweetSpeaker/guard.c && /tmp/guard_test
 * Each case prints, once per reported second, the output's peak (dBFS) and power (dB under a full-scale sine), and the
 * checks fail with exit status 1: no sample over -3 dBFS, the averages' limits held, the high-pass where it should be.
 */
#include "guard.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE    48000.0f
#define BLOCK   1024u

static int failures;

static void
check(int ok, const char *what)
{
	if (!ok) {
		printf("  FAIL: %s\n", what);
		failures++;
	}
}

/* a sine of freq at amp (0..1) for secs, then silence for rest_secs; returns the output power (dB re full-scale sine)
 * averaged over [from_s, to_s) of the tone, and its highest peak (dBFS) */
static void
tone(struct ss_guard *g, float freq, float amp, float secs, const float *report, int nreport, float *pow_db, float *peak_db)
{
	static float buf[2 * BLOCK];
	double phase = 0, sum = 0, sum_all = 0;
	unsigned long n = (unsigned long)(secs * RATE), done = 0, cnt = 0;
	float peak = 0, peak_all = 0;
	int r = 0;
	while (done < n) {
		unsigned k = n - done < BLOCK ? (unsigned)(n - done) : BLOCK;
		for (unsigned i = 0; i < k; i++) {
			float v = amp * (float)sin(phase);
			phase += 2 * M_PI * freq / RATE;
			buf[2 * i] = buf[2 * i + 1] = v;
		}
		guard_run(g, buf, k);
		for (unsigned i = 0; i < k; i++) {
			float v = buf[2 * i];
			sum += (double)v * v;
			sum_all += (double)v * v;
			peak = fmaxf(peak, fabsf(v));
			peak_all = fmaxf(peak_all, fabsf(v));
			cnt++;
		}
		done += k;
		float t = (float)done / RATE;
		if (r < nreport && t >= report[r]) {
			printf("  t %5.1f s: power %6.2f dB, peak %6.2f dBFS\n", t, 10 * log10(sum / cnt / 0.5 + 1e-30),
			    20 * log10(peak + 1e-30));
			sum = 0;
			cnt = 0;
			peak = 0;
			r++;
		}
	}
	*pow_db = (float)(10 * log10(sum_all / (double)n / 0.5 + 1e-30));
	*peak_db = 20 * log10f(peak_all + 1e-30f);
}

/* Goertzel power of freq in x[0..n) */
static double
bin_power(const float *x, unsigned long n, float freq)
{
	double w = 2 * M_PI * freq / RATE, c = 2 * cos(w), s1 = 0, s2 = 0;
	for (unsigned long i = 0; i < n; i++) {
		double s0 = x[i] + c * s1 - s2;
		s2 = s1;
		s1 = s0;
	}
	return s1 * s1 + s2 * s2 - c * s1 * s2;
}

/* a sine of freq at amp for secs: the output's power over its first and last second (dB re full-scale sine), its peak
 * (dBFS), and its distortion over the last second (harmonics 2-6 against the tone, dB) */
static void
tone_thd(struct ss_guard *g, float freq, float amp, float secs, float *first_db, float *last_db, float *peak_db, float *thd_db)
{
	static float buf[2 * BLOCK], last[48000];
	unsigned long n = (unsigned long)(secs * RATE), done = 0, nl = 0;
	double phase = 0, sum_first = 0, sum_last = 0;
	float peak = 0;
	while (done < n) {
		unsigned k = n - done < BLOCK ? (unsigned)(n - done) : BLOCK;
		for (unsigned i = 0; i < k; i++) {
			float v = amp * (float)sin(phase);
			phase += 2 * M_PI * freq / RATE;
			buf[2 * i] = buf[2 * i + 1] = v;
		}
		guard_run(g, buf, k);
		for (unsigned i = 0; i < k; i++) {
			unsigned long t = done + i;
			float v = buf[2 * i];
			peak = fmaxf(peak, fabsf(v));
			if (t >= 2400 && t < 2400 + 48000) {        /* the first second after 50 ms of settling */
				sum_first += (double)v * v;
			}
			if (t >= n - 48000) {
				sum_last += (double)v * v;
				last[nl++] = v;
			}
		}
		done += k;
	}
	*first_db = (float)(10 * log10(sum_first / 48000 / 0.5 + 1e-30));
	*last_db = (float)(10 * log10(sum_last / 48000 / 0.5 + 1e-30));
	*peak_db = 20 * log10f(peak + 1e-30f);
	double fund = bin_power(last, nl, freq), harm = 0;
	for (int h = 2; h <= 6; h++) {
		harm += bin_power(last, nl, freq * h);
	}
	*thd_db = (float)(10 * log10(harm / fund + 1e-30));
}

/* the amps protect the speakers (guard_amp_protects, the Poco F1): the old stages against the new one at full volume */
static void
amp_protects_cases(void)
{
	struct ss_guard g;
	float first, last, pk, thd;
	printf("full volume, 1 kHz at -9 dBFS (a loud ringtone after Apple's chain), +12 dB make-up, 30 s:\n");
	guard_init(&g, RATE, 12.0f);
	tone_thd(&g, 1000, powf(10, -9 / 20.0f), 30, &first, &last, &pk, &thd);
	printf("  Mi 8 guard:      first second %6.2f dB, last %6.2f dB, peak %6.2f dBFS, distortion %6.1f dB, lowest gain %.1f dB\n",
	    first, last, pk, thd, guard_take_min_db(&g));
	guard_init(&g, RATE, 12.0f);
	guard_amp_protects(&g);
	tone_thd(&g, 1000, powf(10, -9 / 20.0f), 30, &first, &last, &pk, &thd);
	float lowest = guard_take_min_db(&g);
	printf("  amps protect:    first second %6.2f dB, last %6.2f dB, peak %6.2f dBFS, distortion %6.1f dB, lowest gain %.1f dB\n",
	    first, last, pk, thd, lowest);
	check(pk <= 0.01f, "amps protect: a peak over 0 dBFS");
	check(fabsf(last - first) < 0.1f, "amps protect: the level fell during a steady tone");
	/* +3 dBFS peaks into a 0 dBFS ceiling: 3 dB, and no high-pass whose start-up could overshoot */
	check(lowest > -3.1f, "amps protect: more than the peak excess taken off");
	check(thd < -60.0f, "amps protect: the limiter distorts a steady tone");

	/* two tones, so the peaks come and go like music's: what is left besides the two tones is the limiter's distortion */
	printf("  two tones (1 kHz + 3.1 kHz, each -12 dBFS), +12 dB make-up, 10 s: what is not the two tones\n");
	for (int mode = 0; mode < 2; mode++) {
		static float b2[2 * BLOCK], tail[48000];
		guard_init(&g, RATE, 12.0f);
		if (mode) {
			guard_amp_protects(&g);
		}
		unsigned long n = (unsigned long)(10 * RATE), done = 0, nt = 0;
		double ph1 = 0, ph2 = 0;
		float a = powf(10, -12 / 20.0f), tpk = 0;
		while (done < n) {
			unsigned k = n - done < BLOCK ? (unsigned)(n - done) : BLOCK;
			for (unsigned i = 0; i < k; i++) {
				float v = a * (float)(sin(ph1) + sin(ph2));
				ph1 += 2 * M_PI * 1000 / RATE;
				ph2 += 2 * M_PI * 3100 / RATE;
				b2[2 * i] = b2[2 * i + 1] = v;
			}
			guard_run(&g, b2, k);
			for (unsigned i = 0; i < k; i++) {
				tpk = fmaxf(tpk, fabsf(b2[2 * i]));
				if (done + i >= n - 48000) {
					tail[nt++] = b2[2 * i];
				}
			}
			done += k;
		}
		double tot = 0;
		for (unsigned long i = 0; i < nt; i++) {
			tot += (double)tail[i] * tail[i];
		}
		double tones = 2 * (bin_power(tail, nt, 1000) + bin_power(tail, nt, 3100)) / nt;
		float rest_db = (float)(10 * log10(fmax(tot - tones, 1e-30) / tot));
		printf("  %-16s level %6.2f dB, peak %6.2f dBFS, everything else %6.1f dB, lowest gain %.1f dB\n",
		    mode ? "amps protect:" : "Mi 8 guard:", 10 * log10(tot / nt / 0.5), 20 * log10f(tpk), rest_db, guard_take_min_db(&g));
		if (mode) {
			check(rest_db < -50.0f, "amps protect: the limiter distorts two tones");
			check(20 * log10f(tpk) <= 0.01f, "amps protect: a two-tone peak over 0 dBFS");
		}
	}

	printf("  a loud burst, then quiet: the gain comes back\n");
	guard_init(&g, RATE, 12.0f);
	guard_amp_protects(&g);
	tone_thd(&g, 1000, 0.7f, 0.5f, &first, &last, &pk, &thd);
	check(pk <= 0.01f, "amps protect: a burst peak over 0 dBFS");
	guard_take_min_db(&g);
	tone_thd(&g, 1000, powf(10, -32 / 20.0f), 2, &first, &last, &pk, &thd);
	printf("  quiet tone after it: last second %6.2f dB (want -20.0)\n", last);
	check(fabsf(last - (-20.0f)) < 0.2f, "amps protect: the gain did not come back after a burst");

	printf("  a full-scale click from silence: never over the ceiling\n");
	guard_init(&g, RATE, 12.0f);
	guard_amp_protects(&g);
	static float clk[2 * BLOCK];
	memset(clk, 0, sizeof(clk));
	for (unsigned i = 300; i < 306; i++) {
		clk[2 * i] = clk[2 * i + 1] = (i & 1) ? -1.0f : 1.0f;
	}
	guard_run(&g, clk, BLOCK);
	float cpk = 0;
	for (unsigned i = 0; i < 2 * BLOCK; i++) {
		cpk = fmaxf(cpk, fabsf(clk[i]));
	}
	printf("  click peak %6.2f dBFS\n", 20 * log10f(cpk + 1e-30f));
	check(20 * log10f(cpk + 1e-30f) <= 0.01f, "amps protect: a click over 0 dBFS");

	/* the Poco F1 as the driver runs it (kProtectsMakeupDB): at full volume Apple's chain sends up to full scale, its
	 * VEQv2 pre-gain is 0 dB there, so the make-up is 0 dB and the guard leaves the sound alone */
	printf("full volume on the Poco F1: 1 kHz at 0 dBFS, no make-up, 10 s:\n");
	guard_init(&g, RATE, 0.0f);
	guard_amp_protects(&g);
	tone_thd(&g, 1000, 1.0f, 10, &first, &last, &pk, &thd);
	lowest = guard_take_min_db(&g);
	printf("  amps protect:    first second %6.2f dB, last %6.2f dB, peak %6.2f dBFS, distortion %6.1f dB, lowest gain %.2f dB\n",
	    first, last, pk, thd, lowest);
	check(pk <= 0.01f, "no make-up: a peak over 0 dBFS");
	check(lowest > -0.05f, "no make-up: the guard turned a full-scale tone down");
	check(thd < -60.0f, "no make-up: the guard distorts a full-scale tone");
	/* bass at full volume (2026-11-07: Safari's music lost up to 3.6 dB on its beats when the high-pass raised the
	 * peaks into a -1 dBFS ceiling): 150 Hz at -3 dBFS comes out as it went in */
	printf("  150 Hz at -3 dBFS, no make-up, 10 s: untouched (no high-pass on the Poco)\n");
	guard_init(&g, RATE, 0.0f);
	guard_amp_protects(&g);
	tone_thd(&g, 150, powf(10, -3 / 20.0f), 10, &first, &last, &pk, &thd);
	lowest = guard_take_min_db(&g);
	printf("  amps protect:    first second %6.2f dB, last %6.2f dB, peak %6.2f dBFS, lowest gain %.2f dB\n",
	    first, last, pk, lowest);
	check(fabsf(last - (-3.0f)) < 0.05f && fabsf(first - (-3.0f)) < 0.05f, "no make-up: 150 Hz did not come out at -3 dB");
	check(lowest > -0.05f, "no make-up: the guard turned 150 Hz down");
	printf("  the same tone 12 dB lower (the slider a little under the top): untouched\n");
	guard_init(&g, RATE, 0.0f);
	guard_amp_protects(&g);
	tone_thd(&g, 1000, powf(10, -12 / 20.0f), 3, &first, &last, &pk, &thd);
	lowest = guard_take_min_db(&g);
	printf("  amps protect:    last second %6.2f dB, peak %6.2f dBFS, lowest gain %.1f dB\n", last, pk, lowest);
	check(fabsf(last - (-12.0f)) < 0.1f, "no make-up: a -12 dBFS tone did not come out at -12 dB");
	check(lowest > -1.0f, "no make-up: the limiter touched a -12 dBFS tone");
}

int
main(void)
{
	struct ss_guard g;
	float pw, pk;

	printf("1 kHz at -12 dBFS (what Apple's chain leaves of full scale), +12 dB make-up, 70 s:\n");
	guard_init(&g, RATE, 12.0f);
	float rep1[] = { 0.1f, 0.5f, 1, 2, 3, 5, 10, 20, 30, 45, 60, 70 };
	tone(&g, 1000, 0.25f, 70, rep1, 12, &pw, &pk);
	check(pk <= -3.0f + 0.01f, "a peak over -3 dBFS");
	float lowest = guard_take_min_db(&g);
	printf("  guard's lowest gain %.1f dB\n", lowest);
	check(lowest > -20.0f, "the gain dropped towards silence (a click)");
	/* the 60 s average sits at -9 dB at the end; the 3 s one under -6, the 0.2 s one under -3 */
	for (int k = 0; k < 3; k++) {
		printf("  average over %4.1f s: %6.2f dB (limit %6.2f)\n", g.tau[k], 10 * log10f(g.avg[k] / 0.5f),
		    10 * log10f(g.limit[k] / 0.5f));
		check(g.avg[k] <= g.limit[k] * 1.01f, "an average over its limit");
	}
	check(10 * log10f(g.avg[2] / 0.5f) > -9.3f, "the 60 s average did not reach its limit");

	printf("the same after 30 s of rest (the averages fell):\n");
	guard_rest(&g, 30);
	float rep2[] = { 0.5f, 1, 2, 3 };
	tone(&g, 1000, 0.25f, 3, rep2, 4, &pw, &pk);

	printf("1 kHz at -32 dBFS (ordinary music level after Apple's chain), 3 s: untouched but the make-up\n");
	guard_init(&g, RATE, 12.0f);
	float rep3[] = { 1, 3 };
	tone(&g, 1000, powf(10, -32 / 20.0f), 3, rep3, 2, &pw, &pk);
	check(fabsf(pw - (-20.0f)) < 0.2f, "a quiet 1 kHz tone was not just lifted by 12 dB");

	float freqs[] = { 50, 100, 200, 400, 1000 };
	float want[] = { -24.1f, -12.3f, -3.0f, -0.3f, 0.0f };   /* 2nd-order Butterworth at 200 Hz, in dB */
	printf("high-pass, quiet tones (-40 dBFS in, so no limiting), level change after the make-up:\n");
	for (int i = 0; i < 5; i++) {
		guard_init(&g, RATE, 0.0f);
		float none[] = { 99 };
		tone(&g, freqs[i], 0.01f, 2, none, 0, &pw, &pk);
		float gain = pw - 10 * log10f(0.01f * 0.01f);
		printf("  %5.0f Hz: %6.2f dB (Butterworth %6.2f)\n", freqs[i], gain, want[i]);
		check(fabsf(gain - want[i]) < 0.5f, "high-pass response");
	}

	amp_protects_cases();

	printf(failures ? "%d FAILED\n" : "all checks passed\n", failures);
	return failures ? 1 : 0;
}
