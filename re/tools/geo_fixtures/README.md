# geo_fixtures -- conditioned routes committed so the GEO pipeline can be
# tested with no network and no place cache

Each fixture is a pair: the raw `[lat, lon]` polyline the conditioner was given,
and the `ROUTE.JSON` it produced. Point the game at the second one and it builds
that route over whatever world the seed gives:

    TD5RE_GEO_ROUTE=<abs path to ...\_ROUTE.json>   td5re.exe --AutoRace=1 ...

No `TD5RE_GEO_PLACE` is needed. That is the whole reason these exist: the real
place caches (`re/assets/geo/<slug>/`) are gitignored and need one authorised
network fetch, so without a fixture there is nothing to test the road side with.

| Fixture | What it is for | Regenerate with |
|---|---|---|
| `la_plata_*` | a real A-to-B city route (Phase 3). 1492 spans, no self-crossing. | `python re/tools/geo_condition.py --in la_plata_route_raw.json --out la_plata_ROUTE.json` |
| `figure8_*` | the OPTION B fixture: a route that genuinely crosses itself, so the grade separation and the crossing-safe localiser have something to act on. 657 spans, 1 crossing site. | `python re/tools/geo_condition.py --synth figure8 --allow-crossings --raw-out figure8_route_raw.json --out figure8_ROUTE.json` |

## figure8 -- the numbers to expect, and the seed they were measured on

A Gerono lemniscate traversed so it passes through the origin twice with a 62
degree crossing angle (`_synth_latlon` in `geo_condition.py` has the derivation).
The conditioner reports one crossing site, **nodes 131..135 against 562..566**,
and plans a grade separation with the LATER leg on the deck -- it has 91 spans of
ramp room against the earlier leg's 82.

**Build the fixture with `-Seed 20260902` AND `TD5RE_AUTOTRACK_SCENERY=1`.**
Both halves are load-bearing and each was learned the hard way:

* `20260901` is the pinned SYNTHETIC byte-identity seed (`verify/topo_gen.ps1`,
  MODELS.DAT `98E749869051ACE3`). It is pinned because it must never move, not
  because it is a good place to put a route. Over that world the figure-eight
  runs across water, and `tg_network_audit.py` then fails its "open span over
  water without a deck" rule -- **identically on a pre-Option-B binary**, so
  that failure is the route meeting the world, not anything the grade
  separation does. `20260902` puts the same route on dry land.
* `20260902`'s own R21 spec roll comes out **`SCENERY = OFF`**, and a
  scenery-off build writes no MODELS.DAT, no TEXTURES.DAT and no MESHTAG.BIN at
  all. It still writes NETWORK.JSON, so the network audit runs and reports OK --
  **a green audit on a track with no geometry in it.** Pin the knob, then check
  MODELS.DAT actually exists before believing any audit result on this fixture.

Measured on `20260902` + scenery pinned (2026-09-30):

    MODELS.DAT        3580548 bytes   (0 bytes of scenery if the knob is unset)
    [GEO XSEP] site 0 BUILT: ... 3079 units (7.1 m) of clearance,
               soffit 2599 above it; worst grade in the window 0.144
    tg_network_audit  RESULT: OK
    tg_strip_audit    657 spans, 0 violations

Drive it with `verify/xspan_leg.ps1`, one run per leg -- the point of Option B is
that a car on the deck and a car underneath each keep their own span, which is
two measurements, not one.
