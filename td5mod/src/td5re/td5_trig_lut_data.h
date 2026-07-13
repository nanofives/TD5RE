/* td5_trig_lut_data.h -- baked 12-bit trig LUT (see td5_trig_lut_data.c).
 * g_sinCosFloatTable @ 0x00488984 dumped from TD5_d3d.exe. */

#ifndef TD5_TRIG_LUT_DATA_H
#define TD5_TRIG_LUT_DATA_H

#include <stdint.h>

#define TD5_TRIG_LUT_SIZE 0x1400  /* 5120, matches original */

extern const uint32_t td5_trig_lut_bits[TD5_TRIG_LUT_SIZE];

#endif /* TD5_TRIG_LUT_DATA_H */
