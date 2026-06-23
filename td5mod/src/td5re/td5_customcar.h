/*
 * td5_customcar.h -- drop-in custom-car registry.
 *
 * Scans re/assets/cars/custom_<name>/ at startup and registers each folder as an
 * extra car slot beyond the 76 built-in cars (index 76 .. 76+count-1), so a car
 * produced by re/tools/td5_car_import.py appears in SELECT CAR with no source
 * edit. The folder layout is identical to a built-in car (himodel.bin +
 * carparam.json + carskin/carhub/carpic PNGs + config.nfo); the asset loader
 * resolves "cars/<dir>.zip" -> re/assets/cars/<dir>/ exactly as for a stock car.
 *
 * Discovery is sorted by folder name so a given custom car gets the SAME index
 * on every machine -- required for netplay / replay determinism (car index is
 * part of the replicated race config).
 */
#ifndef TD5_CUSTOMCAR_H
#define TD5_CUSTOMCAR_H

/* Max custom cars beyond the 76 built-ins. Sizes the extra room in the per-car
 * frontend cache arrays (TD5_CAR_SLOT_MAX = TD5_CAR_COUNT + this). */
#define TD5_CUSTOM_CAR_MAX 16

/* Scan re/assets/cars/custom_<name>/ once (idempotent). Returns the discovered
 * count. Safe to call from any thread-free startup path; self-initialises on
 * the first td5_customcar_count()/_zip_path() if not called explicitly. */
int td5_customcar_init(void);

/* Number of discovered custom cars (0..TD5_CUSTOM_CAR_MAX). */
int td5_customcar_count(void);

/* Registered zip-path for custom car `ci` in [0,count): "cars/custom_<name>.zip"
 * (the asset loader maps this to re/assets/cars/custom_<name>/). NULL if out of
 * range. */
const char *td5_customcar_zip_path(int ci);

/* Total selectable car count = built-in 76 + custom count. Defined in
 * td5_frontend.c (where TD5_CAR_COUNT is authoritative). */
int td5_car_total_count(void);

#endif /* TD5_CUSTOMCAR_H */
