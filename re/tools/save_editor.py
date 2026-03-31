#!/usr/bin/env python3
"""
Test Drive 5 Save File Editor
Handles Config.td5 (5351 bytes) and CupData.td5 (12966 bytes).

Both files use XOR encryption with a 0x80 flip:
    encrypted[i] = plaintext[i] ^ key[i % key_len] ^ 0x80

Config.td5 key: "Outta Mah Face!! " (18 chars)
CupData.td5 key: "Steve Snake says : No Cheating! " (31 chars)

CRC-32 (standard ISO-HDLC) is embedded in the plaintext:
  - Config.td5: bytes [0..3], placeholder 0x10000000 during computation
  - CupData.td5: bytes [0x0C..0x0F], placeholder 0x10000000 during computation
"""

import argparse
import json
import struct
import sys
import zlib
from pathlib import Path

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

CONFIG_SIZE = 5351
CUPDATA_SIZE = 12966

CONFIG_KEY = b"Outta Mah Face !! "          # 18 bytes (space before !!)
CUPDATA_KEY = b"Steve Snake says : No Cheating! "  # 31 bytes

CRC_PLACEHOLDER = bytes([0x10, 0x00, 0x00, 0x00])

GAME_OPTIONS_LABELS = {
    0: ("circuit_laps", {0: "2 laps", 1: "4 laps", 2: "6 laps", 3: "8 laps"}),
    1: ("checkpoint_timers", {0: "Off", 1: "On"}),
    2: ("traffic", {0: "Off", 1: "On"}),
    3: ("cops", {0: "Off", 1: "On"}),
    4: ("difficulty", {0: "Easy", 1: "Normal", 2: "Hard"}),
    5: ("dynamics", {0: "Arcade", 1: "Simulation"}),
    6: ("3d_collisions", {0: "Off", 1: "On"}),
}

SOUND_MODES = {0: "Stereo", 1: "Surround", 2: "3D"}
SPEED_UNITS = {0: "MPH", 1: "KPH"}
SPLIT_MODES = {0: "Horizontal", 1: "Vertical"}

GAME_TYPES = {
    1: "Championship", 2: "Era Challenge", 3: "Challenge",
    4: "Pitbull", 5: "Masters", 6: "Ultimate",
    7: "Time Trial", 8: "Cop Chase", 9: "Drag Race",
    0xFF: "None",
}

DIFFICULTY_NAMES = {0: "Easy", 1: "Normal", 2: "Hard"}

# ---------------------------------------------------------------------------
# XOR encrypt/decrypt (self-inverse)
# ---------------------------------------------------------------------------

def xor_crypt(data: bytearray, key: bytes) -> bytearray:
    key_len = len(key)
    out = bytearray(len(data))
    for i in range(len(data)):
        out[i] = data[i] ^ key[i % key_len] ^ 0x80
    return out

# ---------------------------------------------------------------------------
# CRC-32 helpers
# ---------------------------------------------------------------------------

def compute_crc32(data: bytes) -> int:
    """Standard CRC-32/ISO-HDLC (same as zlib.crc32)."""
    return zlib.crc32(data) & 0xFFFFFFFF

# ---------------------------------------------------------------------------
# File type detection
# ---------------------------------------------------------------------------

def detect_file_type(data: bytes) -> str:
    if len(data) == CONFIG_SIZE:
        return "config"
    elif len(data) == CUPDATA_SIZE:
        return "cupdata"
    else:
        raise ValueError(
            f"Unknown file size {len(data)} bytes. "
            f"Expected {CONFIG_SIZE} (Config.td5) or {CUPDATA_SIZE} (CupData.td5)."
        )

def get_key(ftype: str) -> bytes:
    return CONFIG_KEY if ftype == "config" else CUPDATA_KEY

# ---------------------------------------------------------------------------
# Decrypt / Encrypt file
# ---------------------------------------------------------------------------

def decrypt_file(raw: bytes) -> tuple[bytearray, str]:
    ftype = detect_file_type(raw)
    key = get_key(ftype)
    plain = xor_crypt(bytearray(raw), key)
    return plain, ftype

def encrypt_file(plain: bytearray, ftype: str) -> bytearray:
    key = get_key(ftype)
    return xor_crypt(plain, key)

# ---------------------------------------------------------------------------
# CRC validation and recomputation
# ---------------------------------------------------------------------------

def validate_crc(plain: bytearray, ftype: str) -> tuple[bool, int, int]:
    """Returns (valid, stored_crc, computed_crc)."""
    buf = bytearray(plain)
    if ftype == "config":
        stored_crc = struct.unpack_from("<I", buf, 0)[0]
        buf[0:4] = CRC_PLACEHOLDER
        computed = compute_crc32(bytes(buf))
    else:  # cupdata
        stored_crc = struct.unpack_from("<I", buf, 0x0C)[0]
        buf[0x0C:0x10] = CRC_PLACEHOLDER
        computed = compute_crc32(bytes(buf))
    return (stored_crc == computed), stored_crc, computed

