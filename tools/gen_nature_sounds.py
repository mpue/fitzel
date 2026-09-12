"""Generate the nature ambience: birdsong one-shots and insect/leaf loops.

  bird_chaffinch_[a|b].wav  a descending, accelerating trill with a flourish
  bird_blackbird_[a|b].wav  slow fluted phrases, then a twittering tail
  bird_robin_[a|b].wav      thin, silvery warbles leaping about the scale
  bird_tit.wav              "tea-cher tea-cher", the great tit's two notes
  bird_chiffchaff.wav       "chiff-chaff", two notes, over and over
  bird_cuckoo.wav           the two falling notes, soft and far
  bird_woodpecker.wav       a drumming burst on dead wood
  insects_crickets.wav      (loop) a few crickets at dusk, chirping in pulses
  insects_meadow.wav        (loop) grasshoppers rasping in the summer grass
  leaves_rustle.wav         (loop) wind through a canopy, gusting

All synthesised -- no recordings, so nothing to license. A bird's syrinx makes
something close to a pure whistle swept fast in pitch, which is what these are:
a sine with a weak second and third harmonic, driven through a frequency curve
per note, with a short attack and a soft decay. What makes one species read as
itself is the SHAPE of the phrase (the chaffinch's accelerating descent, the
tit's alternation, the blackbird's slow glides), and that is what each function
below encodes.

The one-shots carry a short, quiet room of their own (a Schroeder reverb) --
the edge-of-a-wood echo that makes a whistle sound like it came from a tree
rather than out of the speaker. Everything else about where a bird is -- left,
right, near, far -- is added at play time by the spatializer (Soundscape.cpp).

The loops are built to wrap without a seam: their envelopes are periodic in
the loop length.

Same constraints as the other generators here: stdlib only.

    python tools/gen_nature_sounds.py content/sounds

(content/sounds is not in the repository -- run this once to create them;
Soundscape stays silent without them.)
"""
import math
import os
import random
import struct
import sys
import wave

SR = 44100
TAU = 2.0 * math.pi


def write(path, samples, peak_to=0.9):
    peak = max(1e-9, max(abs(s) for s in samples))
    g = peak_to / peak
    data = b"".join(struct.pack("<h", int(max(-1.0, min(1.0, s * g)) * 32767))
                    for s in samples)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(data)
    print("wrote", path, "(%.2fs)" % (len(samples) / SR))


def silence(seconds):
    return [0.0] * int(seconds * SR)


def note(out, start, dur, freq, amp=1.0, attack=0.004, release=None,
         harm=(1.0, 0.12, 0.04), vib=(0.0, 0.0), breath=0.006):
    """Add one whistled note at `start` seconds. `freq(u)` maps 0..1 across the
    note to Hz. vib = (rate Hz, depth Hz)."""
    n = int(dur * SR)
    s0 = int(start * SR)
    if release is None:
        release = dur * 0.45
    phase = 0.0
    need = s0 + n + 1
    if len(out) < need:
        out.extend([0.0] * (need - len(out)))
    for i in range(n):
        t = i / SR
        u = i / max(1, n - 1)
        f = freq(u) + vib[1] * math.sin(TAU * vib[0] * t)
        phase += TAU * f / SR
        env = min(1.0, t / attack) * min(1.0, (dur - t) / release) ** 1.5
        v = (harm[0] * math.sin(phase) + harm[1] * math.sin(2 * phase)
             + harm[2] * math.sin(3 * phase))
        v += breath * (random.random() * 2.0 - 1.0)
        out[s0 + i] += amp * env * v


def glide(a, b, curve=1.0):
    return lambda u: a + (b - a) * (u ** curve)


def reverb(x, mix=0.18, decay=0.55):
    """A small Schroeder reverb: four combs into two allpasses."""
    combs = [int(SR * d) for d in (0.0297, 0.0371, 0.0411, 0.0437)]
    g = [10 ** (-3 * d / decay) for d in (0.0297, 0.0371, 0.0411, 0.0437)]
    tail = int(decay * SR)
    x = x + [0.0] * tail
    wet = [0.0] * len(x)
    for c, gc in zip(combs, g):
        buf = [0.0] * c
        k = 0
        for i, s in enumerate(x):
            y = buf[k]
            buf[k] = s + y * gc
            wet[i] += y * 0.25
            k = (k + 1) % c
    for d, ga in ((int(SR * 0.005), 0.7), (int(SR * 0.0017), 0.7)):
        buf = [0.0] * d
        k = 0
        for i in range(len(wet)):
            y = buf[k]
            v = wet[i] + y * ga
            buf[k] = v
            wet[i] = y - ga * v
            k = (k + 1) % d
    return [a * (1.0 - mix) + b * mix for a, b in zip(x, wet)]


def highpass(x, fc):
    rc = 1.0 / (TAU * fc)
    a = rc / (rc + 1.0 / SR)
    y, prev_x, prev_y = [], 0.0, 0.0
    for s in x:
        prev_y = a * (prev_y + s - prev_x)
        prev_x = s
        y.append(prev_y)
    return y


