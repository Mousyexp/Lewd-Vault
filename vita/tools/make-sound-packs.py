# The extra sound packs (vita/sounds/<pack>/*.wav, 48 kHz mono 16-bit): the same fourteen sounds as
# the Vita's own (vita/sounds/*.wav), synthesised here so they're the app's own. Each pack plays the
# same little tunes with its own voice: Soft (sine plucks with a short echo), Retro (8-bit square
# waves) and Bubbly (bright pops that slide up).
#   python3 vita/tools/make-sound-packs.py
import math, os, random, struct, wave

RATE = 48000
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'sounds')

def hz(note):  # MIDI note number to frequency
    return 440.0 * 2 ** ((note - 69) / 12)

# Each sound: notes (MIDI number, start in s, length in s); None is a click of noise
TUNES = {
    'tick':      [(96, 0.0, 0.012)],
    'select':    [(81, 0.0, 0.05), (88, 0.045, 0.07)],
    'back':      [(86, 0.0, 0.05), (79, 0.045, 0.07)],
    'toggle':    [(84, 0.0, 0.06)],
    'open':      [(76, 0.0, 0.05), (81, 0.04, 0.05), (88, 0.08, 0.08)],
    'close':     [(88, 0.0, 0.05), (81, 0.04, 0.05), (76, 0.08, 0.08)],
    'tab':       [(79, 0.0, 0.06)],
    'error':     [(57, 0.0, 0.09), (56, 0.11, 0.14)],
    'saved':     [(72, 0.0, 0.07), (76, 0.06, 0.07), (79, 0.12, 0.07), (84, 0.18, 0.22)],
    'done':      [(79, 0.0, 0.07), (84, 0.07, 0.18)],
    'delete':    [(64, 0.0, 0.05), (55, 0.05, 0.12), (None, 0.0, 0.05)],
    'slideshow': [(72, 0.0, 0.5), (76, 0.0, 0.5), (79, 0.05, 0.5), (84, 0.1, 0.45)],
    'more':      [(91, 0.0, 0.04)],
    'start':     [(72, 0.0, 0.1), (76, 0.1, 0.1), (79, 0.2, 0.1), (83, 0.3, 0.1), (84, 0.4, 0.4)],
}

def voice(pack, note, t, length):
    """One sample of one note at time t into it (0..length), -1..1."""
    if note is None:
        return random.uniform(-1, 1) * math.exp(-t * 80)
    f = hz(note)
    if pack == 'soft':
        env = min(1.0, t / 0.004) * math.exp(-t * 9 / max(length, 0.05))
        return env * (math.sin(2 * math.pi * f * t) * 0.8 + math.sin(4 * math.pi * f * t) * 0.15)
    if pack == 'retro':
        env = 1.0 if t < length * 0.85 else max(0.0, 1 - (t - length * 0.85) / (length * 0.15 + 1e-6))
        ph = (f * t) % 1.0
        return env * (0.55 if ph < 0.25 else -0.55)
    # bubbly: the pitch slides up a little at the start, a fast decay
    glide = f * (1 + 0.6 * math.exp(-t * 60)) if t < 0.05 else f * (1 + 0.6 * math.exp(-3))
    env = min(1.0, t / 0.002) * math.exp(-t * 14 / max(length, 0.05))
    return env * math.sin(2 * math.pi * glide * t * (1.0 if t < 0.05 else 1.0))

def render(pack, tune):
    end = max(s + l for _, s, l in tune) + (0.12 if pack == 'soft' else 0.02)
    n = int(end * RATE)
    out = [0.0] * n
    for note, start, length in tune:
        s0 = int(start * RATE)
        tail = 0.06 if pack != 'retro' else 0.0
        for i in range(int((length + tail) * RATE)):
            if s0 + i >= n:
                break
            t = i / RATE
            if pack == 'retro' and t > length:
                break
            out[s0 + i] += voice(pack, note, t, length)
    if pack == 'soft':  # a short echo, quieter
        d = int(0.09 * RATE)
        for i in range(n - 1, d - 1, -1):
            out[i] += out[i - d] * 0.28
    peak = max(1e-6, max(abs(x) for x in out))
    gain = 0.8 / peak
    fade = int(0.004 * RATE)  # (no click at the end)
    for i in range(fade):
        out[n - 1 - i] *= i / fade
    return [int(max(-1, min(1, x * gain)) * 32767) for x in out]

def main():
    random.seed(7)
    for pack in ('soft', 'retro', 'bubbly'):
        d = os.path.join(ROOT, pack)
        os.makedirs(d, exist_ok=True)
        for name, tune in TUNES.items():
            pcm = render(pack, tune)
            with wave.open(os.path.join(d, name + '.wav'), 'wb') as w:
                w.setnchannels(1)
                w.setsampwidth(2)
                w.setframerate(RATE)
                w.writeframes(struct.pack('<%dh' % len(pcm), *pcm))
    print('made 3 packs of', len(TUNES), 'sounds in vita/sounds/')

main()
