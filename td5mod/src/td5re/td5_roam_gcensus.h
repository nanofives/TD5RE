/**
 * td5_roam_gcensus.h -- GEO FREE ROAM: planner graph vs drivable-surface census (PORT-ONLY, DEV-ONLY, G2).
 * See td5_roam_gcensus.c.
 */
#ifndef TD5_ROAM_GCENSUS_H
#define TD5_ROAM_GCENSUS_H

#ifndef TD5RE_RELEASE
/* DEV ONLY. TD5RE_ROAM_GRAPH_CENSUS=1: loads every casco ROAMTILE, builds the planner graph, samples every
 * edge centre line every 2 m against the drivable surface, writes <log dir>/roam_graph_census.{txt,csv} and
 * returns 1 (the caller exits). 0 when the knob is off. */
int td5_roam_graph_census_dev_test(void);
#endif

#endif /* TD5_ROAM_GCENSUS_H */
