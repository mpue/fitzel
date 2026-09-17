"""Generate the sound effects of SKYSTRIKE (sandbox/scripts/shmup.lua).

  shmup_shot.wav     the vulcan: a short soft "tsik" -- it fires ten times a second
  shmup_missile.wav  a homing missile leaving the rail
  shmup_hit.wav      a shot ringing off armour
  shmup_boom_s.wav   a fighter going up
  shmup_boom_m.wav   a gunship, a turret, a boat
  shmup_boom_l.wav   a boss: a long rolling detonation
  shmup_bomb.wav     the bomb: a rising rush, then the blast
  shmup_die.wav      the player's ship lost
  shmup_power.wav    power-up: a quick rising arpeggio
  shmup_item.wav     a score gem collected (tiny, high)
  shmup_extend.wav   extra ship jingle
  shmup_warning.wav  the boss siren
  shmup_charge.wav   a laser drone charging
  shmup_laser.wav    its beam
  shmup_start.wav    game start
  shmup_clear.wav    stage clear fanfare

Arcade sounds are simple waveforms on purpose -- square and saw tones swept in
pitch, noise shaped by a filter and an envelope. What matters is that the cues
that fire constantly (shot, hit, item) are short and quiet, so that the ones
that carry news (warning, extend, power) are heard over them.

Same constraints as the other generators here: stdlib only.

    python tools/gen_shmup_sounds.py content/sounds

(content/sounds is not in the repository -- run this once; the game falls back
to the engine's missile sounds without them.)
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
    print(f"{path}: {len(samples) / SR:.2f}s")


def n_of(sec):
    return int(sec * SR)


def lowpass(x, fc):
    a = 1.0 - math.exp(-TAU * fc / SR)
    y, out = 0.0, []
    for s in x:
        y += a * (s - y)
        out.append(y)
    return out


def highpass(x, fc):
    lp = lowpass(x, fc)
    return [s - l for s, l in zip(x, lp)]


def sweep_lowpass(x, f0, f1):
    """One-pole lowpass whose cutoff glides from f0 to f1 (exponentially)."""
    y, out, n = 0.0, [], len(x)
    for i, s in enumerate(x):
        fc = f0 * (f1 / f0) ** (i / max(1, n - 1))
        a = 1.0 - math.exp(-TAU * fc / SR)
        y += a * (s - y)
        out.append(y)
    return out


def noise(sec, rng):
    return [rng.uniform(-1.0, 1.0) for _ in range(n_of(sec))]


def tone(sec, f0, f1=None, shape="sine", curve=1.0):
    """A tone gliding from f0 to f1. shape: sine | square | saw | tri."""
    f1 = f0 if f1 is None else f1
    n, ph, out = n_of(sec), 0.0, []
    for i in range(n):
        k = (i / max(1, n - 1)) ** curve
        f = f0 + (f1 - f0) * k
        ph += f / SR
        p = ph % 1.0
        if shape == "square":
            v = 1.0 if p < 0.5 else -1.0
        elif shape == "saw":
            v = 2.0 * p - 1.0
        elif shape == "tri":
            v = 4.0 * abs(p - 0.5) - 1.0
        else:
            v = math.sin(TAU * p)
        out.append(v)
    return out


def env(x, attack, decay, hold=0.0):
    """Linear attack, optional hold, exponential decay (decay = time constant)."""
    out = []
    for i, s in enumerate(x):
        t = i / SR
        if t < attack:
            g = t / attack
        elif t < attack + hold:
            g = 1.0
        else:
            g = math.exp(-(t - attack - hold) / decay)
        out.append(s * g)
    return out


def fade_out(x, sec):
    n = min(len(x), n_of(sec))
    for i in range(n):
        x[len(x) - n + i] *= 1.0 - i / n
    return x


def add(*tracks, at=None):
    at = at or [0.0] * len(tracks)
    length = max(n_of(a) + len(t) for t, a in zip(tracks, at))
    out = [0.0] * length
    for t, a in zip(tracks, at):
        o = n_of(a)
        for i, s in enumerate(t):
            out[o + i] += s
    return out


def gain(x, g):
    return [s * g for s in x]


def softclip(x, drive=1.5):
    return [math.tanh(s * drive) for s in x]


# --- The cues ------------------------------------------------------------------

def shot(rng):
    body = env(tone(0.05, 2400.0, 900.0, "square", 0.6), 0.001, 0.012)
    body = lowpass(body, 5000.0)
    tick = env(highpass(noise(0.02, rng), 3000.0), 0.0005, 0.004)
    return fade_out(add(gain(body, 0.6), gain(tick, 0.5)), 0.01)


def missile(rng):
    rush = sweep_lowpass(noise(0.32, rng), 900.0, 5000.0)
    rush = env(rush, 0.02, 0.09)
    return fade_out(rush, 0.05)


def hit(rng):
    ping = env(tone(0.05, 3100.0, 2600.0, "tri"), 0.0005, 0.01)
    crack = env(highpass(noise(0.03, rng), 2500.0), 0.0005, 0.005)
    return fade_out(add(gain(ping, 0.5), crack), 0.01)


def boom(rng, length, low, bright, crackles=0):
    body = sweep_lowpass(noise(length, rng), bright, 180.0)
    body = env(body, 0.004, length * 0.28)
    thump = env(tone(length * 0.7, low * 2.2, low * 0.6, "sine", 0.5), 0.003, length * 0.2)
    parts, at = [body, gain(thump, 1.2)], [0.0, 0.0]
    for _ in range(crackles):
        c = env(sweep_lowpass(noise(0.4, rng), 4000.0, 300.0), 0.002, 0.07)
        parts.append(gain(c, rng.uniform(0.3, 0.6)))
        at.append(rng.uniform(0.08, length * 0.6))
    return fade_out(softclip(add(*parts, at=at), 1.3), 0.08)


def bomb(rng):
    rise = sweep_lowpass(noise(0.55, rng), 300.0, 6000.0)
    rise = [s * (i / len(rise)) ** 2 for i, s in enumerate(rise)]
    blast = boom(rng, 1.6, 45.0, 3500.0, crackles=4)
    return fade_out(add(gain(rise, 0.6), blast, at=[0.0, 0.5]), 0.1)


def die(rng):
    fall = env(tone(0.7, 900.0, 90.0, "square", 0.7), 0.002, 0.25)
    fall = lowpass(fall, 2500.0)
    return fade_out(add(gain(fall, 0.45), boom(rng, 1.2, 50.0, 2800.0, 3)), 0.1)


def arpeggio(notes, step, shape="square", decay=0.07, cutoff=4000.0):
    parts, at = [], []
    for i, f in enumerate(notes):
        parts.append(env(tone(step * 2.2, f, f, shape), 0.002, decay))
        at.append(i * step)
    return fade_out(lowpass(add(*parts, at=at), cutoff), 0.03)


def power(rng):
    return arpeggio([523.3, 659.3, 784.0, 1046.5, 1318.5], 0.045)


def item(rng):
    return fade_out(env(tone(0.07, 1800.0, 2600.0, "tri"), 0.001, 0.02), 0.01)


def extend(rng):
    a = arpeggio([784.0, 987.8, 1174.7, 1568.0], 0.09, decay=0.12)
    b = env(tone(0.5, 1568.0, 1568.0, "square"), 0.004, 0.2)
    b2 = env(tone(0.5, 1975.5, 1975.5, "square"), 0.004, 0.2)
    return fade_out(lowpass(add(a, gain(b, 0.5), gain(b2, 0.35), at=[0.0, 0.36, 0.36]),
                            4500.0), 0.1)


def warning(rng):
    parts, at = [], []
    for k in range(6):
        f = 620.0 if k % 2 == 0 else 830.0
        parts.append(env(tone(0.36, f, f, "square"), 0.01, 0.5, hold=0.28))
        at.append(k * 0.38)
    return fade_out(lowpass(add(*parts, at=at), 2200.0), 0.1)


def charge(rng):
    x = tone(0.9, 180.0, 1400.0, "saw", 1.8)
    trem = [s * (0.7 + 0.3 * math.sin(TAU * 18.0 * i / SR)) for i, s in enumerate(x)]
    return fade_out(lowpass(env(trem, 0.2, 10.0), 3000.0), 0.05)


def laser(rng):
    buzz = tone(1.3, 110.0, 104.0, "saw")
    hiss = highpass(noise(1.3, rng), 1500.0)
    x = add(gain(buzz, 0.7), gain(hiss, 0.25))
    x = [s * (0.8 + 0.2 * math.sin(TAU * 31.0 * i / SR)) for i, s in enumerate(x)]
    return fade_out(env(lowpass(x, 3500.0), 0.01, 10.0), 0.25)


def start(rng):
    chord = add(*[env(tone(0.8, f, f, "tri"), 0.05, 0.3) for f in (392.0, 523.3, 659.3, 784.0)])
    sweep = env(sweep_lowpass(noise(0.6, rng), 400.0, 6000.0), 0.3, 0.1)
    return fade_out(add(chord, gain(sweep, 0.3)), 0.1)


def clear(rng):
    run = arpeggio([523.3, 659.3, 784.0, 1046.5], 0.11, decay=0.1)
    final = add(*[env(tone(0.9, f, f, "square"), 0.005, 0.35)
                  for f in (1046.5, 1318.5, 1568.0)])
    return fade_out(lowpass(add(run, gain(final, 0.45), at=[0.0, 0.46]), 4000.0), 0.15)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "content/sounds"
    os.makedirs(out, exist_ok=True)
    rng = random.Random(1987)
    cues = [
        ("shmup_shot.wav", shot, 0.45), ("shmup_missile.wav", missile, 0.45),
        ("shmup_hit.wav", hit, 0.35), ("shmup_item.wav", item, 0.4),
        ("shmup_boom_s.wav", lambda r: boom(r, 0.5, 70.0, 3000.0), 0.8),
        ("shmup_boom_m.wav", lambda r: boom(r, 0.9, 55.0, 3500.0, 2), 0.85),
        ("shmup_boom_l.wav", lambda r: boom(r, 2.4, 40.0, 4000.0, 7), 0.9),
        ("shmup_bomb.wav", bomb, 0.9), ("shmup_die.wav", die, 0.9),
        ("shmup_power.wav", power, 0.6), ("shmup_extend.wav", extend, 0.7),
        ("shmup_warning.wav", warning, 0.65), ("shmup_charge.wav", charge, 0.5),
        ("shmup_laser.wav", laser, 0.55), ("shmup_start.wav", start, 0.6),
        ("shmup_clear.wav", clear, 0.7),
    ]
    for name, fn, peak in cues:
        write(os.path.join(out, name), fn(rng), peak)


if __name__ == "__main__":
    main()
