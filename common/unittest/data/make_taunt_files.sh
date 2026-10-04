#!/bin/sh
# Makes the test inputs of common/unittest/taunt.cpp (our own, CC0): a
# 0.3 s silence, then 2.4 s of a 440 Hz + 660 Hz tone, as Ogg Vorbis and
# MP3 (stereo, 44100 Hz) and FLAC (mono, 22050 Hz).  Needs python3, oggenc
# (vorbis-tools), lame and flac.  Run from this directory.
set -eu
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
python3 - "$tmp" <<'PY'
import math, struct, sys, wave
def write(path, rate, channels):
    frames = []
    for i in range(int(rate * 2.7)):
        t = i / rate
        v = 0.0 if t < 0.3 else 0.5 * math.sin(2 * math.pi * 440 * t) + 0.2 * math.sin(2 * math.pi * 660 * t)
        s = int(max(-1.0, min(1.0, v)) * 32767)
        frames.append(struct.pack('<' + 'h' * channels, *([s] * channels)))
    with wave.open(path, 'wb') as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b''.join(frames))
write(sys.argv[1] + '/stereo44.wav', 44100, 2)
write(sys.argv[1] + '/mono22.wav', 22050, 1)
PY
oggenc -Q -q 1 -o taunt-test.ogg "$tmp/stereo44.wav"
lame --quiet --noreplaygain -b 64 "$tmp/stereo44.wav" taunt-test.mp3
flac --silent --force -8 -o taunt-test.flac "$tmp/mono22.wav"
