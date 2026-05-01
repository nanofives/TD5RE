#!/usr/bin/env python3
"""
Generate an unlocked Config.td5 for Test Drive 5.

Config.td5 layout (5351 bytes, packed):
  0x0000  uint32  crc32
  0x0004  int32[7]  game_options (28 bytes)
  0x0020  uint8   p1_device_index
  0x0021  uint8   p2_device_index
  0x0022  int32   ff_config_a
  0x0026  int32   ff_config_b
  0x002A  int32   ff_config_c
  0x002E  int32   ff_config_d
  0x0032  uint32[18]  controller_bindings (72 bytes)
  0x007A  uint32[8]   p1_device_desc (32 bytes)
  0x009A  uint32[8]   p2_device_desc (32 bytes)
  0x00BA  uint8   sound_mode
  0x00BB  uint8   sfx_volume
  0x00BC  uint8   music_volume
  0x00BD  int32   display_mode_ordinal
  0x00C1  int32   fog_enabled
  0x00C5  int32   speed_units
  0x00C9  int32   camera_damping
  0x00CD  uint32[98]  p1_custom_bindings (392 bytes)
  0x0255  uint32[98]  p2_custom_bindings (392 bytes)
  0x03DD  uint8   split_screen_mode
  0x03DE  uint8   catchup_assist
  0x03DF  uint8   camera_byte_a
  0x03E0  uint8   camera_byte_b
  0x03E1  uint8[4264] npc_group_table (26 groups x 164 bytes)
  0x1489  uint8   reserved_zero (always 0)
  0x148A  uint8   music_track
  0x148B  uint8   cup_tier_state (masked to 3 bits)
  0x148C  uint8   max_unlocked_car
  0x148D  uint8   all_cars_unlocked
  0x148E  uint8[26]  track_locks (1=unlocked, 0=locked)
  0x14A8  uint8[37]  car_locks (0=unlocked, nonzero=locked)
  0x14CD  uint8[26]  cheat_flags
  Total: 0x14E7 = 5351 bytes

XOR encryption: each byte ^= key[i % len(key)] ^ 0x80
Key: "Outta Mah Face !! "
CRC-32: standard ISO 3309, poly 0xEDB88320, init 0xFFFFFFFF, final XOR 0xFFFFFFFF
  - Set crc32 field to 0x00000010, compute CRC over full buffer, store result.
"""

import struct
import sys

CONFIG_SIZE = 5351
XOR_KEY = b"Outta Mah Face !! "
CRC_PLACEHOLDER = 0x00000010
NUM_TRACKS = 26
NUM_CARS = 37
NUM_CHEATS = 26
NPC_TABLE_SIZE = 26 * 164  # 4264

def crc32_td5(data: bytes) -> int:
    """Standard CRC-32 matching TD5's implementation."""
    table = []
    for i in range(256):
        c = i
        for _ in range(8):
            if c & 1:
                c = 0xEDB88320 ^ (c >> 1)
            else:
                c >>= 1
        table.append(c)

    crc = 0xFFFFFFFF
    for b in data:
        crc = table[(crc ^ b) & 0xFF] ^ (crc >> 8)
    return crc ^ 0xFFFFFFFF

def xor_encrypt(data: bytearray, key: bytes) -> bytearray:
    klen = len(key)
    for i in range(len(data)):
        data[i] = data[i] ^ key[i % klen] ^ 0x80
    return data

def generate_unlocked_config() -> bytes:
    buf = bytearray(CONFIG_SIZE)

    # Game options: 7 int32s at offset 0x04
    # Default sensible values (0 = default for most)
    game_opts = [0] * 7
    struct.pack_into('<7i', buf, 0x04, *game_opts)

    # Device indices
    buf[0x20] = 0  # p1_device_index
    buf[0x21] = 0  # p2_device_index

    # Force feedback config (4 int32s)
    struct.pack_into('<4i', buf, 0x22, 0, 0, 0, 0)

    # Controller bindings: leave as zeros (defaults)
    # Device descriptors: leave as zeros

    # Audio settings
    buf[0xBA] = 0    # sound_mode (0=stereo)
    buf[0xBB] = 80   # sfx_volume
    buf[0xBC] = 80   # music_volume

    # Display settings
    struct.pack_into('<i', buf, 0xBD, 0)    # display_mode_ordinal
    struct.pack_into('<i', buf, 0xC1, 1)    # fog_enabled
    struct.pack_into('<i', buf, 0xC5, 0)    # speed_units (0=mph)
    struct.pack_into('<i', buf, 0xC9, 0)    # camera_damping

    # Custom bindings: leave as zeros (defaults)

    # Misc settings
    buf[0x3DD] = 0  # split_screen_mode
    buf[0x3DE] = 1  # catchup_assist (enabled)
    buf[0x3DF] = 0  # camera_byte_a
    buf[0x3E0] = 0  # camera_byte_b

    # NPC group table: leave as zeros (will use defaults)

    # reserved_zero
    buf[0x1489] = 0

    # music_track
    buf[0x148A] = 0

    # Cup tier state: max progression (all tiers unlocked)
    buf[0x148B] = 0x07  # cup_tier_state (3 bits, all set = all tiers)

    # Max unlocked car: all 37 cars
    buf[0x148C] = NUM_CARS  # max_unlocked_car = 37

    # All cars unlocked flag
    buf[0x148D] = 1  # all_cars_unlocked = true

    # Track locks: 1 = unlocked for all 26 tracks
    for i in range(NUM_TRACKS):
        buf[0x148E + i] = 1

    # Car locks: 0 = unlocked for all 37 cars
    for i in range(NUM_CARS):
        buf[0x14A8 + i] = 0

    # Cheat flags: enable all cheats
    for i in range(NUM_CHEATS):
        buf[0x14CD + i] = 1

    # --- CRC-32 ---
    # Step 1: Set CRC placeholder
    struct.pack_into('<I', buf, 0x00, CRC_PLACEHOLDER)

    # Step 2: Compute CRC over entire buffer
    crc = crc32_td5(bytes(buf))

    # Step 3: Store computed CRC
    struct.pack_into('<I', buf, 0x00, crc)

    # --- XOR encrypt ---
    buf = xor_encrypt(buf, XOR_KEY)

    return bytes(buf)

if __name__ == '__main__':
    out_path = sys.argv[1] if len(sys.argv) > 1 else "original/Config.td5"
    data = generate_unlocked_config()
    assert len(data) == CONFIG_SIZE, f"Size mismatch: {len(data)} != {CONFIG_SIZE}"

    with open(out_path, 'wb') as f:
        f.write(data)

    print(f"Written {len(data)} bytes to {out_path}")
    print(f"  All {NUM_TRACKS} tracks unlocked")
    print(f"  All {NUM_CARS} cars unlocked")
    print(f"  All {NUM_CHEATS} cheats enabled")
    print(f"  Cup tier state: 0x07 (all tiers)")