def lowpass(x, fc):
    a = 1.0 - math.exp(-TAU * fc / SR)
    y, v = [], 0.0
    for s in x:
        v += a * (s - v)
        y.append(v)
    return y


# --- The songs ---------------------------------------------------------------

def chaffinch(seed):
    random.seed(seed)
    out = silence(0.1)
    t = 0.05
    n = random.randint(10, 13)
    top, bottom = random.uniform(4400, 5000), random.uniform(2900, 3300)
    for k in range(n):
        u = k / (n - 1)
        fa = top + (bottom - top) * u ** 0.8
        dur = 0.058 - 0.022 * u
        note(out, t, dur, glide(fa * 1.08, fa * 0.82), amp=0.6 + 0.4 * u)
        t += dur + 0.042 - 0.018 * u
    # The flourish: a quick down-and-up sweep that ends the phrase.
    t += 0.03
    note(out, t, 0.07, glide(5200, 3100, 0.7), amp=1.0)
    t += 0.08
    note(out, t, 0.16, lambda u: 2800 + 1400 * math.sin(math.pi * u) ** 2, amp=0.9)
    out += silence(0.1)
    return reverb(highpass(out, 1500))


def blackbird(seed):
    random.seed(seed)
    out = silence(0.1)
    t = 0.05
    f = random.uniform(1700, 2200)
    for _ in range(random.randint(4, 6)):
        dur = random.uniform(0.16, 0.32)
        g = random.uniform(1500, 2900)
        note(out, t, dur, glide(f, g, random.uniform(0.6, 1.6)), amp=random.uniform(0.7, 1.0),
             attack=0.02, harm=(1.0, 0.18, 0.05), vib=(random.uniform(14, 22), 35))
        f = g
        t += dur + random.uniform(0.05, 0.12)
    # The twittering tail, quieter and much higher.
    for _ in range(random.randint(5, 8)):
        fa = random.uniform(4200, 6200)
        note(out, t, 0.05, glide(fa, fa * 0.75), amp=0.35)
        t += 0.07
    out += silence(0.1)
    return reverb(highpass(out, 900), mix=0.22)


def robin(seed):
    random.seed(seed)
    out = silence(0.1)
    t = 0.05
    for k in range(random.randint(11, 16)):
        dur = random.uniform(0.04, 0.12)
        fa = random.uniform(3200, 7400)
        swell = math.sin(math.pi * k / 15.0) * 0.5 + 0.5
        if random.random() < 0.25:
            # A trill: the note shaken fast in pitch.
            note(out, t, dur * 1.6, lambda u, fa=fa: fa, amp=0.5 * swell + 0.2,
                 vib=(random.uniform(45, 70), random.uniform(300, 700)))
            t += dur * 1.6
        else:
            fb = fa * random.uniform(0.7, 1.3)
            note(out, t, dur, glide(fa, fb), amp=0.5 * swell + 0.3)
            t += dur
        t += random.uniform(0.02, 0.07)
    out += silence(0.1)
    return reverb(highpass(out, 2000), mix=0.2)


def great_tit():
    random.seed(7)
    out = silence(0.05)
    t = 0.05
    for _ in range(5):
        note(out, t, 0.085, glide(6900, 6500), amp=0.8)
        t += 0.085 + 0.035
        note(out, t, 0.12, glide(4300, 3900), amp=1.0)
        t += 0.12 + 0.11
    out += silence(0.1)
    return reverb(highpass(out, 2000))


def chiffchaff():
    random.seed(11)
    out = silence(0.05)
    t = 0.05
    hi, lo = 4700, 3900
    for k in range(10):
        f = hi if k % 2 == 0 else lo
        f *= random.uniform(0.97, 1.03)
        note(out, t, 0.085, glide(f * 1.04, f * 0.94), amp=0.9)
        t += 0.085 + random.uniform(0.11, 0.15)
    out += silence(0.1)
    return reverb(highpass(out, 2000))


def cuckoo():
    random.seed(13)
    out = silence(0.05)
    note(out, 0.05, 0.26, glide(712, 698), amp=1.0, attack=0.04, release=0.12,
         harm=(1.0, 0.25, 0.08), breath=0.01)
    note(out, 0.43, 0.36, glide(592, 572), amp=0.9, attack=0.05, release=0.18,
         harm=(1.0, 0.25, 0.08), breath=0.01)
    out += silence(0.2)
    return reverb(lowpass(out, 2500), mix=0.3, decay=0.9)


def woodpecker():
    random.seed(17)
    out = silence(1.3)
    t = 0.05
    rate = 19.0
    for k in range(20):
        s0 = int(t * SR)
        for i in range(int(0.03 * SR)):
            tt = i / SR
            env = math.exp(-tt / 0.006)
            v = (math.sin(TAU * 1150 * tt) * 0.6 + math.sin(TAU * 2300 * tt) * 0.25
                 + (random.random() * 2 - 1) * 0.5)
            if s0 + i < len(out):
                out[s0 + i] += env * v * (1.0 - 0.02 * k)
        t += 1.0 / rate
        rate *= 0.985            # the drum slows as it ends
    return reverb(highpass(out, 400), mix=0.3, decay=0.8)


