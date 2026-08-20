/* ========================================================================
 * td5_carbalance.h — tracked per-car carparam corrections (PORT-ONLY).
 *
 * WHY THIS EXISTS: the per-car performance data lives in
 * re/assets/cars/<code>/carparam.json, which is NOT tracked by git (0 of 77
 * car dirs are in the index). An edit there is local-only: unversioned, not
 * deployed, and lost on a fresh clone. This module is the tracked home for
 * corrections to that data — a small table applied to the 268-byte carparam
 * image at load, so the fix ships with the source instead of living on one
 * machine's disk.
 *
 * WHERE IT HOOKS: td5_asset_open_and_read() — the single choke point every
 * carparam consumer passes through (physics load, frontend MORE STATS, the
 * handling bar, the AI speed pool, the traffic top-speed lookup). Hooking
 * there is deliberate: td5_carparam.h's contract is that the display bars and
 * the weight mechanics agree BY CONSTRUCTION, which only holds if every reader
 * sees the same bytes. Do not move this to a single consumer.
 *
 * KNOB: TD5RE_CAR_BALANCE — default ON; set to 0 to load the raw asset values
 * (A/B the corrections without editing anything).
 * ======================================================================== */
#ifndef TD5_CARBALANCE_H
#define TD5_CARBALANCE_H

/* Apply any tracked carparam corrections in place. No-op unless entry_name is
 * carparam.dat, the buffer is a full 268-byte image, and the car code parsed
 * from zip_path has table entries. Safe to call for every asset read. */
void td5_carbalance_apply(const char *entry_name, const char *zip_path,
                          void *data, int size);

#endif /* TD5_CARBALANCE_H */
