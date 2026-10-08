#!/usr/bin/env python3
# Synthesises the capture-the-flag cues of data/sounds (README.md there).
# Public domain (CC0 1.0): written for this fork, no samples of anyone.
#
#	python3 data/sounds/src/ctf_cues.py [--measure DESCENT2.S22] [--plot DIR]
#
# Writes data/sounds/ctf-*.wav: mono, 22050 Hz, 16-bit PCM.  Every cue is
# brought to the loudness of the game's multiplayer voices ("Blue team
# has the flag", "You have scored"), which they precede or replace: the
# 95th percentile of the A-weighted RMS over 50 ms windows, 0.33 to 0.38
# for those voices in DESCENT2.S22 (0.32 the median of all its samples),
# with the peaks softly limited below full scale.  --measure prints
# those figures for a DESCENT2.S22 (read only).
#
# The design: the event by the timbre, the side by the mode and pitch.
#	taken     synthetic, buzzy: an alarm (our flag) or a rising call
#	dropped   wooden plucks: a bounce up (theirs lost ours) or down
#	returned  bells with a metallic tail: falling major (ours home),
#	          rising minor and lower (theirs home)
#	capture   brass: a major fanfare (we score) or a falling minor
#	          phrase (they score)
# ours = the listener's team's flag; we/they = the listener's team.

import argparse
import os
import struct
import sys
import wave

import numpy as np

RATE = 22050
TARGET_LOUDNESS = 0.355	# p95 of the A-weighted 50 ms window RMS
PEAK = 0.97
KNEE = 0.75

OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')


def t_axis(seconds):
	return np.arange(int(seconds * RATE)) / RATE


def note_hz(name):
	"""'A4' -> 440, 'C#5', 'Eb3'."""
	names = {'C': -9, 'D': -7, 'E': -5, 'F': -4, 'G': -2, 'A': 0, 'B': 2}
	n = names[name[0]]
	rest = name[1:]
	if rest[0] == '#':
		n += 1
		rest = rest[1:]
	elif rest[0] == 'b':
		n -= 1
		rest = rest[1:]
	octave = int(rest)
	return 440.0 * 2 ** ((n + 12 * (octave - 4)) / 12)


def adsr(n, attack, decay, sustain, release):
	"""An envelope of n samples (times in seconds)."""
	a = max(1, int(attack * RATE))
	d = max(1, int(decay * RATE))
	r = max(1, int(release * RATE))
	s = max(0, n - a - d - r)
	env = np.concatenate([
		np.linspace(0, 1, a, endpoint=False),
		np.linspace(1, sustain, d, endpoint=False),
		np.full(s, sustain),
		np.linspace(sustain, 0, r),
	])
	return env[:n] if len(env) >= n else np.pad(env, (0, n - len(env)))


def band_limited(freq, t, harmonics, phase=0.0):
	"""Sum of harmonics [(k, amplitude)], those above Nyquist left out;
	freq may be an array (a glide)."""
	freq = np.broadcast_to(np.asarray(freq, dtype=float), t.shape)
	ph = 2 * np.pi * np.cumsum(freq) / RATE + phase
	out = np.zeros_like(t)
	for k, a in harmonics:
		mask = freq * k < RATE / 2 * 0.95
		out += np.where(mask, a * np.sin(k * ph), 0.0)
	return out


def square_h(n=15):
	return [(k, 1.0 / k) for k in range(1, n + 1, 2)]


def saw_h(n=20, tilt=1.0):
	return [(k, 1.0 / k ** tilt) for k in range(1, n + 1)]


def place(buf, sig, at):
	i = int(at * RATE)
	end = min(len(buf), i + len(sig))
	buf[i:end] += sig[:end - i]


def bell(freq, seconds, bright=1.0, decay=1.0):
	"""A struck bell: inharmonic partials, the high ones dying first, a
	slow beat between two close partials (the metallic shimmer)."""
	t = t_axis(seconds)
	partials = [  # ratio, amplitude, decay time (s)
		(0.5, 0.20, 0.45),
		(1.0, 1.00, 0.38),
		(1.003, 0.35, 0.38),	# beats with the fundamental
		(2.0, 0.45 * bright, 0.25),
		(2.76, 0.30 * bright, 0.16),
		(5.40, 0.16 * bright, 0.09),
		(8.93, 0.08 * bright, 0.05),
	]
	out = np.zeros_like(t)
	for ratio, amp, tau in partials:
		f = freq * ratio
		if f >= RATE / 2 * 0.9:
			continue
		out += amp * np.exp(-t / (tau * decay)) * np.sin(2 * np.pi * f * t)
	return out * np.minimum(1, t / 0.002)


