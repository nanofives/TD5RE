"""
td5_auto_dump.py - Automate texture dumping by cycling through all tracks/cars.

Sends keyboard inputs to the game window to navigate menus automatically.
Requires: game running with dump mode enabled, all tracks/cars unlocked.

Flow per combination:
  1. From main menu → Race Menu → Car Select → pick car → Track Select → pick track → OK
  2. Wait for race to load (textures get dumped)
  3. ESC → exit race → back to main menu
  4. Repeat

Usage:
  python td5_auto_dump.py                    # cycle all tracks with car 0
  python td5_auto_dump.py --cars 0,5,10,15   # specific car indices
  python td5_auto_dump.py --all-cars         # all 37 cars (slow!)
"""
import argparse
import ctypes
import ctypes.wintypes
import os
import sys
import time

# Win32 constants
WM_KEYDOWN = 0x0100
WM_KEYUP = 0x0101
VK_RETURN = 0x0D
VK_ESCAPE = 0x1B
VK_LEFT = 0x25
VK_RIGHT = 0x27
VK_UP = 0x26
VK_DOWN = 0x28

user32 = ctypes.windll.user32
FindWindowA = user32.FindWindowA
FindWindowA.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
FindWindowA.restype = ctypes.wintypes.HWND
PostMessageA = user32.PostMessageA


def find_game_window():
    """Find the TD5 game window."""
    # Try common window class names
    hwnd = FindWindowA(None, b"Test Drive 5")
    if not hwnd:
        hwnd = FindWindowA(None, b"TD5")
    if not hwnd:
        # Enumerate all windows to find it
        import subprocess
        result = subprocess.run(['powershell', '-c',
            'Get-Process | Where-Object {$_.MainWindowTitle -ne ""} | '
            'Select-Object ProcessName,MainWindowTitle | Format-Table -AutoSize'],
            capture_output=True, text=True)
        print("Running windows:")
        print(result.stdout)
        print("Could not find TD5 window automatically.")
        return None
    return hwnd


def send_key(hwnd, vk, hold=0.05):
    """Send a key press to the game window."""
    PostMessageA(hwnd, WM_KEYDOWN, vk, 0)
    time.sleep(hold)
    PostMessageA(hwnd, WM_KEYUP, vk, 0)
    time.sleep(0.05)


def send_keys(hwnd, vk, count, delay=0.15):
    """Send a key press multiple times."""
    for _ in range(count):
        send_key(hwnd, vk)
        time.sleep(delay)


def wait(seconds, label=""):
    """Wait with status."""
    if label:
        print(f"  {label} ({seconds}s)...", end='', flush=True)
    time.sleep(seconds)
    if label:
        print(" done")


def navigate_main_to_race_menu(hwnd):
    """From main menu to Race Menu (Quick Race)."""
    # Main menu: press Enter to go to race type menu
    send_key(hwnd, VK_RETURN)
    wait(1.5, "entering race menu")


def select_car(hwnd, target_car, current_car=0):
    """Navigate car selection to a specific car index."""
    diff = target_car - current_car
    if diff > 0:
        send_keys(hwnd, VK_RIGHT, diff, 0.3)
    elif diff < 0:
        send_keys(hwnd, VK_LEFT, -diff, 0.3)
    wait(0.3)
    # Press down to highlight OK, then Enter
    send_key(hwnd, VK_DOWN)
    time.sleep(0.2)
    send_key(hwnd, VK_DOWN)
    time.sleep(0.2)
    send_key(hwnd, VK_RETURN)
    wait(1.0, f"selected car {target_car}")


def select_track(hwnd, track_offset):
    """Navigate track selection by offset from current."""
    if track_offset > 0:
        for _ in range(track_offset):
            send_key(hwnd, VK_RIGHT)
            time.sleep(0.3)
    wait(0.3)
    # Press down to OK, then Enter
    send_key(hwnd, VK_DOWN)
    time.sleep(0.2)
    send_key(hwnd, VK_RETURN)
    wait(1.0, "selected track")


def wait_for_race_load(seconds=8):
    """Wait for race to fully load (textures get dumped during this)."""
    wait(seconds, "race loading (textures dumping)")


def exit_race(hwnd):
    """ESC to exit race back to menu."""
    send_key(hwnd, VK_ESCAPE)
    wait(2.0, "exiting race")
    send_key(hwnd, VK_ESCAPE)
    wait(2.0, "back to menu")


def main():
    parser = argparse.ArgumentParser(description="Auto-cycle tracks/cars for texture dump")
    parser.add_argument('--tracks', type=int, default=16,
                        help="Number of tracks to cycle (default: 16)")
    parser.add_argument('--cars', type=str, default="0",
                        help="Comma-separated car indices to test (default: 0)")
    parser.add_argument('--all-cars', action='store_true',
                        help="Test all 37 cars")
    parser.add_argument('--load-time', type=float, default=8.0,
                        help="Seconds to wait for race load (default: 8)")
    parser.add_argument('--dry-run', action='store_true',
                        help="Show plan without sending inputs")
    args = parser.parse_args()

    if args.all_cars:
        car_indices = list(range(37))
    else:
        car_indices = [int(x) for x in args.cars.split(',')]

    total_races = args.tracks * len(car_indices)
    print(f"Plan: {args.tracks} tracks × {len(car_indices)} cars = {total_races} races")
    print(f"Estimated time: {total_races * (args.load_time + 10):.0f} seconds")
    print()

    if args.dry_run:
        for car in car_indices:
            for track in range(args.tracks):
                print(f"  Race: car={car}, track={track}")
        return

    hwnd = find_game_window()
    if not hwnd:
        print("ERROR: Game window not found. Launch the game first.")
        sys.exit(1)

    print(f"Found game window: {hwnd}")
    print("Starting in 3 seconds... switch to the game's main menu!")
    wait(3)

    race_count = 0
    for car_idx, car in enumerate(car_indices):
        for track in range(args.tracks):
            race_count += 1
            print(f"\n[{race_count}/{total_races}] Car {car}, Track {track}")

            # Navigate: Main menu → Race Menu
            navigate_main_to_race_menu(hwnd)

            # Car select (navigate to target car)
            if track == 0:
                # First track for this car: select the car
                select_car(hwnd, car, car_indices[car_idx - 1] if car_idx > 0 else 0)
            else:
                # Same car, just confirm
                send_key(hwnd, VK_DOWN)
                time.sleep(0.2)
                send_key(hwnd, VK_DOWN)
                time.sleep(0.2)
                send_key(hwnd, VK_RETURN)
                wait(0.5)

            # Track select (advance by 1 from previous, or start at 0)
            if track == 0:
                # First track: just press OK
                send_key(hwnd, VK_DOWN)
                time.sleep(0.2)
                send_key(hwnd, VK_RETURN)
                wait(1.0)
            else:
                select_track(hwnd, 1)

            # Wait for textures to load
            wait_for_race_load(args.load_time)

            # Exit race
            exit_race(hwnd)

    print(f"\nDone! {race_count} races completed.")
    print("Run td5_organize_dump.py and td5_build_index.py to process the dumps.")


if __name__ == '__main__':
    main()
