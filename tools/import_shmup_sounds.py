"""Cut SKYSTRIKE's sound effects (sandbox/scripts/shmup.lua) out of a sample library.

    python tools/import_shmup_sounds.py D:/samples content/sounds

The sources are recorded explosions and foley from the library under <root>
(the Bluezone packs BC0197/BC0200/BC0214 in ExplosionSounds/, and fx/). Each cue
takes a slice of one file, fades it out, and is levelled to the loudness its
job wants: fighters going up forty times a stage stay well below a boss, the
armour clank below both. The results are written as skystrike_*.wav (16-bit,
the source's rate and channels) -- the game prefers them to the synthesised
shmup_*.wav from gen_shmup_sounds.py, and picks one of several variants at
random where there are several, so a wave of kills does not sound like one
sample on repeat.

Needs numpy. content/sounds is not in the repository; the sample library is
not either -- this records which slice of which file each cue is.
"""
import os
import struct
import sys
import wave

import numpy as np

BZ = "ExplosionSounds/"
EXP = BZ + "BC0214-wav/explosions/Bluezone-BC0214-explosion-%03d.wav"

# out name: (source, start s, length s or None = to the end, fade-out s, target RMS dBFS
#            [, fade-in s])
CUES = {
    "skystrike_boom_s1": ("fx/explosion_short.wav", 0.0, None, 0.2, -17),
    "skystrike_boom_s2": (EXP % 11, 0.0, 1.1, 0.4, -17),
    "skystrike_boom_s3": (EXP % 13, 0.0, 1.2, 0.45, -17),
    "skystrike_boom_s4": (EXP % 16, 0.0, 1.2, 0.45, -17),
    "skystrike_boom_m1": (EXP % 31, 0.0, 2.0, 0.7, -15),
    "skystrike_boom_m2": (EXP % 1, 0.0, 2.0, 0.7, -15),
    "skystrike_boom_m3": ("fx/explosion2.wav", 0.0, None, 0.4, -15),
    "skystrike_boom_m4": (EXP % 7, 0.0, 2.0, 0.7, -15),
    "skystrike_boom_l1": ("fx/the_biggest_known_explosion_sound.wav", 0.0, 6.5, 2.8, -13),
    "skystrike_boom_l2": (EXP % 17, 0.0, None, 1.2, -13),
    "skystrike_boom_l3": (EXP % 22, 0.0, None, 1.2, -13),
    "skystrike_bomb": (BZ + "BC0214-wav/explosions-whooshes/Bluezone-BC0214-explosion-whooshe-013.wav",
                       0.0, None, 1.0, -13),
    "skystrike_die": ("fx/explosion_player.wav", 0.0, None, 0.4, -13),
    # The mine: ten beeps 0.125 s apart, the blast at 1.30 s. Cut in two, so a
    # mine shot while it beeps does not blow up in the speakers anyway.
    "skystrike_mine_arm": (BZ + "BC0214-wav/explosions-beeps-mines/Bluezone-BC0214-beep-mine-explosion-006.wav",
                           0.0, 1.27, 0.02, -21),
    "skystrike_mine_boom": (BZ + "BC0214-wav/explosions-beeps-mines/Bluezone-BC0214-beep-mine-explosion-006.wav",
                            1.27, 3.0, 1.0, -15),
    "skystrike_clank1": (BZ + "BC0200-wav/howitzer-shot-metallic-parts/Bluezone-BC0200-howitzer-shot-metallic-part-012.wav",
                         0.0, 0.32, 0.2, -25),
    "skystrike_clank2": (BZ + "BC0200-wav/howitzer-shot-metallic-parts/Bluezone-BC0200-howitzer-shot-metallic-part-005.wav",
                         0.0, 0.32, 0.2, -25),
    "skystrike_clank3": (BZ + "BC0200-wav/howitzer-shot-metallic-parts/Bluezone-BC0200-howitzer-shot-metallic-part-009.wav",
                         0.0, 0.32, 0.2, -25),
    "skystrike_launch": (BZ + "BC0200-wav/grenade-launchers/Bluezone-BC0200-grenade-launcher-004.wav",
                         0.0, 0.7, 0.3, -20),
    "skystrike_missile": ("fx/110616__soundscalpel-com__foley-cable-whoosh-air-002.wav", 0.0, None, 0.05, -25),
    "skystrike_cannon1": (BZ + "BC0200-wav/howitzer-shot-distant-explosions/Bluezone-BC0200-howitzer-shot-distant-explosion-001.wav",
                          0.0, 1.6, 0.8, -21),
    "skystrike_cannon2": (BZ + "BC0200-wav/howitzer-shot-distant-explosions/Bluezone-BC0200-howitzer-shot-distant-explosion-010.wav",
                          0.0, 1.6, 0.8, -21),
    "skystrike_debris": (BZ + "BC0197-wav/falling-metal-debris/Bluezone-BC0197-falling-metal-debris-008.wav",
                         0.0, None, 0.5, -19),
    # STORM FRONT: single claps cut out of long thunderstorm recordings, and a
    # stretch of rain the stage lays over itself every few seconds.
    "skystrike_thunder1": ("fx/495330__tosha73__five-real-thunder-sounds.wav", 26.9, 6.5, 2.5, -19, 0.05),
    "skystrike_thunder2": ("fx/495330__tosha73__five-real-thunder-sounds.wav", 43.0, 7.0, 2.5, -19, 0.05),
    "skystrike_thunder3": ("fx/gewitter_001.wav", 3.8, 7.0, 2.5, -19, 0.05),
    "skystrike_thunder4": ("fx/gewitter_002.wav", 32.6, 6.5, 2.5, -19, 0.05),
    "skystrike_rain": ("fx/regen_001.wav", 10.0, 16.0, 2.0, -27, 2.0),
}