def pluck(freq, seconds, damp=0.18):
	"""A wooden pluck (marimba-like: fundamental and a 4th harmonic, short)."""
	t = t_axis(seconds)
	out = (np.sin(2 * np.pi * freq * t) * np.exp(-t / damp)
		+ 0.35 * np.sin(2 * np.pi * freq * 4 * t) * np.exp(-t / (damp / 5))
		+ 0.12 * np.sin(2 * np.pi * freq * 9.9 * t) * np.exp(-t / (damp / 12)))
	return out * np.minimum(1, t / 0.0015)


def brass(freq, seconds, attack=0.03, release=0.08, vibrato=0.0, bright=1.0, glide_from=None):
	t = t_axis(seconds)
	f = np.full_like(t, freq)
	if glide_from is not None:
		g = np.minimum(1, t / 0.06)
		f = glide_from + (freq - glide_from) * g
	if vibrato:
		f = f * (1 + vibrato * np.sin(2 * np.pi * 5.5 * t) * np.minimum(1, t / 0.25))
	# The brightness opens with the attack (a brass "blat").
	env = adsr(len(t), attack, 0.08, 0.8, release)
	tone = band_limited(f, t, saw_h(24, 1.0 + 0.6 / bright))
	tone2 = band_limited(f * 1.004, t, saw_h(24, 1.0 + 0.6 / bright), 1.0)
	return (tone + 0.6 * tone2) * env


def lowpass(x, cutoff):
	"""A gentle one-pole low-pass, run twice."""
	a = np.exp(-2 * np.pi * cutoff / RATE)
	for _ in range(2):
		y = np.empty_like(x)
		acc = 0.0
		for i, v in enumerate(x):
			acc = (1 - a) * v + a * acc
			y[i] = acc
		x = y
	return x


def reverb(x, mix=0.25, seconds=0.35):
	"""A short metallic room: four feedback combs (Schroeder), each
	decaying by 60 dB in `seconds`; the cue ends with its buffer."""
	out = np.copy(x)
	for delay_ms in (29.7, 37.1, 41.1, 43.7):
		d = int(delay_ms * RATE / 1000)
		g = 10 ** (-3 * delay_ms / 1000 / seconds)
		y = np.copy(x)
		for i in range(d, len(y)):
			y[i] += g * y[i - d]
		out += mix * (y - x) / 4
	return out


# The cues ------------------------------------------------------------------

def own_flag_taken():
	"""Our flag is taken: an urgent two-tone alarm (a hard square wave,
	a tritone apart, four times)."""
	buf = np.zeros(int(0.86 * RATE))
	hi, lo = 988.0, 698.0
	for k in range(4):
		f = hi if k % 2 == 0 else lo
		t = t_axis(0.19)
		sig = band_limited(f, t, square_h(19)) * adsr(len(t), 0.004, 0.03, 0.85, 0.025)
		place(buf, sig, k * 0.205)
	return buf


def enemy_flag_taken():
	"""We have their flag: a quick rising major call, bright synth."""
	buf = np.zeros(int(0.72 * RATE))
	notes = ['G4', 'C5', 'E5', 'G5']
	for k, n in enumerate(notes):
		last = k == len(notes) - 1
		t = t_axis(0.36 if last else 0.09)
		sig = band_limited(note_hz(n), t, saw_h(16, 1.3)) * adsr(len(t), 0.005, 0.05, 0.7, 0.2 if last else 0.02)
		place(buf, sig, k * 0.075)
	return buf


def own_flag_dropped():
	"""Their carrier lost our flag: two wooden plucks bouncing up (a
	fourth), the second answered an octave higher."""
	buf = np.zeros(int(0.62 * RATE))
	place(buf, pluck(note_hz('D5'), 0.4), 0.0)
	place(buf, pluck(note_hz('G5'), 0.45), 0.13)
	place(buf, 0.5 * pluck(note_hz('G6'), 0.3, 0.1), 0.26)
	return buf


