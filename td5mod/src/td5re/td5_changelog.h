/**
 * td5_changelog.h -- In-game CHANGELOG screen content (TD5RE source port).
 *
 * A flat, scrollable list of lines rendered by Screen_Changelog
 * (frontend_changelog_render in td5_frontend.c). The most recent 7 days are
 * itemised day-by-day; everything before that is condensed into an "EARLIER
 * HIGHLIGHTS" summary of the biggest changes since the port began (2026-03-31).
 *
 * Keep each CL_ITEM line <= ~62 characters so it fits the body column without
 * running under the scrollbar. Add new entries at the TOP of LAST 7 DAYS and,
 * once they age out, fold the gist into EARLIER HIGHLIGHTS.
 *
 * Included by exactly ONE translation unit (td5_frontend.c); the array is
 * file-static there.
 */
#ifndef TD5_CHANGELOG_H
#define TD5_CHANGELOG_H

/* Line styles (see frontend_changelog_render for the colours/indent). */
#define CL_SECTION 0   /* gold section heading                     */
#define CL_DATE    1   /* cyan date / sub-heading                  */
#define CL_ITEM    2   /* white bullet line                        */
#define CL_BLANK   3   /* vertical spacer (text ignored)           */

typedef struct {
    int         style;
    const char *text;
} TD5_ChangelogLine;