def recompute_crc(plain: bytearray, ftype: str) -> bytearray:
    """Set CRC placeholder, compute CRC, write it back."""
    buf = bytearray(plain)
    if ftype == "config":
        buf[0:4] = CRC_PLACEHOLDER
        crc = compute_crc32(bytes(buf))
        struct.pack_into("<I", buf, 0, crc)
    else:
        buf[0x0C:0x10] = CRC_PLACEHOLDER
        crc = compute_crc32(bytes(buf))
        struct.pack_into("<I", buf, 0x0C, crc)
    return buf

# ---------------------------------------------------------------------------
# Config.td5 field parsing
# ---------------------------------------------------------------------------

def parse_npc_entry(data: bytes, offset: int) -> dict:
    name_raw = data[offset:offset + 13]
    name = name_raw.split(b"\x00")[0].decode("ascii", errors="replace")
    car_sprite_id = struct.unpack_from("<I", data, offset + 0x10)[0]
    car_index = struct.unpack_from("<I", data, offset + 0x14)[0]
    best_lap_ms = struct.unpack_from("<I", data, offset + 0x18)[0]
    best_race_ms = struct.unpack_from("<I", data, offset + 0x1C)[0]
    return {
        "name": name,
        "car_sprite_id": car_sprite_id,
        "car_index": car_index,
        "best_lap_ms": best_lap_ms,
        "best_lap_time": format_time_ms(best_lap_ms),
        "best_race_ms": best_race_ms,
        "best_race_time": format_time_ms(best_race_ms),
    }

def format_time_ms(ms: int) -> str:
    if ms == 0 or ms == 0xFFFFFFFF:
        return "--:--.---"
    minutes = ms // 60000
    seconds = (ms % 60000) / 1000.0
    return f"{minutes:02d}:{seconds:06.3f}"

def parse_npc_group(data: bytes, offset: int) -> dict:
    header = struct.unpack_from("<I", data, offset)[0]
    entries = []
    for i in range(5):
        entry_off = offset + 0x04 + i * 0x20
        entries.append(parse_npc_entry(data, entry_off))
    return {
        "header": header,
        "type": "special_unlock" if header == 1 else "standard",
        "entries": entries,
    }

def parse_config(plain: bytearray) -> dict:
    d = {}

    # CRC
    d["crc32"] = f"0x{struct.unpack_from('<I', plain, 0)[0]:08X}"

    # Game Options (offset 0x04, 7 dwords)
    game_opts = {}
    for i in range(7):
        val = struct.unpack_from("<I", plain, 0x04 + i * 4)[0]
        name, labels = GAME_OPTIONS_LABELS[i]
        game_opts[name] = {"value": val, "label": labels.get(val, f"unknown({val})")}
    d["game_options"] = game_opts

    # Controllers
    d["p1_controller_device"] = plain[0x20]
    d["p2_controller_device"] = plain[0x21]

    # Force feedback
    d["force_feedback"] = {
        "dword_a": struct.unpack_from("<I", plain, 0x22)[0],
        "dword_b": struct.unpack_from("<I", plain, 0x26)[0],
        "dword_c": struct.unpack_from("<I", plain, 0x2A)[0],
        "dword_d": struct.unpack_from("<I", plain, 0x2E)[0],
    }

    # Controller bindings (18 dwords)
    bindings = []
    for i in range(18):
        bindings.append(struct.unpack_from("<I", plain, 0x32 + i * 4)[0])
    d["controller_bindings"] = bindings

    # Device descriptors (raw hex)
    d["p1_device_descriptor"] = plain[0x7A:0x9A].hex()
    d["p2_device_descriptor"] = plain[0x9A:0xBA].hex()

    # Audio
    d["sound_mode"] = {"value": plain[0xBA], "label": SOUND_MODES.get(plain[0xBA], "unknown")}
    d["sfx_volume"] = plain[0xBB]
    d["music_volume"] = plain[0xBC]

    # Display
    d["display_mode_ordinal"] = struct.unpack_from("<I", plain, 0xBD)[0]
    d["fogging"] = {"value": struct.unpack_from("<I", plain, 0xC1)[0],
                    "label": "On" if struct.unpack_from("<I", plain, 0xC1)[0] else "Off"}
    speed_val = struct.unpack_from("<I", plain, 0xC5)[0]
    d["speed_units"] = {"value": speed_val, "label": SPEED_UNITS.get(speed_val, "unknown")}
    d["camera_damping"] = struct.unpack_from("<I", plain, 0xC9)[0]

    # Custom bindings (raw hex, large blobs)
    d["p1_custom_bindings"] = plain[0xCD:0xCD + 392].hex()
    d["p2_custom_bindings"] = plain[0x255:0x255 + 392].hex()

    # Misc
    d["split_screen_mode"] = {"value": plain[0x3DD],
                              "label": SPLIT_MODES.get(plain[0x3DD], "unknown")}
    d["catchup_assist"] = plain[0x3DE]
    d["unknown_camera_byte"] = plain[0x3DF]
    d["music_track"] = plain[0x3E0]

    # NPC Racer Group Table (26 groups x 164 bytes at offset 0x3E1)
    groups = []
    for g in range(26):
        goff = 0x3E1 + g * 164
        groups.append(parse_npc_group(plain, goff))
    d["npc_racer_groups"] = groups

    # Progression
    d["reserved_byte"] = plain[0x148A]
    d["cup_tier_state"] = plain[0x148B] & 0x07
    d["max_unlocked_car_index"] = plain[0x148C]
    d["all_cars_unlocked"] = plain[0x148D]

    # Track lock table (26 bytes at 0x148E)
    d["track_locks"] = list(plain[0x148E:0x148E + 26])

    # Car lock table (37 bytes at 0x14A8)
    d["car_locks"] = list(plain[0x14A8:0x14A8 + 37])

    # Cheat flags (26 bytes at 0x14CD)
    d["cheat_flags"] = list(plain[0x14CD:0x14CD + 26])

    return d