def read(path):
    """WAV (PCM 16/24/32 or float) -> (float array frames x channels, rate)."""
    data = open(path, "rb").read()
    if data[:4] != b"RIFF":
        raise ValueError(path + ": not a RIFF/WAV file")
    pos, fmt, raw = 12, None, None
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            fmt = struct.unpack("<HHIIHH", body[:16])
            if fmt[0] == 0xFFFE and len(body) >= 26:            # WAVE_FORMAT_EXTENSIBLE
                fmt = (struct.unpack("<H", body[24:26])[0],) + fmt[1:]
        elif cid == b"data":
            raw = body
        pos += 8 + size + (size & 1)
    tag, ch, sr, _, align, bits = fmt
    n = len(raw) // align
    raw = raw[:n * align]
    if tag == 3:
        a = np.frombuffer(raw, dtype="<f4" if bits == 32 else "<f8").astype(np.float64)
    elif bits == 16:
        a = np.frombuffer(raw, dtype="<i2").astype(np.float64) / 32768.0
    elif bits == 24:
        b = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        a = np.where(v >= 1 << 23, v - (1 << 24), v).astype(np.float64) / (1 << 23)
    elif bits == 32:
        a = np.frombuffer(raw, dtype="<i4").astype(np.float64) / (1 << 31)
    else:
        a = (np.frombuffer(raw, dtype=np.uint8).astype(np.float64) - 128.0) / 128.0
    return a.reshape(-1, ch), sr


def cut(x, sr, start, length, fade, target, fade_in=None):
    s = int(start * sr)
    e = len(x) if length is None else min(len(x), s + int(length * sr))
    y = x[s:e].copy()
    # A few milliseconds of fade-in when the cut starts mid-sound, none when it
    # starts at the file's own beginning (that transient is the point).
    if s > 0 or fade_in:
        k = min(len(y), int((fade_in or 0.004) * sr))
        y[:k] *= np.linspace(0.0, 1.0, k)[:, None]
    k = min(len(y), int(fade * sr))
    if k > 0:
        y[-k:] *= (np.linspace(1.0, 0.0, k) ** 2)[:, None]
    # Level the body (the loudest half second), then keep the peak under -1 dBFS.
    w = int(0.5 * sr)
    if len(y) > w:
        c = np.concatenate([[0.0], np.cumsum(np.mean(y ** 2, axis=1))])
        rms = np.sqrt(((c[w:] - c[:-w]) / w).max()) + 1e-12
    else:
        rms = np.sqrt(np.mean(y ** 2)) + 1e-12
    y *= 10 ** (target / 20.0) / rms
    peak = np.abs(y).max()
    if peak > 0.89:
        y *= 0.89 / peak
    return y


def write(path, y, sr):
    pcm = (np.clip(y, -1.0, 1.0) * 32767.0).astype("<i2")
    with wave.open(path, "wb") as w:
        w.setnchannels(y.shape[1])
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes(pcm.tobytes())


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    root = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else "content/sounds"
    os.makedirs(out, exist_ok=True)
    for name, (src, start, length, fade, target, *rest) in CUES.items():
        x, sr = read(os.path.join(root, src))
        y = cut(x, sr, start, length, fade, target, *rest)
        write(os.path.join(out, name + ".wav"), y, sr)
        print(f"{name}.wav  {len(y) / sr:5.2f}s  <- {src}")


if __name__ == "__main__":
    main()