def enemy_flag_dropped():
	"""Our carrier lost their flag: two low plucks falling a minor third,
	duller."""
	buf = np.zeros(int(0.62 * RATE))
	place(buf, pluck(note_hz('Eb5'), 0.42, 0.16), 0.0)
	place(buf, pluck(note_hz('C5'), 0.46, 0.2), 0.16)
	return lowpass(buf, 4000)


def own_flag_returned():
	"""Our flag is home: a reassuring falling major chime (G-E-C) with a
	metallic tail."""
	buf = np.zeros(int(1.35 * RATE))
	for k, n in enumerate(['G5', 'E5', 'C5']):
		place(buf, bell(note_hz(n), 1.35 - k * 0.14, bright=1.0, decay=1.0 + 0.25 * k), k * 0.14)
	return reverb(buf, 0.3, 0.3)


def enemy_flag_returned():
	"""Their flag is home: a rising minor chime, lower and darker
	(A-C-E)."""
	buf = np.zeros(int(1.35 * RATE))
	for k, n in enumerate(['A4', 'C5', 'E5']):
		place(buf, bell(note_hz(n), 1.35 - k * 0.14, bright=0.6, decay=0.9 + 0.25 * k), k * 0.14)
	return reverb(lowpass(buf, 4000), 0.3, 0.25)


def we_scored():
	"""We score: a brass fanfare, C-E-G and a held C major chord."""
	buf = np.zeros(int(1.45 * RATE))
	for k, n in enumerate(['C5', 'E5', 'G5']):
		place(buf, brass(note_hz(n), 0.13, 0.012, 0.04), k * 0.11)
	for n, a in (('C5', 0.8), ('E5', 0.6), ('G5', 0.6), ('C6', 0.8)):
		place(buf, a * brass(note_hz(n), 1.1, 0.02, 0.35, vibrato=0.006), 0.33)
	return reverb(buf, 0.2, 0.5)


def they_scored():
	"""They score: a brass phrase falling in minor (G-Eb-C), lower than
	the fanfare, the last note sagging."""
	buf = np.zeros(int(1.3 * RATE))
	place(buf, brass(note_hz('G4'), 0.24, 0.02, 0.05, bright=0.7), 0.0)
	place(buf, brass(note_hz('Eb4'), 0.24, 0.02, 0.05, bright=0.7), 0.25)
	t = t_axis(0.78)
	f0 = note_hz('C4')
	sag = f0 * (1 - 0.06 * np.clip((t - 0.3) / 0.45, 0, 1))
	env = adsr(len(t), 0.03, 0.1, 0.8, 0.4)
	tone = band_limited(sag, t, saw_h(24, 1.5)) + 0.3 * band_limited(sag * 1.003, t, saw_h(24, 1.5), 1.0)
	place(buf, tone * env, 0.5)
	return reverb(lowpass(buf, 3000), 0.15, 0.3)


CUES = [
	('ctf-own-flag-taken', own_flag_taken),
	('ctf-enemy-flag-taken', enemy_flag_taken),
	('ctf-own-flag-dropped', own_flag_dropped),
	('ctf-enemy-flag-dropped', enemy_flag_dropped),
	('ctf-own-flag-returned', own_flag_returned),
	('ctf-enemy-flag-returned', enemy_flag_returned),
	('ctf-we-scored', we_scored),
	('ctf-they-scored', they_scored),
]


# Levels --------------------------------------------------------------------

def a_weighting(x):
	"""IEC 61672 A-weighting (0 dB at 1 kHz), in the frequency domain."""
	spectrum = np.fft.rfft(x)
	f2 = np.fft.rfftfreq(len(x), 1 / RATE) ** 2
	ra = (12194.0 ** 2 * f2 ** 2) / ((f2 + 20.6 ** 2) * np.sqrt((f2 + 107.7 ** 2) * (f2 + 737.9 ** 2)) * (f2 + 12194.0 ** 2) + 1e-30)
	return np.fft.irfft(spectrum * ra / 0.7943, len(x))


