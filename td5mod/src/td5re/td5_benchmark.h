/**
 * td5_benchmark.h -- Benchmark frame-rate capture + report
 *
 * Tier 5 port of the orig benchmark report pipeline:
 *
 *   0x00428D20  InitializeBenchmarkFrameRateCapture
 *     Orig allocs HeapAllocTracked(1000000) into g_benchmarkSampleBuffer
 *     and zeroes g_benchmarkSampleCount.  Port mirrors with a heap u32
 *     ring of TD5_BENCHMARK_MAX_SAMPLES entries; allocator is libc.
 *
 *   0x00428D40  RecordBenchmarkFrameRateSample
 *     Orig: g_benchmarkSampleBuffer[g_benchmarkSampleCount++] = sample.
 *     Port: identical, with bounds guard.
 *
 *   0x00428D60  FormatBenchmarkReportText
 *     Orig: small wvsprintfA wrapper for report-text formatting.
 *     Port: collapsed into the report writer (no consumers outside it).
 *
 *   0x00428D80  WriteBenchmarkResultsTgaReport
 *     Orig: writes a 640x480 24bpp TGA composed of:
 *       - 256-sample FPS strip chart (rows 0x6428..0x3b627 of pixel buf)
 *       - System info text (Processor / Memory / OS / Screen Mode /
 *         Texture Memory) via DXDraw::PrintTGA + DXDecimal glyph blits
 *       - Min/Max/Average FPS labels at the top of the chart
 *       - Path resolved via _splitpath/_makepath on FPSName_exref so the
 *         output file lives alongside the input asset
 *     Port: emits a portable plain-text frame-time report (frame times
 *     in microseconds + min/max/avg FPS) to the path returned by
 *     td5_benchmark_default_report_path().  The DDraw 16bpp TGA blit
 *     + DXDraw font system are not portable to D3D11 without
 *     reconstructing the original font atlas; this is documented as
 *     ARCH-DIVERGENCE in td5_game.c:4863.
 *
 * Wiring:
 *   td5_benchmark_init_capture()   -- called from td5_game_init_race_session
 *                                     when benchmark_active is set
 *   td5_benchmark_record_sample()  -- called once per rendered frame from
 *                                     td5_game_run_race_frame
 *   td5_benchmark_write_report()   -- called when the race transitions to
 *                                     TD5_GAMESTATE_BENCHMARK
 *
 * The capture is a no-op (and the report is suppressed) when
 * benchmark_active is 0, so non-benchmark races pay zero overhead.
 */

#ifndef TD5_BENCHMARK_H
#define TD5_BENCHMARK_H

#include <stddef.h>
#include <stdint.h>

/* Orig sample buffer is 1MB / 4 = 262144 u32 slots, but the orig writer
 * draws into a 256-sample strip; cap at 1MB-equivalent for parity. */
#define TD5_BENCHMARK_MAX_SAMPLES  262144

/* [CONFIRMED @ 0x00428D20] Reset the per-frame sample ring + counter. */
void td5_benchmark_init_capture(void);

/* [CONFIRMED @ 0x00428D40] Append one frame-time sample (microseconds). */
void td5_benchmark_record_sample(uint32_t sample);

/* Returns the number of samples currently captured. */
uint32_t td5_benchmark_get_sample_count(void);

/* [CONFIRMED @ 0x00428D80] Write the report (ARCH-DIVERGENT format).
 * Returns 1 on success, 0 if no samples or write failed. */
int  td5_benchmark_write_report(const char *path);

/* Returns the default report path under log/ (single shared buffer). */
const char *td5_benchmark_default_report_path(void);

/* Release the sample buffer.  Safe to call without a prior init. */
void td5_benchmark_shutdown(void);

#endif /* TD5_BENCHMARK_H */