# ---------------------------------------------------------------------------
# CupData.td5 field parsing
# ---------------------------------------------------------------------------

def parse_cupdata(plain: bytearray) -> dict:
    d = {}

    gt = plain[0]
    d["game_type"] = {"value": gt, "label": GAME_TYPES.get(gt, f"unknown({gt})")}
    d["race_index"] = plain[1]
    d["npc_group_index"] = plain[2]
    d["track_opponent_state"] = plain[3]
    d["race_rule_variant"] = plain[4]
    d["time_trial_flag"] = plain[5]
    d["wanted_flag"] = plain[6]
    d["difficulty"] = {"value": plain[7], "label": DIFFICULTY_NAMES.get(plain[7], "unknown")}
    d["checkpoint_mode"] = plain[8]
    d["traffic_enabled"] = plain[9]
    d["encounter_enabled"] = plain[0x0A]
    d["circuit_lap_count"] = plain[0x0B]
    d["crc32"] = f"0x{struct.unpack_from('<I', plain, 0x0C)[0]:08X}"

    # Race schedule (30 dwords at 0x10)
    schedule = []
    for i in range(30):
        schedule.append(struct.unpack_from("<I", plain, 0x10 + i * 4)[0])
    d["race_schedule"] = schedule

    # Race results (30 dwords at 0x88)
    results = []
    for i in range(30):
        results.append(struct.unpack_from("<I", plain, 0x88 + i * 4)[0])
    d["race_results"] = results

    # Actor state summary (6 slots x 0x388 bytes at offset 0x100)
    # Only extract key fields, not the full 12624-byte blob
    actor_slots = []
    for s in range(6):
        slot_off = 0x100 + s * 0x388
        # Just store as hex -- too large and complex to fully parse here
        actor_slots.append(plain[slot_off:slot_off + 0x388].hex())
    d["actor_slots_hex"] = actor_slots

    # Slot state (6 x 4 bytes at 0x3270)
    slot_states = []
    for s in range(6):
        slot_states.append(struct.unpack_from("<I", plain, 0x3270 + s * 4)[0])
    d["slot_states"] = slot_states

    # Masters state + cup sub-state
    d["masters_opponent_base"] = struct.unpack_from("<I", plain, 0x3288)[0]
    d["cup_substate_a"] = struct.unpack_from("<I", plain, 0x3290)[0]
    d["cup_substate_b"] = struct.unpack_from("<I", plain, 0x3294)[0]
    d["cup_substate_byte_a"] = plain[0x3296]
    d["cup_substate_word_a"] = struct.unpack_from("<H", plain, 0x3297)[0]
    d["cup_substate_c"] = struct.unpack_from("<I", plain, 0x329B)[0]
    d["masters_encounter_flags"] = struct.unpack_from("<I", plain, 0x329F)[0]
    d["cup_substate_word_b"] = struct.unpack_from("<H", plain, 0x32A3)[0]
    d["cup_substate_byte_b"] = plain[0x32A5]

    return d

# ---------------------------------------------------------------------------
# Rebuild from JSON
# ---------------------------------------------------------------------------

