/*
 * td5_msvc_rand.c -- MSVC-compatible rand()/srand() override.
 *
 * Overrides mingw libc's rand()/srand() so the port's RNG sequence matches
 * MSVC 6.0's CRT (the compiler TD5_d3d.exe was built with). Verified by
 * paired Frida + pool13 capture: identical srand(0x1A2B3C4D) seed produces
 * DIFFERENT sequences on mingw vs MSVC, causing AI car selection to land
 * on different cardefs for slots 1..5 -> different body_probes -> different
 * RS_RIGHT_BOUNDARY_A/B -> different active_upper/lower_bound, with cascade
 * into find_offset_peer AABB gate -> lat_offset_bias accumulation pattern.
 *
 * MSVC CRT linear-congruential generator (since MSC 6.0):
 *   _holdrand = _holdrand * 0x343FD + 0x269EC3
 *   return (_holdrand >> 16) & 0x7FFF
 *
 * Constants: 0x343FD = 214013, 0x269EC3 = 2531011, mask = 32767 (RAND_MAX).
 *
 * Linker: defining rand/srand at file scope here makes them strong symbols
 * that the mingw libc weak/static rand definitions yield to. No --wrap or
 * -Wl,-u needed: GNU ld resolves to our definitions when libtd5re.a comes
 * before -lmingw32/-lmsvcrt on the link line (which it does in
 * build_standalone.bat).
 */
#include <stdint.h>

/* Match MSVC's per-thread _holdrand sentinel: 1 at startup, mutated by
 * srand(). Single-threaded since TD5 is single-threaded gameplay. */
static uint32_t s_holdrand = 1u;

void srand(unsigned int seed) {
    s_holdrand = (uint32_t)seed;
}

int rand(void) {
    s_holdrand = s_holdrand * 0x343FDU + 0x269EC3U;
    return (int)((s_holdrand >> 16) & 0x7FFFU);
}
