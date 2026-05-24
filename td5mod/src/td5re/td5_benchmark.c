/**
 * td5_benchmark.c -- Benchmark frame-rate capture + report
 *
 * See td5_benchmark.h for the full per-function audit + orig <-> port
 * divergence map.  Summary:
 *
 *   InitializeBenchmarkFrameRateCapture (0x00428D20)  -> td5_benchmark_init_capture
 *   RecordBenchmarkFrameRateSample      (0x00428D40)  -> td5_benchmark_record_sample
 *   FormatBenchmarkReportText           (0x00428D60)  -> folded inline
 *   WriteBenchmarkResultsTgaReport      (0x00428D80)  -> td5_benchmark_write_report
 *
 * The 1921-byte orig writer composes a 640x480 24bpp TGA from DDraw
 * glyph blits (DXDraw::PrintTGA, DXDecimal) over a 256-sample bar
 * chart.  Reproducing the TGA output byte-faithfully would require
 * porting the entire DDraw font atlas + per-glyph blit path -- both
 * are architecturally absent in the D3D11 port.  We therefore emit a
 * plain-text report with the same statistics (sample count, min /
 * max / average FPS, per-sample dt) and document the divergence in
 * the orig audit header in td5_game.c.
 */

#include "td5_benchmark.h"

#include "td5re.h"
#include "td5_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define LOG_TAG "benchmark"

/* Mirrors orig g_benchmarkSampleBuffer (1MB heap alloc) and
 * g_benchmarkSampleCount (DAT_004A2CF4).  Buffer is allocated on first
 * init and reused across benchmark runs. */
static uint32_t *s_samples = NULL;
static uint32_t  s_sample_count = 0;
static uint32_t  s_sample_capacity = 0;

/* Default report path -- single shared buffer so the caller does not
 * need to manage one.  Written by td5_benchmark_default_report_path. */
static char s_default_path[260];

/* [CONFIRMED @ 0x00428D20 InitializeBenchmarkFrameRateCapture]
 * Orig allocates 1000000 bytes via HeapAllocTracked (= 250000 u32
 * slots) and zeroes g_benchmarkSampleCount.  Port allocates lazily on
 * first call; subsequent calls only zero the counter (matching orig's
 * "init at race start, accumulate, write, reset" lifecycle). */
void td5_benchmark_init_capture(void) {
    if (s_samples == NULL) {
        s_sample_capacity = TD5_BENCHMARK_MAX_SAMPLES;
        s_samples = (uint32_t *)calloc(s_sample_capacity, sizeof(uint32_t));
        if (s_samples == NULL) {
            TD5_LOG_W(LOG_TAG, "init_capture: failed to allocate %u-sample buffer",
                      s_sample_capacity);
            s_sample_capacity = 0;
        }
    }
    s_sample_count = 0;
}

/* [CONFIRMED @ 0x00428D40 RecordBenchmarkFrameRateSample]
 * Orig: *(buffer + count*4) = sample; count++.  No bounds check in
 * orig (relies on the 250000-slot cap exceeding any plausible race
 * length -- ~1.16 hours at 60Hz).  Port adds the bounds guard so a
 * runaway benchmark can't corrupt heap. */
void td5_benchmark_record_sample(uint32_t sample) {
    if (s_samples == NULL || s_sample_count >= s_sample_capacity) {
        return;
    }
    s_samples[s_sample_count++] = sample;
}

uint32_t td5_benchmark_get_sample_count(void) {
    return s_sample_count;
}

const char *td5_benchmark_default_report_path(void) {
    /* Orig uses _splitpath/_makepath on FPSName_exref to swap the
     * extension and write next to the configured asset path.  Port
     * has no FPSName_exref -- emit under log/ alongside the trace CSV
     * files. */
    snprintf(s_default_path, sizeof(s_default_path),
             "log/benchmark_%u.txt", (unsigned)td5_plat_time_ms());
    return s_default_path;
}

