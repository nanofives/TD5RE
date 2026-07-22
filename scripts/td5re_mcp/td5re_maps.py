"""td5re_maps.py -- name<->code tables mirrored from the game headers.

Kept in sync by hand with:
  * TD5_ScreenIndex  (td5_types.h)  -- frontend screen indices
  * DirectInput DIK_* scancodes     -- for friendly press/hold key names

Only the stable, commonly-driven entries are listed; unknown names fall back
to raw integers so the client never hard-blocks on a missing alias.
"""
from __future__ import annotations

from typing import Dict

# --- frontend screens (TD5_ScreenIndex in td5_types.h) --------------------
SCREENS: Dict[str, int] = {
    "attract_mode": 2,
    "main_menu": 5,
    "race_type_menu": 6,
    "quick_race": 7,
    "connection_browser": 8,
    "session_picker": 9,
    "create_session": 10,
    "network_lobby": 11,
    "options_hub": 12,
    "control_options": 14,
    "sound_options": 15,
    "display_options": 16,
    "two_player_options": 17,
    "controller_binding": 18,
    "music_test": 19,
    "car_selection": 20,
    "track_selection": 21,
    "extras_gallery": 22,
    "high_score": 23,
    "race_results": 24,
    "name_entry": 25,
    "mp_lobby": 30,
    "lan_menu": 31,
    "direct_connect": 32,
    "net_nickname": 33,
    "changelog": 41,
    "pending_test": 42,
    "race_options": 44,
    "mp_guide": 45,
}
SCREEN_NAMES = {v: k for k, v in SCREENS.items()}


def resolve_screen(name_or_index) -> int:
    """Accept an int index or a friendly name; return the numeric index."""
    if isinstance(name_or_index, int):
        return name_or_index
    s = str(name_or_index).strip().lower().replace(" ", "_").replace("-", "_")
    if s.isdigit():
        return int(s)
    if s in SCREENS:
        return SCREENS[s]
    raise KeyError(f"unknown screen '{name_or_index}' (known: {', '.join(sorted(SCREENS))})")


# --- DirectInput DIK_* scancodes (the ones worth driving) -----------------
KEYS: Dict[str, int] = {
    "escape": 0x01,
    "return": 0x1C, "enter": 0x1C,
    "space": 0x39,
    "up": 0xC8, "down": 0xD0, "left": 0xCB, "right": 0xCD,
    "lctrl": 0x1D, "lshift": 0x2A, "lalt": 0x38,
    "tab": 0x0F, "backspace": 0x0E,
    # letters commonly bound to driving in keyboard play
    "w": 0x11, "a": 0x1E, "s": 0x1F, "d": 0x20,
    "1": 0x02, "2": 0x03, "3": 0x04, "4": 0x05, "5": 0x06,
    "f5": 0x3F, "f12": 0x58,
    "p": 0x19,   # pause
}


# --- race action verbs (k_actions[] in td5_inputscript.c) -----------------
ACTIONS = ("throttle", "brake", "handbrake", "horn", "gearup", "geardown",
           "camera", "rearview", "left", "right", "pause", "escape")


def resolve_action(name: str) -> str:
    """Validate an action verb name (game side does the bit lookup)."""
    s = str(name).strip().lower()
    if s in ACTIONS:
        return s
    raise KeyError(f"unknown action '{name}' (known: {', '.join(ACTIONS)})")


def resolve_key(name_or_dik) -> int:
    """Accept an int DIK scancode or a friendly name; return the scancode."""
    if isinstance(name_or_dik, int):
        return name_or_dik
    s = str(name_or_dik).strip().lower()
    if s.startswith("0x"):
        return int(s, 16)
    if s.isdigit():
        return int(s)
    if s in KEYS:
        return KEYS[s]
    raise KeyError(f"unknown key '{name_or_dik}' (known: {', '.join(sorted(KEYS))})")
