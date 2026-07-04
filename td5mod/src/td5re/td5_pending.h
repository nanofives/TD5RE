/**
 * td5_pending.h -- Dev/QA "pending to test" checklist + in-game overlay.
 *
 * A flat list of features awaiting manual testing. On the PENDING TO TEST
 * menu, ENTER on a row opens a details modal (td5_pending_detail_text) with a
 * longer, testing-focused note; SUPR/DELETE drops the row outright once it's
 * been verified. An optional in-game overlay lists every item, right-justified
 * on the right edge of the screen (like the dev debug data).
 *
 * Persistence: an UNTRACKED plain-text file next to the exe (td5re_pending.txt),
 * seeded from a compiled-in default list on first run. The file is the source of
 * truth thereafter (no re-merge of defaults, so /end-deleted items stay gone).
 */
#ifndef TD5_PENDING_H
#define TD5_PENDING_H

/* Module lifecycle (registered in td5re.c module table, after "save"). */
int  td5_pending_init(void);       /* load the file, or seed + write it on first run */
void td5_pending_shutdown(void);   /* no-op: writes happen on each toggle            */

/* List access. */
int         td5_pending_count(void);
const char *td5_pending_text(int i);
const char *td5_pending_detail_text(int i);   /* longer test-focus note for the ENTER modal;
                                                  falls back to a generic note if none recorded */
void        td5_pending_delete(int i);     /* drop item i + persist (SUPR on the menu) */
void        td5_pending_save(void);        /* rewrite the backing file now */

/* In-game overlay state (the actual right-justified draw lives in td5_hud.c,
 * td5_hud_draw_pending_overlay, so it matches the debug-stats HUD font). */
int  td5_pending_overlay_on(void);
void td5_pending_set_overlay(int on);
void td5_pending_toggle_overlay(void);

#endif /* TD5_PENDING_H */
