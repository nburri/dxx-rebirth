# Bundled sounds

The release packages put these into the game's `sounds` folder.

## Capture the flag cues

Short cues every machine plays when the host announces a flag event in
capture the flag (standard and Classic), told from the listener's side:
"own" is the flag of the listener's team, "enemy" the other one.  Where
the original game has a voice for the event ("Blue team has the flag",
"You have the flag", "Red team has scored", "You have scored"), it plays
right after the cue.  `common/main/ctf_cues.h` maps the events to the
cues; `similar/main/net_modes.cpp` loads them into sound ids the game's
data leaves free (`SOUND_CTF_*` in `common/main/sounds.h`).

| File | Event | Sound |
|---|---|---|
| `ctf-own-flag-taken.wav` | the enemy takes our flag | urgent two-tone alarm |
| `ctf-enemy-flag-taken.wav` | we take their flag | quick rising major call |
| `ctf-own-flag-dropped.wav` | their carrier loses our flag | two wooden plucks bouncing up |
| `ctf-enemy-flag-dropped.wav` | our carrier loses their flag | two duller plucks falling |
| `ctf-own-flag-returned.wav` | our flag goes home | falling major bell chime, metallic tail |
| `ctf-enemy-flag-returned.wav` | their flag goes home | rising minor chime, lower and darker |
| `ctf-we-scored.wav` | we capture | brass fanfare, major chord |
| `ctf-they-scored.wav` | they capture | low brass phrase falling in minor |

A flag goes home when its own team touches it (Classic option), when it
lay away from home for 30 s (Classic, when nothing else would return
it), or at once as a dying carrier's ("dropped flag returns home").

All are mono, 22050 Hz, 16-bit PCM, 0.6 to 1.5 s long (the game converts
them to its 8-bit sounds, at 11025 Hz with `-sound11k`).  Their loudness
is that of the game's multiplayer voices in DESCENT2.S22 (the 95th
percentile of the A-weighted RMS over 50 ms windows: 0.355; the voices
measure 0.32 to 0.39), the peaks softly limited below full scale.

They are this fork's own work, synthesised by `src/ctf_cues.py` (numpy;
no recorded samples), and public domain (CC0 1.0 Universal).
`python3 src/ctf_cues.py --plot DIR` writes them again with an image of
their waveforms and spectrograms; `--measure DESCENT2.S22` prints the
game's levels they are matched to.

If a file is missing, the event plays the HUD message beep (or only the
voice).  A mission whose own HAM file uses one of the cue's sound ids
keeps its sound; that cue is not loaded.
