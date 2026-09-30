HORNS / MEMES -- drop-in folder
===============================

Any .wav file you drop in this folder shows up in the MEMES tab of the horn
picker, inside a local player's profile panel in multiplayer setup.

Format
------
Mono, 22050 Hz, 16-bit PCM. That is the format both Test Drive 5 and Test
Drive 6 use for horns, so a file in that format drops straight into the same
sound slot with no conversion at load time. Other formats may fail to load or
play back at the wrong pitch.

Keep them short. A horn is a one-shot: the sound plays from the start every
time the horn is pressed and is cut when it finishes. A long file will be
audible for as long as it runs.

Naming
------
The filename becomes the on-screen label: underscores and dashes turn into
spaces and the text is uppercased, so `air_horn.wav` lists as `AIR HORN`.

Filenames longer than 26 characters are skipped, because the saved choice is
stored under an id built from the filename and truncating it could make two
files collide. Files that are not valid RIFF/WAVE containers are skipped too,
and both cases are reported in log/engine.log.

Up to 64 files are listed. The list is sorted by filename, so a given horn
keeps the same position every run.

Notes
-----
The folder is scanned once at startup. Add or remove files with the game
closed, or restart it to pick up changes.

Only use audio you have the right to use.