def rebuild_config(d: dict, plain: bytearray) -> bytearray:
    """Apply JSON fields back onto a plaintext buffer."""
    buf = bytearray(plain)

    # Game options
    if "game_options" in d:
        for i in range(7):
            name, _ = GAME_OPTIONS_LABELS[i]
            if name in d["game_options"]:
                val = d["game_options"][name]
                v = val["value"] if isinstance(val, dict) else val
                struct.pack_into("<I", buf, 0x04 + i * 4, int(v))

    # Controllers
    if "p1_controller_device" in d:
        buf[0x20] = int(d["p1_controller_device"])
    if "p2_controller_device" in d:
        buf[0x21] = int(d["p2_controller_device"])

    # Force feedback
    if "force_feedback" in d:
        ff = d["force_feedback"]
        for key, off in [("dword_a", 0x22), ("dword_b", 0x26),
                         ("dword_c", 0x2A), ("dword_d", 0x2E)]:
            if key in ff:
                struct.pack_into("<I", buf, off, int(ff[key]))

    # Controller bindings
    if "controller_bindings" in d:
        for i, v in enumerate(d["controller_bindings"][:18]):
            struct.pack_into("<I", buf, 0x32 + i * 4, int(v))

    # Device descriptors
    if "p1_device_descriptor" in d:
        buf[0x7A:0x9A] = bytes.fromhex(d["p1_device_descriptor"])
    if "p2_device_descriptor" in d:
        buf[0x9A:0xBA] = bytes.fromhex(d["p2_device_descriptor"])

    # Audio
    if "sound_mode" in d:
        v = d["sound_mode"]
        buf[0xBA] = int(v["value"] if isinstance(v, dict) else v)
    if "sfx_volume" in d:
        buf[0xBB] = int(d["sfx_volume"])
    if "music_volume" in d:
        buf[0xBC] = int(d["music_volume"])

    # Display
    if "display_mode_ordinal" in d:
        struct.pack_into("<I", buf, 0xBD, int(d["display_mode_ordinal"]))
    if "fogging" in d:
        v = d["fogging"]
        struct.pack_into("<I", buf, 0xC1, int(v["value"] if isinstance(v, dict) else v))
    if "speed_units" in d:
        v = d["speed_units"]
        struct.pack_into("<I", buf, 0xC5, int(v["value"] if isinstance(v, dict) else v))
    if "camera_damping" in d:
        struct.pack_into("<I", buf, 0xC9, int(d["camera_damping"]))

    # Custom bindings
    if "p1_custom_bindings" in d:
        buf[0xCD:0xCD + 392] = bytes.fromhex(d["p1_custom_bindings"])
    if "p2_custom_bindings" in d:
        buf[0x255:0x255 + 392] = bytes.fromhex(d["p2_custom_bindings"])

    # Misc
    if "split_screen_mode" in d:
        v = d["split_screen_mode"]
        buf[0x3DD] = int(v["value"] if isinstance(v, dict) else v)
    if "catchup_assist" in d:
        buf[0x3DE] = int(d["catchup_assist"])
    if "unknown_camera_byte" in d:
        buf[0x3DF] = int(d["unknown_camera_byte"])
    if "music_track" in d:
        buf[0x3E0] = int(d["music_track"])

    # NPC racer groups
    if "npc_racer_groups" in d:
        for g, group in enumerate(d["npc_racer_groups"][:26]):
            goff = 0x3E1 + g * 164
            struct.pack_into("<I", buf, goff, int(group.get("header", 0)))
            for e, entry in enumerate(group.get("entries", [])[:5]):
                eoff = goff + 0x04 + e * 0x20
                # Name (13 bytes, NUL-padded)
                name_bytes = entry.get("name", "").encode("ascii")[:13]
                buf[eoff:eoff + 13] = name_bytes.ljust(13, b"\x00")
                buf[eoff + 13:eoff + 16] = b"\x00\x00\x00"  # padding
                struct.pack_into("<I", buf, eoff + 0x10, int(entry.get("car_sprite_id", 0)))
                struct.pack_into("<I", buf, eoff + 0x14, int(entry.get("car_index", 0)))
                struct.pack_into("<I", buf, eoff + 0x18, int(entry.get("best_lap_ms", 0)))
                struct.pack_into("<I", buf, eoff + 0x1C, int(entry.get("best_race_ms", 0)))

    # Progression
    if "reserved_byte" in d:
        buf[0x148A] = int(d["reserved_byte"])
    if "cup_tier_state" in d:
        buf[0x148B] = int(d["cup_tier_state"]) & 0x07
    if "max_unlocked_car_index" in d:
        buf[0x148C] = int(d["max_unlocked_car_index"])
    if "all_cars_unlocked" in d:
        buf[0x148D] = int(d["all_cars_unlocked"])
    if "track_locks" in d:
        for i, v in enumerate(d["track_locks"][:26]):
            buf[0x148E + i] = int(v)
    if "car_locks" in d:
        for i, v in enumerate(d["car_locks"][:37]):
            buf[0x14A8 + i] = int(v)
    if "cheat_flags" in d:
        for i, v in enumerate(d["cheat_flags"][:26]):
            buf[0x14CD + i] = int(v)

    return buf

