# Autotrack feedback round 2 (2026-08-27)

Verbatim user feedback from a manual drive of the merged batch (build `56347841`).
Round 1 (30 items) is merged; this is the follow-up round.

## A. Buildings / city
1. Building textures should not be stretched.
2. Taller buildings sometimes have only a facade, missing side geometry -- one
   visible face, kills the immersion.
3. Buildings must NEVER touch the road -- always a sidewalk between.
4. There are coloured lines on buildings.
5. Cities need cross streets. Building textures from different tracks may be
   mixed within the same biome.
6. Building biomes should follow the logic of an actual city: crossing streets,
   varying heights, people on the streets, streets vs avenues.
7. Elevated sidewalks are fine in the city; OUTSIDE the city a sidewalk can be
   just another texture (no raised slab).

## B. Street furniture / signage
8. Fences should be on the side NEAR the road.
9. Some fences use the same texture as buildings.
10. Lamp post textures look wrong -- use the ones TD5 tracks use. If they cannot
    be found, try OCR over the texture pages to identify them.
11. Street lamps should only be visible on NIGHT-time generated tracks.
12. Start and finish banners should be the TD5 ones that read START / FINISH.

## C. Flora
13. Primitive tree textures are showing (probably pre-migration). From now on
    ONLY use previously-used assets/geometry.
14. Background trees look wrong -- grey at the bottom, white at the top.

## D. Terrain / water / bridges / tunnels
15. Water appears ON TOP of a tunnel (seen last race) -- wrong.
16. Bridge pillars should use a different texture than bridge fences.
17. Bridge road should have a predetermined height, so pillars are not visible
    poking through the road surface during bridges.

## E. Branches / track geometry
18. Sudden changes of lane width on right-hand track branches look wrong.
19. Clean up how new branches are created; add safeguards so no other geometry
    is rendered over a branch.
20. Geometry can overlap on acute curves -- all geometry must connect smoothly.

## F. Race flow / structure
21. The finish banner appears where there is no finish line. Placement looks
    right, it is just NOT TRIGGERING RACE END.  <-- functional bug, not cosmetic
22. Night-time should be decided upon ENTERING the race.
23. Biome transitions should be smoother. At race start, set parameters per
    biome: countryside -> more bridges, mountains -> more tunnels, cities ->
    longer urban and suburban stretches. Consider biome INCOMPATIBILITY (do not
    put wildly different biomes adjacent) and reason it out.

## G. Tooling
24. Log ALL elements generated in the last run, so the generator's output can be
    understood without parsing MODELS.DAT by hand.
    STATUS 2026-08-27: we log seed, span count, section mix, biome runs, fork
    positions, texture page count, MODELS.DAT size and guardrail coverage.
    We do NOT log a per-element inventory (counts + locations per element type).
    That gap is what forced an offline MODELS.DAT parse to find the far-band
    ceiling this session.
25. Add UI elements for the knobs in a dedicated menu when the auto-generated
    track is selected.