static const TD5_ChangelogLine k_changelog_lines[] = {

    { CL_SECTION, "LAST 7 DAYS" },
    { CL_BLANK,   "" },

    { CL_DATE,    "June 25" },
    { CL_ITEM,    "Italy (Courmayeur) no longer crashes when you start" },
    { CL_ITEM,    "  the race" },
    { CL_ITEM,    "Cop Chase: you can no longer start a race with no" },
    { CL_ITEM,    "  cops or no suspects (pick at least one of each)" },
    { CL_ITEM,    "Arrested cars now stop dead and can't be driven" },
    { CL_ITEM,    "A big red ARRESTED appears in the middle of the" },
    { CL_ITEM,    "  busted car's screen; its floating bar disappears" },
    { CL_ITEM,    "Arrested cars vanish from the split-screen map" },
    { CL_ITEM,    "Both cop and suspect feel a strong rumble on arrest" },
    { CL_ITEM,    "Every suspect shows its own bust-progress bar that" },
    { CL_ITEM,    "  shrinks with distance and shows twice as far" },
    { CL_ITEM,    "CHOOSE YOUR TEAM and COP CHASE - ROLES now show" },
    { CL_ITEM,    "  each player's profile name instead of PLAYER 1/2" },
    { CL_ITEM,    "Cup team mode: the host can scroll to the AI" },
    { CL_ITEM,    "  opponents and set each one's team and skill" },
    { CL_ITEM,    "  (per-opponent slider) — they really drive at it" },
    { CL_ITEM,    "Dev: host can also assign teams to ADD AI PLAYER bots" },
    { CL_ITEM,    "Cup final standings now show each player's profile" },
    { CL_ITEM,    "  name instead of PLAYER 1 / PLAYER 2" },
    { CL_ITEM,    "WHAT NEXT? menu: CAR SELECTION goes straight to the" },
    { CL_ITEM,    "  car grid (was sending you back to profile select)" },
    { CL_ITEM,    "WHAT NEXT? buttons now line up under the title like" },
    { CL_ITEM,    "  every other menu screen" },
    { CL_ITEM,    "Game is now crisp on high-DPI / scaled displays:" },
    { CL_ITEM,    "  no more blur on 4K screens at 150%/300% scaling" },
    { CL_ITEM,    "Fixed menu UP/LEFT going dead with several controllers" },
    { CL_ITEM,    "  connected (often after a race or entering a game" },
    { CL_ITEM,    "  mode): a stuck/off-centre stick no longer jams those" },
    { CL_ITEM,    "  directions for everyone" },
    { CL_ITEM,    "Fixed controller rumble dying after a few races: force" },
    { CL_ITEM,    "  feedback now keeps working on every pad race after race" },
    { CL_ITEM,    "Multiplayer results table tidied: name column lines" },
    { CL_ITEM,    "  up under the title, columns closer, cleaner divider" },
    { CL_ITEM,    "Cup races now show a CUP RESULTS screen with a POINTS" },
    { CL_ITEM,    "  column (points this race + running cup total)" },
    { CL_ITEM,    "Empty split-screen cells never repeat: with 7" },
    { CL_ITEM,    "  players one shows the MAP, the other the STANDINGS" },
    { CL_ITEM,    "Split-screen map now zooms and turns smoothly" },
    { CL_ITEM,    "  instead of flickering and snapping around" },
    { CL_ITEM,    "Split-screen overview map no longer draws a stray" },
    { CL_ITEM,    "  diagonal line across point-to-point tracks (Sydney)" },
    { CL_ITEM,    "CUP - WHAT NEXT? menu tidied: title moved to the top" },
    { CL_ITEM,    "  and the buttons realigned under it, a bit narrower" },
    { CL_ITEM,    "Fixed 7th+ split-screen car driving rolled onto its" },
    { CL_ITEM,    "  side / wobbling — big fields now sit flat on the road" },
    { CL_ITEM,    "Multiplayer cups: choose the track, traffic and" },
    { CL_ITEM,    "  police for every race, one at a time; set how" },
    { CL_ITEM,    "  many AI opponents race for points; and the race" },
    { CL_ITEM,    "  now ends the moment all humans finish" },
    { CL_ITEM,    "Dev: ADD AI PLAYER in the multiplayer lobby fills" },
    { CL_ITEM,    "  seats with auto-driving bots (random name/colour/" },
    { CL_ITEM,    "  car) so you can test split-screen MP on your own" },
    { CL_ITEM,    "CHANGELOG screen: bulleted items, more spacing, and" },
    { CL_ITEM,    "  the PENDING TO TEST button moved up by the title" },
    { CL_ITEM,    "New split-screen post-race menu (race again / view" },
    { CL_ITEM,    "  replay / back to lobby / car / mode / main menu)" },
    { CL_ITEM,    "Police chase: cops 2.5x tougher than traffic, never" },
    { CL_ITEM,    "  launch airborne on impact, and dodge traffic" },
    { CL_ITEM,    "Car-select 3rd stat bar now shows HANDLING (real" },
    { CL_ITEM,    "  car inertia) instead of an empty GRIP bar" },
    { CL_ITEM,    "Force feedback: new rumble backend lifts the 4-pad" },
    { CL_ITEM,    "  cap, so a 5th+ controller can rumble" },
    { CL_ITEM,    "Split-screen confirms before backing out on every" },
    { CL_ITEM,    "  setup screen (no more accidental drop-back)" },
    { CL_BLANK,   "" },

    { CL_DATE,    "June 24" },
    { CL_ITEM,    "Aston Martin Vantage stat bars no longer draw blank" },
    { CL_ITEM,    "Reinstated the car horn + Test Drive 6 driver horns" },
    { CL_ITEM,    "Shorter controller names so they fit the lobby row" },
    { CL_ITEM,    "Catch-up now paces off the opponent directly ahead" },
    { CL_ITEM,    "Traffic: lighter on impact, no despawning in front" },
    { CL_ITEM,    "  of you, and fills sparse stretches of road" },
    { CL_ITEM,    "Controller slots stay put across a pad disconnect" },
    { CL_ITEM,    "Opponent name plates sit below the wheels at 50%" },
    { CL_ITEM,    "Reset-car moved to SELECT; CHANGE VIEW moved to L3" },
    { CL_ITEM,    "Force feedback routed to the correct split-screen pad" },
    { CL_ITEM,    "Auto stuck-recovery removed: manual only + 5s cooldown" },
    { CL_ITEM,    "Cop Chase: multi-cop roles + cop-only car select" },
    { CL_ITEM,    "Empty split-screen map hides finished cars and zooms" },
    { CL_ITEM,    "  to the live field, with cleaner roads" },
    { CL_ITEM,    "Fixed circuit finish placing you 2nd despite winning" },
    { CL_ITEM,    "Per-viewport engine audio panning for 3+ players" },
    { CL_ITEM,    "6-player car-select: two columns, no clipped text" },
    { CL_BLANK,   "" },

    { CL_DATE,    "June 23" },
    { CL_ITEM,    "Player colours now reach the mode-vote screen" },
    { CL_ITEM,    "AI commits to track branches earlier (no missed forks)" },
    { CL_ITEM,    "TD6 city checkpoints add time; timeout hides the star" },
    { CL_ITEM,    "Traffic drives one-way with a pile-up braking floor" },
    { CL_ITEM,    "Traffic renders in 6+ opponent single-player fields" },
    { CL_ITEM,    "TD5 app icon shows on the taskbar of other PCs" },
    { CL_ITEM,    "Track Studio: web tool to import & edit custom tracks" },
    { CL_ITEM,    "  (textures, skybox, lanes, branches) + textured road" },
    { CL_BLANK,   "" },

    { CL_DATE,    "June 22" },
    { CL_ITEM,    "Custom-track converter: a centerline becomes a" },
    { CL_ITEM,    "  drivable level, with branch/junction support" },
    { CL_ITEM,    "Car Studio: browser tool to view/edit/build cars;" },
    { CL_ITEM,    "  imports glTF / OBJ / FBX / .blend models" },
    { CL_ITEM,    "Multiplayer game modes: Time Trial, Cup, Cop Chase" },
    { CL_ITEM,    "Shared frontend screen routine + consistent titles" },
    { CL_BLANK,   "" },

    { CL_DATE,    "June 21" },
    { CL_ITEM,    "Custom-car pipeline with drop-in auto-enumerated slots" },
    { CL_ITEM,    "Cop Chase polish: strobe, banner, siren, indicator" },
    { CL_ITEM,    "Tamed the violent launch / roll on steep descents" },
    { CL_ITEM,    "Gentle flip-recovery (coast, then settle upright)" },
    { CL_ITEM,    "Per-device MP profiles + pad-reconnect modal" },
    { CL_BLANK,   "" },

    { CL_DATE,    "June 19-20" },
    { CL_ITEM,    "Netplay render decoupled from the lockstep barrier" },
    { CL_ITEM,    "  so the client frame-rate is no longer host-capped" },
    { CL_ITEM,    "Multi-cop, deterministic traffic pursuit + LOST screen" },
    { CL_ITEM,    "TD6 batch: reverse banners, spacing, gear-1, circuits" },
    { CL_BLANK,   "" },

    { CL_DATE,    "June 18" },
    { CL_ITEM,    "TD6 AI drives the track's authored racing line" },
    { CL_ITEM,    "Reverse-direction checkpoints; grass bleeds speed" },
    { CL_BLANK,   "" },
    { CL_BLANK,   "" },

    { CL_SECTION, "EARLIER HIGHLIGHTS" },
    { CL_DATE,    "since the port began, March 2026" },
    { CL_BLANK,   "" },
    { CL_ITEM,    "Clean-room C source port from a full Ghidra decompile;" },
    { CL_ITEM,    "  the DirectDraw/D3D3 layer is replaced by a D3D11" },
    { CL_ITEM,    "  backend, so the game runs with no original DLLs." },
    { CL_BLANK,   "" },
    { CL_ITEM,    "Online netplay: Winsock2 UDP lockstep (replacing" },
    { CL_ITEM,    "  DirectPlay), LAN lobby with roster + passwords," },
    { CL_ITEM,    "  and UPnP port mapping for hosting." },
    { CL_BLANK,   "" },
    { CL_ITEM,    "Split-screen up to 9 players / 16 racer slots, a" },
    { CL_ITEM,    "  press-to-join lobby, and full multi-device joystick" },
    { CL_ITEM,    "  + force-feedback support." },
    { CL_BLANK,   "" },
    { CL_ITEM,    "Test Drive 6 content ported in: extra cars and the" },
    { CL_ITEM,    "  point-to-point city tracks (London, Hong Kong)" },
    { CL_ITEM,    "  with lighting zones and breakable street props." },
    { CL_BLANK,   "" },
    { CL_ITEM,    "Custom content: Car Studio + Track Studio tools and" },
    { CL_ITEM,    "  import pipelines for your own cars and tracks." },
    { CL_BLANK,   "" },
    { CL_ITEM,    "Rendering overhaul: procedural wheels, terrain-hugging" },
    { CL_ITEM,    "  shadows, texture-free VFX, multi-threaded panes, and" },
    { CL_ITEM,    "  an FPS-independent camera with working VSync." },
    { CL_BLANK,   "" },
    { CL_ITEM,    "AI overhaul: ray-sensing opponents and GTA-style" },
    { CL_ITEM,    "  dynamic traffic." },
    { CL_BLANK,   "" },
    { CL_ITEM,    "Physics: sub-tick render interpolation, hill-climb" },
    { CL_ITEM,    "  assist, and steep-slope launch/roll fixes." },
    { CL_BLANK,   "" },
    { CL_ITEM,    "Frontend revamp: crisp TTF vector text, reworked" },
    { CL_ITEM,    "  Options/Configure, car-select stat bars, saves." },
    { CL_BLANK,   "" },
    { CL_ITEM,    "Editable assets: JSON/PNG sources are packed on load," },
    { CL_ITEM,    "  retiring the binary .DAT files; tidy INI config." },
    { CL_BLANK,   "" },
    { CL_ITEM,    "Plus a LAN release auto-updater and publish tooling." },
    { CL_BLANK,   "" },
    { CL_BLANK,   "" },
    { CL_DATE,    "-- end of changelog --" },
    { CL_BLANK,   "" },
};

#define TD5_CHANGELOG_LINE_COUNT \
    ((int)(sizeof(k_changelog_lines) / sizeof(k_changelog_lines[0])))

#endif /* TD5_CHANGELOG_H */