def rebuild_cupdata(d: dict, plain: bytearray) -> bytearray:
    """Apply JSON fields back onto a CupData plaintext buffer."""
    buf = bytearray(plain)

    if "game_type" in d:
        v = d["game_type"]
        buf[0] = int(v["value"] if isinstance(v, dict) else v)
    if "race_index" in d:
        buf[1] = int(d["race_index"])
    if "npc_group_index" in d:
        buf[2] = int(d["npc_group_index"])
    if "track_opponent_state" in d:
        buf[3] = int(d["track_opponent_state"])
    if "race_rule_variant" in d:
        buf[4] = int(d["race_rule_variant"])
    if "time_trial_flag" in d:
        buf[5] = int(d["time_trial_flag"])
    if "wanted_flag" in d:
        buf[6] = int(d["wanted_flag"])
    if "difficulty" in d:
        v = d["difficulty"]
        buf[7] = int(v["value"] if isinstance(v, dict) else v)
    if "checkpoint_mode" in d:
        buf[8] = int(d["checkpoint_mode"])
    if "traffic_enabled" in d:
        buf[9] = int(d["traffic_enabled"])
    if "encounter_enabled" in d:
        buf[0x0A] = int(d["encounter_enabled"])
    if "circuit_lap_count" in d:
        buf[0x0B] = int(d["circuit_lap_count"])

    if "race_schedule" in d:
        for i, v in enumerate(d["race_schedule"][:30]):
            struct.pack_into("<I", buf, 0x10 + i * 4, int(v))
    if "race_results" in d:
        for i, v in enumerate(d["race_results"][:30]):
            struct.pack_into("<I", buf, 0x88 + i * 4, int(v))

    if "actor_slots_hex" in d:
        for s, hexstr in enumerate(d["actor_slots_hex"][:6]):
            slot_off = 0x100 + s * 0x388
            slot_data = bytes.fromhex(hexstr)
            buf[slot_off:slot_off + len(slot_data)] = slot_data

    if "slot_states" in d:
        for s, v in enumerate(d["slot_states"][:6]):
            struct.pack_into("<I", buf, 0x3270 + s * 4, int(v))

    if "masters_opponent_base" in d:
        struct.pack_into("<I", buf, 0x3288, int(d["masters_opponent_base"]))
    if "cup_substate_a" in d:
        struct.pack_into("<I", buf, 0x3290, int(d["cup_substate_a"]))
    if "cup_substate_b" in d:
        struct.pack_into("<I", buf, 0x3294, int(d["cup_substate_b"]))
    if "cup_substate_byte_a" in d:
        buf[0x3296] = int(d["cup_substate_byte_a"])
    if "cup_substate_word_a" in d:
        struct.pack_into("<H", buf, 0x3297, int(d["cup_substate_word_a"]))
    if "cup_substate_c" in d:
        struct.pack_into("<I", buf, 0x329B, int(d["cup_substate_c"]))
    if "masters_encounter_flags" in d:
        struct.pack_into("<I", buf, 0x329F, int(d["masters_encounter_flags"]))
    if "cup_substate_word_b" in d:
        struct.pack_into("<H", buf, 0x32A3, int(d["cup_substate_word_b"]))
    if "cup_substate_byte_b" in d:
        buf[0x32A5] = int(d["cup_substate_byte_b"])

    return buf

# ---------------------------------------------------------------------------
# "set" command: edit individual fields by dot-path
# ---------------------------------------------------------------------------

# Flat field map for Config.td5 quick edits (field_name -> (offset, size, type))
# type: "u8", "u32", "u8_array"
CONFIG_QUICK_FIELDS = {
    # Game options
    "circuit_laps":          (0x04, 4, "u32"),
    "checkpoint_timers":     (0x08, 4, "u32"),
    "traffic":               (0x0C, 4, "u32"),
    "cops":                  (0x10, 4, "u32"),
    "difficulty":            (0x14, 4, "u32"),
    "dynamics":              (0x18, 4, "u32"),
    "3d_collisions":         (0x1C, 4, "u32"),
    # Controllers
    "p1_controller_device":  (0x20, 1, "u8"),
    "p2_controller_device":  (0x21, 1, "u8"),
    # Audio
    "sound_mode":            (0xBA, 1, "u8"),
    "sfx_volume":            (0xBB, 1, "u8"),
    "music_volume":          (0xBC, 1, "u8"),
    # Display
    "display_mode_ordinal":  (0xBD, 4, "u32"),
    "fogging":               (0xC1, 4, "u32"),
    "speed_units":           (0xC5, 4, "u32"),
    "camera_damping":        (0xC9, 4, "u32"),
    # Misc
    "split_screen_mode":     (0x3DD, 1, "u8"),
    "catchup_assist":        (0x3DE, 1, "u8"),
    "music_track":           (0x3E0, 1, "u8"),
    # Progression
    "cup_tier_state":        (0x148B, 1, "u8"),
    "max_unlocked_car_index":(0x148C, 1, "u8"),
    "all_cars_unlocked":     (0x148D, 1, "u8"),
}

# Virtual fields that perform batch operations
CONFIG_VIRTUAL_FIELDS = {
    "unlock_all_cars",
    "unlock_all_tracks",
    "unlock_all_cups",
    "unlock_everything",
}

CUPDATA_QUICK_FIELDS = {
    "game_type":             (0x00, 1, "u8"),
    "race_index":            (0x01, 1, "u8"),
    "npc_group_index":       (0x02, 1, "u8"),
    "race_rule_variant":     (0x04, 1, "u8"),
    "time_trial_flag":       (0x05, 1, "u8"),
    "wanted_flag":           (0x06, 1, "u8"),
    "difficulty":            (0x07, 1, "u8"),
    "checkpoint_mode":       (0x08, 1, "u8"),
    "traffic_enabled":       (0x09, 1, "u8"),
    "encounter_enabled":     (0x0A, 1, "u8"),
    "circuit_lap_count":     (0x0B, 1, "u8"),
}

