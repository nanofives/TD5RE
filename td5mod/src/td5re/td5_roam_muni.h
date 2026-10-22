/* td5_roam_muni.h -- GEO FREE ROAM: the MUNICIPAL LINE (linea municipal) of a block (PORT-ONLY, roam NA).
 *
 * Mariano, 2026-10-22: "La Plata's building code says every building must stand on the municipal line; in the game
 * buildings can be set back from the sidewalk; normalize this." The municipal line is the back edge of the pavement:
 * a street's kerb moved into the block by its pavement width, the outer line td5_roam_scenery.c's pavement chain
 * computes (pv_chain_geom without the facade clip). This module holds those line pieces for one tile ctx and moves
 * the street-facing walls of a footprint onto them:
 *
 *   - a WALL is a ring edge whose outside faces a line piece (normals within TD5_MUNI_FACE_DEG) it projects onto,
 *     with nothing of its own ring between it and the line;
 *   - a wall up to snap_max behind the line is brought forward onto it (the side walls stretch), one up to
 *     protrude_max in front of it (on the pavement) is cut back; farther ones are left (a building inside the block);
 *   - a vertex between two walls on different streets is the block corner: the two lines' crossing, cut by an
 *     OCHAVA (a chamfer square to the corner's bisector, legs of ochava_leg along each street) when the corner turns
 *     between 30 and 150 degrees. An ochava the footprint already has (a short diagonal edge) is kept: its two
 *     vertices each move onto their own street's line;
 *   - every new vertex stands behind the line (td5_muni_behind >= 0), every point of a wall on the line lies on a line
 *     piece (not on its extension), the swept area of a wall brought forward meets no other footprint (opt->blocked),
 *     the ring stays simple with at least half its area, and NO point of the result (every edge, every metre) is in
 *     front of the line, else the footprint is left as it was (an angled corner wing on the pavement, TD5_MUNI_FRONT).
 *
 * Coordinates are the caller's (tile-local units). Pure geometry: no ctx, no I/O. */
#ifndef TD5_ROAM_MUNI_H
#define TD5_ROAM_MUNI_H

typedef struct {
    double ax, az, bx, bz;      /* the line piece                                           */
    double nx, nz;              /* unit normal INTO the block (away from the road)          */
    int    chain;               /* the kerb chain it belongs to (pieces of one chain join)  */
} TD5_MuniSeg;

typedef struct TD5_Muni TD5_Muni;

TD5_Muni *td5_muni_new(void);
void td5_muni_free(TD5_Muni *m);
void td5_muni_reset(TD5_Muni *m);
int  td5_muni_add(TD5_Muni *m, const TD5_MuniSeg *s);
/* bucket the pieces over [x0,x1]x[z0,z1] (cells of `cell`); call once after the last add */
int  td5_muni_index(TD5_Muni *m, double x0, double z0, double x1, double z1, double cell);
int  td5_muni_count(const TD5_Muni *m);

/* How far (x, z) stands BEHIND the line (> 0 inside the block, < 0 on the pavement / road side): the piece nearest
 * the point among those it projects onto within r (units). 0 = no piece within r (*g untouched). */
int  td5_muni_behind(const TD5_Muni *m, double x, double z, double r, double *g, int *seg);

typedef struct {
    double snap_max;            /* a wall farther behind than this is inside the block: left   */
    double protrude_max;        /* a wall farther in front than this is not cut back: left     */
    double tol;                 /* a wall within this of the line is already on it             */
    double inset;               /* a moved wall stands this far behind the line (no z-fight)   */
    double ochava_leg;          /* chamfer leg along each street at a corner, 0 = none          */
    double min_wall;            /* an edge shorter than this is no wall (a jog)                 */
    double reach;               /* census: walls are looked for this far behind / in front      */
    int  (*blocked)(void *u, const double *qx, const double *qz);   /* the swept quad meets another footprint */
    void  *u;
} TD5_MuniOpt;

enum {
    TD5_MUNI_NONE = 0,          /* no wall faces a line piece within reach: nothing to do      */
    TD5_MUNI_ON,                /* every wall already within tol of the line                    */
    TD5_MUNI_SNAPPED,           /* at least one wall moved onto the line                         */
    TD5_MUNI_FAR,               /* the walls that face a line are all farther than snap_max     */
    TD5_MUNI_BLOCKED,           /* a wall's swept area meets another footprint                   */
    TD5_MUNI_CORNER,            /* a corner (or its ochava) could not stand behind both lines    */
    TD5_MUNI_RING,              /* the moved ring is not simple / lost half its area / too long  */
    TD5_MUNI_ROAD,              /* (the caller's verdict) the moved ring needs a shift off the drivable surface */
    TD5_MUNI_FRONT,             /* some edge no wall rule moves (angled, short) would still stand in front of the line */
    TD5_MUNI__N
};

typedef struct {
    int fate;                   /* TD5_MUNI_*                                                    */
    int walls;                  /* walls found                                                   */
    int fwd, back;              /* walls brought forward / cut back                              */
    int corners, ochavas;       /* block corners filled, of them cut by a new ochava             */
} TD5_MuniRes;

/* Move the walls of ring (x, z)[n] onto the line: the result in (ox, oz)[*on] (cap points at most). Returns the fate
 * (res->fate too); the output is a copy of the input unless the fate is TD5_MUNI_SNAPPED. */
int  td5_muni_snap(const TD5_Muni *m, const TD5_MuniOpt *o, const double *x, const double *z, int n,
                   double *ox, double *oz, int *on, int cap, TD5_MuniRes *res);

/* No point of ring (x, z)[n] (each vertex, every metre of each edge) stands more than o->tol in front of the line. */
int  td5_muni_ring_behind(const TD5_Muni *m, const TD5_MuniOpt *o, const double *x, const double *z, int n);

/* CENSUS: the walls of ring (x, z)[n] (as td5_muni_snap finds them, within o->reach either side) and each wall's
 * gap to the line (mean of its two ends, > 0 behind). Returns the wall count, gaps in gap[] (up to cap). */
int  td5_muni_walls(const TD5_Muni *m, const TD5_MuniOpt *o, const double *x, const double *z, int n,
                    double *gap, int cap);

#endif