# --- The loops ---------------------------------------------------------------

def crickets(seconds=10.0):
    random.seed(19)
    n = int(seconds * SR)
    out = [0.0] * n
    for voice in range(4):
        f = random.uniform(4300, 5200)
        period = random.uniform(0.38, 0.62)
        # Whole chirps per loop, so the loop wraps on the beat.
        count = max(1, round(seconds / period))
        period = seconds / count
        amp = random.uniform(0.35, 1.0)
        offset = random.uniform(0, period)
        pulses = random.randint(3, 4)
        for c in range(count):
            for p in range(pulses):
                t0 = (offset + c * period + p * 0.032) % seconds
                s0 = int(t0 * SR)
                for i in range(int(0.018 * SR)):
                    tt = i / SR
                    env = math.sin(math.pi * tt / 0.018)
                    out[(s0 + i) % n] += amp * env * math.sin(TAU * f * tt)
    return out


def meadow(seconds=12.0):
    random.seed(23)
    n = int(seconds * SR)
    out = [0.0] * n
    for voice in range(6):
        amp = random.uniform(0.3, 1.0)
        rate = random.uniform(18, 40)          # strokes a second
        bursts = random.randint(3, 6)
        for b in range(bursts):
            t0 = random.uniform(0, seconds)
            length = random.uniform(0.4, 1.6)
            s0 = int(t0 * SR)
            state = 0.0
            for i in range(int(length * SR)):
                tt = i / SR
                env = math.sin(math.pi * tt / length) ** 0.5
                stroke = max(0.0, math.sin(TAU * rate * tt)) ** 4
                # Bright noise: differenced white noise is a crude high-pass.
                r = random.random() * 2 - 1
                v = r - state
                state = r
                out[(s0 + i) % n] += amp * env * stroke * v
    # Stridulation lives between about 4 and 12 kHz; below that it is hiss.
    return lowpass(highpass(out, 4000), 12000)


def leaves(seconds=15.0):
    random.seed(29)
    n = int(seconds * SR)
    # Gusts: a few sines whose periods divide the loop, so the swell wraps.
    parts = [(random.randint(1, 3), random.uniform(0, TAU), random.uniform(0.3, 0.6))
             for _ in range(3)] + [(random.randint(4, 9), random.uniform(0, TAU), 0.15)]
    out = []
    lp1 = lp2 = 0.0
    a1 = 1.0 - math.exp(-TAU * 6000 / SR)
    a2 = 1.0 - math.exp(-TAU * 900 / SR)
    for i in range(n):
        t = i / SR
        g = sum(amp * (0.5 + 0.5 * math.sin(TAU * k * t / seconds + ph)) for k, ph, amp in parts)
        g = 0.25 + 0.75 * min(1.0, g / 1.2)
        r = random.random() * 2 - 1
        lp1 += a1 * (r - lp1)          # below 6 kHz
        lp2 += a2 * (lp1 - lp2)        # minus below 900 Hz: a band of hiss
        # Individual leaves: sparse clicks riding on the gusts.
        click = (random.random() * 2 - 1) * 3.0 if random.random() < 0.002 * g else 0.0
        out.append(((lp1 - lp2) + click * 0.2) * g)
    # A seam-free wrap: crossfade the last quarter second into the first.
    fade = int(0.25 * SR)
    for i in range(fade):
        w = i / fade
        out[i] = out[i] * w + out[n - fade + i] * (1.0 - w)
    return out[: n - fade]


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))
    write(os.path.join(d, "bird_chaffinch_a.wav"), chaffinch(1))
    write(os.path.join(d, "bird_chaffinch_b.wav"), chaffinch(2))
    write(os.path.join(d, "bird_blackbird_a.wav"), blackbird(3))
    write(os.path.join(d, "bird_blackbird_b.wav"), blackbird(4))
    write(os.path.join(d, "bird_robin_a.wav"), robin(5))
    write(os.path.join(d, "bird_robin_b.wav"), robin(6))
    write(os.path.join(d, "bird_tit.wav"), great_tit())
    write(os.path.join(d, "bird_chiffchaff.wav"), chiffchaff())
    write(os.path.join(d, "bird_cuckoo.wav"), cuckoo())
    write(os.path.join(d, "bird_woodpecker.wav"), woodpecker())
    write(os.path.join(d, "insects_crickets.wav"), crickets(), peak_to=0.7)
    write(os.path.join(d, "insects_meadow.wav"), meadow(), peak_to=0.7)
    write(os.path.join(d, "leaves_rustle.wav"), leaves(), peak_to=0.8)


if __name__ == "__main__":
    main()