def apply_virtual_field(buf: bytearray, ftype: str, field: str, value: int):
    """Apply a virtual field that modifies multiple bytes."""
    if ftype != "config":
        raise ValueError(f"Virtual field '{field}' only applies to Config.td5")

    if field == "unlock_all_cars":
        if value:
            buf[0x148D] = 1  # all_cars_unlocked flag
            buf[0x148C] = 0x24  # max_unlocked_car_index = 36
            for i in range(37):
                buf[0x14A8 + i] = 0  # clear all car locks
            print("  All 37 cars unlocked.")
        else:
            buf[0x148D] = 0
            print("  All-cars flag cleared (individual locks unchanged).")

    elif field == "unlock_all_tracks":
        if value:
            for i in range(26):
                buf[0x148E + i] = 0  # 0=unlocked in memory (!=0 means locked)
            print("  All 26 tracks unlocked.")
        else:
            # Reset to defaults: first 8 unlocked (0), rest locked (1)
            for i in range(8):
                buf[0x148E + i] = 0
            for i in range(8, 26):
                buf[0x148E + i] = 1
            print("  Track locks reset to defaults.")

    elif field == "unlock_all_cups":
        if value:
            buf[0x148B] = 0x07  # all cup tiers available
            print("  All cup tiers unlocked (tier state = 7).")
        else:
            buf[0x148B] = 0
            print("  Cup tier reset to 0 (Championship/Era only).")

    elif field == "unlock_everything":
        if value:
            apply_virtual_field(buf, ftype, "unlock_all_cars", 1)
            apply_virtual_field(buf, ftype, "unlock_all_tracks", 1)
            apply_virtual_field(buf, ftype, "unlock_all_cups", 1)
        else:
            apply_virtual_field(buf, ftype, "unlock_all_cars", 0)
            apply_virtual_field(buf, ftype, "unlock_all_tracks", 0)
            apply_virtual_field(buf, ftype, "unlock_all_cups", 0)

# ---------------------------------------------------------------------------
# Display helpers
# ---------------------------------------------------------------------------

def display_config(d: dict):
    print("=" * 60)
    print("  Config.td5")
    print("=" * 60)
    print(f"  CRC-32: {d['crc32']}")
    print()

    print("  --- Game Options ---")
    for name, opt in d["game_options"].items():
        print(f"    {name:20s} = {opt['value']} ({opt['label']})")
    print()

    print("  --- Audio ---")
    print(f"    sound_mode           = {d['sound_mode']['value']} ({d['sound_mode']['label']})")
    print(f"    sfx_volume           = {d['sfx_volume']}")
    print(f"    music_volume         = {d['music_volume']}")
    print()

    print("  --- Display ---")
    print(f"    display_mode_ordinal = {d['display_mode_ordinal']}")
    print(f"    fogging              = {d['fogging']['value']} ({d['fogging']['label']})")
    print(f"    speed_units          = {d['speed_units']['value']} ({d['speed_units']['label']})")
    print(f"    camera_damping       = {d['camera_damping']}")
    print()

    print("  --- Controllers ---")
    print(f"    p1_device            = {d['p1_controller_device']}")
    print(f"    p2_device            = {d['p2_controller_device']}")
    print(f"    split_screen_mode    = {d['split_screen_mode']['value']} ({d['split_screen_mode']['label']})")
    print(f"    catchup_assist       = {d['catchup_assist']}")
    print(f"    music_track          = {d['music_track']}")
    print()

    print("  --- Progression ---")
    print(f"    cup_tier_state       = {d['cup_tier_state']}")
    print(f"    max_unlocked_car     = {d['max_unlocked_car_index']}")
    print(f"    all_cars_unlocked    = {d['all_cars_unlocked']}")
    print()

    # Track locks
    locked_tracks = [i for i, v in enumerate(d["track_locks"]) if v != 0]
    unlocked_tracks = [i for i, v in enumerate(d["track_locks"]) if v == 0]
    print(f"    tracks locked        = {locked_tracks}")
    print(f"    tracks unlocked      = {unlocked_tracks}")

    # Car locks
    locked_cars = [i for i, v in enumerate(d["car_locks"]) if v == 1]
    unlocked_cars = [i for i, v in enumerate(d["car_locks"]) if v == 0]
    print(f"    cars locked          = {locked_cars}")
    print(f"    cars unlocked        = {unlocked_cars}")
    print()

    # Cheat flags (only show if any are non-zero)
    active_cheats = [(i, v) for i, v in enumerate(d["cheat_flags"]) if v != 0]
    if active_cheats:
        print("  --- Active Cheats ---")
        for idx, val in active_cheats:
            print(f"    group {idx:2d}: flags = 0x{val:02X}")
        print()

    # NPC groups: just show non-empty high scores
    print("  --- High Scores (non-empty entries) ---")
    for g, group in enumerate(d["npc_racer_groups"]):
        has_scores = any(
            e["best_lap_ms"] != 0 and e["best_lap_ms"] != 0xFFFFFFFF
            for e in group["entries"]
        )
        if has_scores:
            print(f"    Group {g:2d} ({group['type']}):")
            for e in group["entries"]:
                if e["best_lap_ms"] != 0 and e["best_lap_ms"] != 0xFFFFFFFF:
                    print(f"      {e['name']:13s}  car={e['car_index']:2d}  "
                          f"lap={e['best_lap_time']}  race={e['best_race_time']}")

