/**
 * td5_roam_ram_test.h -- GEO FREE ROAM F8e: the `roam-ram` self-test (DEV-ONLY, see td5_roam_ram_test.c).
 */
#ifndef TD5_ROAM_RAM_TEST_H
#define TD5_ROAM_RAM_TEST_H

#include <stdint.h>
#include <stddef.h>

struct TD5_Actor;

#ifndef TD5RE_RELEASE
void     td5_roam_ram_test_arm(void);
void     td5_roam_ram_test_disarm(void);
uint32_t td5_roam_ram_test_bits(struct TD5_Actor *a);   /* slot 0 control bits, 0 when idle */
int      td5_roam_ram_test_done(void);
int      td5_roam_ram_test_verdict(char *note, size_t n);   /* 0 PASS / 2 FAIL */
#endif

#endif /* TD5_ROAM_RAM_TEST_H */