def loudness(x, window=RATE // 20):
	x = a_weighting(x)
	n = len(x) // window
	if n == 0:
		return float(np.sqrt(np.mean(x ** 2)))
	fr = np.sqrt(np.mean(x[:n * window].reshape(n, window) ** 2, axis=1))
	return float(np.percentile(fr, 95))


def soft_limit(x):
	out = np.copy(x)
	m = np.abs(x) > KNEE
	span = PEAK - KNEE
	out[m] = np.sign(x[m]) * (KNEE + span * np.tanh((np.abs(x[m]) - KNEE) / span))
	return out


# The most the limiter may squash a peak (as the taunts' MAX_LIMITING).
MAX_DRIVE = 1.6


def normalise(x):
	"""To TARGET_LOUDNESS, the peaks above KNEE bent towards PEAK; a peak
	is driven at most MAX_DRIVE times full scale into the limiter (a cue
	that would need more stays quieter: the design is at fault)."""
	x = x - np.mean(x)
	x = x / np.max(np.abs(x))
	gain = 1.0
	for _ in range(40):
		l = loudness(soft_limit(x * gain))
		if abs(l - TARGET_LOUDNESS) < 0.002:
			break
		gain = min(MAX_DRIVE, gain * (TARGET_LOUDNESS / l) ** 0.8)
	y = soft_limit(x * gain)
	normalise.last_gain = gain
	# Fades against clicks: 3 ms in, 40 ms out.
	f = int(0.003 * RATE)
	y[:f] *= np.linspace(0, 1, f)
	f = int(0.04 * RATE)
	y[-f:] *= np.linspace(1, 0, f)
	return y


def write_wav(path, x):
	pcm = np.clip(np.round(x * 32767), -32768, 32767).astype('<i2')
	with wave.open(path, 'wb') as w:
		w.setnchannels(1)
		w.setsampwidth(2)
		w.setframerate(RATE)
		w.writeframes(pcm.tobytes())


def measure_s22(path):
	b = open(path, 'rb').read()
	_, _, n = struct.unpack_from('<4sii', b, 0)
	base = 12 + 20 * n
	levels = []
	for i in range(n):
		name, ln, _, off = struct.unpack_from('<8siii', b, 12 + 20 * i)
		d = (np.frombuffer(b, np.uint8, ln, base + off).astype(float) - 128) / 128
		if ln < RATE // 10:
			continue
		name = name.split(b'\0')[0].decode('latin1')
		levels.append((name, loudness(d), float(np.max(np.abs(d)))))
	v = [l for _, l, _ in levels]
	print('%s: %u samples, loudness median %.3f (quartiles %.3f, %.3f)' % (path, len(v), np.median(v), np.percentile(v, 25), np.percentile(v, 75)))
	for name, l, p in levels:
		if name.startswith('mes') or name == 'message':
			print('  %-8s loudness %.3f peak %.3f' % (name, l, p))


def plot(cues, out_dir):
	import matplotlib
	matplotlib.use('Agg')
	import matplotlib.pyplot as plt
	fig, axes = plt.subplots(len(cues), 2, figsize=(12, 2.1 * len(cues)), gridspec_kw={'width_ratios': [1, 1.3]})
	for row, (name, x) in zip(axes, cues):
		t = np.arange(len(x)) / RATE
		row[0].plot(t, x, lw=0.4, color='#2a6fdb')
		row[0].set_ylim(-1, 1)
		row[0].set_xlim(0, 1.5)
		row[0].set_title(name, fontsize=9, loc='left')
		row[1].specgram(x, NFFT=512, Fs=RATE, noverlap=384, cmap='magma', vmin=-110)
		row[1].set_ylim(0, 8000)
		row[1].set_xlim(0, 1.5)
		row[0].tick_params(labelsize=7)
		row[1].tick_params(labelsize=7)
	axes[-1][0].set_xlabel('s')
	axes[-1][1].set_xlabel('s (spectrogram, 0-8 kHz)')
	fig.tight_layout()
	path = os.path.join(out_dir, 'ctf-cues.png')
	fig.savefig(path, dpi=110)
	print('wrote', path)


def main():
	ap = argparse.ArgumentParser()
	ap.add_argument('--measure', help='a DESCENT2.S22 to measure (read only)')
	ap.add_argument('--plot', help='a directory for the waveform and spectrogram image')
	ap.add_argument('--out', default=OUT_DIR)
	args = ap.parse_args()
	if args.measure:
		measure_s22(args.measure)
	made = []
	for name, fn in CUES:
		x = normalise(fn())
		write_wav(os.path.join(args.out, name + '.wav'), x)
		made.append((name, x))
		print('%-24s %.2f s loudness %.3f peak %.3f drive %.2f' % (name, len(x) / RATE, loudness(x), np.max(np.abs(x)), normalise.last_gain))
	if args.plot:
		plot(made, args.plot)


if __name__ == '__main__':
	sys.exit(main())