def display_cupdata(d: dict):
    print("=" * 60)
    print("  CupData.td5")
    print("=" * 60)
    print(f"  CRC-32: {d['crc32']}")
    print()

    print("  --- Cup State ---")
    print(f"    game_type            = {d['game_type']['value']} ({d['game_type']['label']})")
    print(f"    race_index           = {d['race_index']}")
    print(f"    npc_group_index      = {d['npc_group_index']}")
    print(f"    race_rule_variant    = {d['race_rule_variant']}")
    print(f"    difficulty           = {d['difficulty']['value']} ({d['difficulty']['label']})")
    print(f"    circuit_lap_count    = {d['circuit_lap_count']}")
    print()

    print("  --- Flags ---")
    print(f"    time_trial           = {d['time_trial_flag']}")
    print(f"    wanted/cops          = {d['wanted_flag']}")
    print(f"    checkpoint_mode      = {d['checkpoint_mode']}")
    print(f"    traffic              = {d['traffic_enabled']}")
    print(f"    encounters           = {d['encounter_enabled']}")
    print()

    print("  --- Race Schedule (30 dwords) ---")
    sched = d["race_schedule"]
    for i in range(0, 30, 6):
        vals = " ".join(f"{v:8d}" for v in sched[i:i+6])
        print(f"    [{i:2d}..{min(i+5,29):2d}] {vals}")
    print()

    print("  --- Race Results (30 dwords) ---")
    res = d["race_results"]
    for i in range(0, 30, 6):
        vals = " ".join(f"{v:8d}" for v in res[i:i+6])
        print(f"    [{i:2d}..{min(i+5,29):2d}] {vals}")
    print()

    print("  --- Slot States ---")
    for i, s in enumerate(d["slot_states"]):
        print(f"    slot {i}: 0x{s:08X}")

# ---------------------------------------------------------------------------
# CLI commands
# ---------------------------------------------------------------------------

def cmd_decrypt(args):
    raw = Path(args.file).read_bytes()
    plain, ftype = decrypt_file(raw)
    valid, stored, computed = validate_crc(plain, ftype)
    print(f"File type: {ftype}")
    print(f"CRC-32:   stored=0x{stored:08X}  computed=0x{computed:08X}  {'OK' if valid else 'MISMATCH'}")
    out = args.out or (Path(args.file).stem + "_decrypted.bin")
    Path(out).write_bytes(plain)
    print(f"Decrypted {len(plain)} bytes -> {out}")

def cmd_encrypt(args):
    plain = bytearray(Path(args.file).read_bytes())
    ftype = detect_file_type(plain)
    plain = recompute_crc(plain, ftype)
    enc = encrypt_file(plain, ftype)
    out = args.out or ("Config.td5" if ftype == "config" else "CupData.td5")
    Path(out).write_bytes(enc)
    print(f"Encrypted {len(enc)} bytes -> {out}")

def cmd_info(args):
    raw = Path(args.file).read_bytes()
    plain, ftype = decrypt_file(raw)
    valid, stored, computed = validate_crc(plain, ftype)
    if not valid:
        print(f"WARNING: CRC mismatch (stored=0x{stored:08X}, computed=0x{computed:08X})")
        print()
    if ftype == "config":
        display_config(parse_config(plain))
    else:
        display_cupdata(parse_cupdata(plain))

def cmd_export(args):
    raw = Path(args.file).read_bytes()
    plain, ftype = decrypt_file(raw)
    valid, stored, computed = validate_crc(plain, ftype)
    if not valid:
        print(f"WARNING: CRC mismatch (stored=0x{stored:08X}, computed=0x{computed:08X})")

    if ftype == "config":
        d = parse_config(plain)
    else:
        d = parse_cupdata(plain)

    d["_file_type"] = ftype
    d["_file_size"] = len(plain)

    out = args.json or (Path(args.file).stem + ".json")
    Path(out).write_text(json.dumps(d, indent=2), encoding="utf-8")
    print(f"Exported {ftype} -> {out}")

def cmd_import(args):
    d = json.loads(Path(args.json).read_text(encoding="utf-8"))
    ftype = d.get("_file_type")
    if not ftype:
        raise ValueError("JSON missing '_file_type' field. Was this exported by this tool?")

    # If a source .td5 exists, use it as the base; otherwise start from zeros
    if args.base:
        raw = Path(args.base).read_bytes()
        plain, _ = decrypt_file(raw)
    else:
        size = CONFIG_SIZE if ftype == "config" else CUPDATA_SIZE
        plain = bytearray(size)

    if ftype == "config":
        buf = rebuild_config(d, plain)
    else:
        buf = rebuild_cupdata(d, plain)

    buf = recompute_crc(buf, ftype)
    enc = encrypt_file(buf, ftype)
    out = args.out or ("Config.td5" if ftype == "config" else "CupData.td5")
    Path(out).write_bytes(enc)
    print(f"Imported {ftype} from JSON, wrote {len(enc)} bytes -> {out}")