/* [CONFIRMED @ 0x00428D80 WriteBenchmarkResultsTgaReport]
 * Orig writes a 0x312-byte TGA header + DX::TGACompress'd pixel
 * payload composed of a 256-sample FPS chart + system-info text.
 *
 * Port divergence (ARCH-DIVERGENCE: benchmark output collapse -- see
 * td5_game.c:4852):
 *   - No DDraw glyph blit (DXDraw::PrintTGA) available in D3D11
 *   - No 16bpp TGA compressor in the source-port surface stack
 *   - No FPSName_exref _splitpath/_makepath asset alias path
 *
 * What is preserved (the gameplay-relevant data):
 *   - Sample count
 *   - Min / Max / Average FPS  (matches orig's Min FPS / Max FPS /
 *     Avg FPS labels rendered at the top of the chart)
 *   - Per-sample microsecond dt (matches orig's per-sample bar
 *     heights, just dumped as a flat list instead of a chart)
 *
 * The orig divides each sample by 3 before display (the "uVar5 / 3"
 * passed to DXDecimal) because the sample is the number of 100ns
 * QueryPerformanceCounter ticks per frame divided by some scale
 * factor -- the port stores frame microseconds directly so the
 * conversion is just 1e6/sample_us. */
int td5_benchmark_write_report(const char *path) {
    FILE *fp;
    uint32_t i;
    uint32_t min_us = UINT32_MAX;
    uint32_t max_us = 0;
    uint64_t sum_us = 0;
    double   min_fps, max_fps, avg_fps;

    if (s_samples == NULL || s_sample_count == 0) {
        TD5_LOG_W(LOG_TAG, "write_report: no samples to write");
        return 0;
    }
    if (path == NULL) {
        path = td5_benchmark_default_report_path();
    }

    /* Pass 1: Min/Max/Sum over samples (mirrors orig 0x00428D80 loop
     * that maintains local_64c / local_640 / local_638). */
    for (i = 0; i < s_sample_count; i++) {
        uint32_t s = s_samples[i];
        if (s == 0) continue;  /* orig skips zero-dt samples too */
        if (s < min_us) min_us = s;
        if (s > max_us) max_us = s;
        sum_us += s;
    }
    if (sum_us == 0) {
        TD5_LOG_W(LOG_TAG, "write_report: all samples zero, nothing to write");
        return 0;
    }

    min_fps = (min_us > 0) ? 1.0e6 / (double)min_us : 0.0;
    max_fps = (max_us > 0) ? 1.0e6 / (double)max_us : 0.0;
    avg_fps = ((double)s_sample_count * 1.0e6) / (double)sum_us;

    fp = fopen(path, "w");
    if (fp == NULL) {
        TD5_LOG_W(LOG_TAG, "write_report: fopen('%s') failed", path);
        return 0;
    }

    /* Header mirrors the labels orig blits onto the TGA. */
    fprintf(fp, "TD5RE Benchmark Report\n");
    fprintf(fp, "======================\n");
    fprintf(fp, "Samples       : %u\n", s_sample_count);
    fprintf(fp, "Min FPS       : %.2f  (%u us)\n", min_fps, min_us);
    fprintf(fp, "Max FPS       : %.2f  (%u us)\n", max_fps, max_us);
    fprintf(fp, "Average FPS   : %.2f  (%.2f us avg)\n",
            avg_fps, (double)sum_us / (double)s_sample_count);
    fprintf(fp, "Total time    : %.3f s\n", (double)sum_us / 1.0e6);
    fprintf(fp, "\n");
    fprintf(fp, "Per-sample frame time (us):\n");
    for (i = 0; i < s_sample_count; i++) {
        fprintf(fp, "%u\n", s_samples[i]);
    }

    fclose(fp);

    /* Mirrors orig's LogReport("Min FPS: %d Max FPS: %d Avg FPS: %d")
     * call at the end of WriteBenchmarkResultsTgaReport. */
    TD5_LOG_I(LOG_TAG,
              "Report: samples=%u min_fps=%.2f max_fps=%.2f avg_fps=%.2f path='%s'",
              s_sample_count, min_fps, max_fps, avg_fps, path);
    return 1;
}

void td5_benchmark_shutdown(void) {
    if (s_samples != NULL) {
        free(s_samples);
        s_samples = NULL;
    }
    s_sample_count = 0;
    s_sample_capacity = 0;
}