def cmd_set(args):
    raw = Path(args.file).read_bytes()
    plain, ftype = decrypt_file(raw)
    valid, stored, computed = validate_crc(plain, ftype)
    if not valid:
        print(f"WARNING: CRC mismatch on input (stored=0x{stored:08X}, computed=0x{computed:08X})")

    field = args.field
    value = int(args.value, 0)  # supports hex (0x...) and decimal

    # Check virtual fields first
    if field in CONFIG_VIRTUAL_FIELDS:
        apply_virtual_field(plain, ftype, field, value)
    else:
        # Lookup in quick-field tables
        table = CONFIG_QUICK_FIELDS if ftype == "config" else CUPDATA_QUICK_FIELDS
        if field not in table:
            all_fields = sorted(list(table.keys()) | (CONFIG_VIRTUAL_FIELDS if ftype == "config" else set()))
            print(f"Unknown field '{field}'. Available fields:")
            for f in all_fields:
                print(f"  {f}")
            sys.exit(1)

        offset, size, dtype = table[field]
        old_val = struct.unpack_from("<I" if dtype == "u32" else "B", plain, offset)[0]
        if dtype == "u32":
            struct.pack_into("<I", plain, offset, value)
        else:
            plain[offset] = value & 0xFF
        print(f"  {field}: {old_val} -> {value}")

    # Recompute CRC and encrypt
    plain = recompute_crc(plain, ftype)
    enc = encrypt_file(plain, ftype)
    out = args.out or args.file
    Path(out).write_bytes(enc)
    print(f"Wrote {len(enc)} bytes -> {out}")

def cmd_fields(args):
    """List all editable fields for a file type."""
    ftype = args.type
    if ftype == "config":
        print("Config.td5 fields:")
        print()
        print("  --- Direct Fields ---")
        for name, (off, size, dtype) in sorted(CONFIG_QUICK_FIELDS.items(), key=lambda x: x[1][0]):
            print(f"    {name:25s}  offset=0x{off:04X}  size={size}  type={dtype}")
        print()
        print("  --- Virtual Fields (batch operations) ---")
        for name in sorted(CONFIG_VIRTUAL_FIELDS):
            print(f"    {name}")
    else:
        print("CupData.td5 fields:")
        print()
        for name, (off, size, dtype) in sorted(CUPDATA_QUICK_FIELDS.items(), key=lambda x: x[1][0]):
            print(f"    {name:25s}  offset=0x{off:04X}  size={size}  type={dtype}")

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Test Drive 5 save file editor (Config.td5 / CupData.td5)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""\
examples:
  %(prog)s info Config.td5                          Show all fields
  %(prog)s decrypt Config.td5 --out config.bin      Decrypt to raw binary
  %(prog)s encrypt config.bin --out Config.td5      Re-encrypt from raw binary
  %(prog)s export Config.td5 --json config.json     Export to JSON
  %(prog)s import config.json --out Config.td5      Import from JSON
  %(prog)s set Config.td5 unlock_all_cars 1         Unlock all cars
  %(prog)s set Config.td5 sfx_volume 80             Set SFX volume to 80
  %(prog)s set Config.td5 difficulty 2              Set difficulty to Hard
  %(prog)s fields config                            List all editable fields
""")
    sub = parser.add_subparsers(dest="command", required=True)

    # decrypt
    p = sub.add_parser("decrypt", help="Decrypt a .td5 file to raw binary")
    p.add_argument("file", help="Input .td5 file")
    p.add_argument("--out", help="Output file path (default: <stem>_decrypted.bin)")

    # encrypt
    p = sub.add_parser("encrypt", help="Encrypt a raw binary back to .td5")
    p.add_argument("file", help="Input raw binary file (must be 5351 or 12966 bytes)")
    p.add_argument("--out", help="Output file path")

    # info
    p = sub.add_parser("info", help="Display all known fields")
    p.add_argument("file", help="Input .td5 file")

    # export
    p = sub.add_parser("export", help="Export to JSON")
    p.add_argument("file", help="Input .td5 file")
    p.add_argument("--json", help="Output JSON path (default: <stem>.json)")

    # import
    p = sub.add_parser("import", aliases=["imp"], help="Import from JSON, re-encrypt")
    p.add_argument("json", help="Input JSON file (exported by this tool)")
    p.add_argument("--out", help="Output .td5 file path")
    p.add_argument("--base", help="Base .td5 file to patch (optional; if omitted, builds from zeros + JSON)")

    # set
    p = sub.add_parser("set", help="Edit a single field by name")
    p.add_argument("file", help="Input .td5 file (modified in-place unless --out)")
    p.add_argument("field", help="Field name (use 'fields' command to list)")
    p.add_argument("value", help="New value (decimal or 0x hex)")
    p.add_argument("--out", help="Output file path (default: overwrite input)")

    # fields
    p = sub.add_parser("fields", help="List all editable field names")
    p.add_argument("type", choices=["config", "cupdata"], help="File type")

    args = parser.parse_args()

    dispatch = {
        "decrypt": cmd_decrypt,
        "encrypt": cmd_encrypt,
        "info": cmd_info,
        "export": cmd_export,
        "import": cmd_import,
        "imp": cmd_import,
        "set": cmd_set,
        "fields": cmd_fields,
    }
    dispatch[args.command](args)

if __name__ == "__main__":
    main()
