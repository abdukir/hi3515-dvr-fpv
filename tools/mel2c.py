#!/usr/bin/env python3
"""mel2c.py — turn a melody into the note_t table the DVR's buzzer plays.

The device buzzer is one bit of GPIO bit-banged by buzz_tone(), which BLOCKS for a note's
whole length while egg_tick() can only move the animation between notes. Note length is
therefore the animation's frame rate, so every note here is emitted as one short UNIT and
anything longer is that unit repeated. Do not "optimise" that back into long notes.

Input, either:
  a MIDI file            py -3 tools/mel2c.py song.mid
  or note names on stdin py -3 tools/mel2c.py -    <<< "A4/2 Bb4 C#5 D5/4 r/2"
                         (name[octave][/beats], 'r' = rest, default 1 beat)

Options:
  --unit MS     unit length, default 90 (see above)
  --beat N      units per beat for note-name input, default 2
  --max-sec S   trim the result to S seconds (the egg's caption clears at 9.3 s)
  --name SYM    C identifier for the array, default MEL_EGG
  --octave N    shift everything by N octaves; this transducer is thin below ~800 Hz

Prints a ready-to-paste C array.
"""
import sys, struct, argparse

A4 = 440.0
# semitones above C, because midi = 12*(octave+1) + this
STEP = {'c': 0, 'd': 2, 'e': 4, 'f': 5, 'g': 7, 'a': 9, 'b': 11}


def midi_to_hz(n):
    return A4 * (2.0 ** ((n - 69) / 12.0))


def name_to_midi(tok):
    """'C#5' / 'Bb4' / 'A' -> midi number (octave defaults to 4)."""
    s = tok.strip()
    if not s or s[0].lower() == 'r':
        return None
    i = 0
    letter = s[i].lower(); i += 1
    if letter not in STEP:
        raise SystemExit('bad note: ' + tok)
    semi = STEP[letter]
    while i < len(s) and s[i] in '#b':
        semi += 1 if s[i] == '#' else -1
        i += 1
    octv = int(s[i:]) if i < len(s) and s[i:].lstrip('-').isdigit() else 4
    return 12 * (octv + 1) + semi


# ----------------------------------------------------------------- MIDI reading
def _vlq(b, i):
    v = 0
    while True:
        c = b[i]; i += 1
        v = (v << 7) | (c & 0x7F)
        if not (c & 0x80):
            return v, i


WANT_TRACK = 0


def read_midi(path):
    """-> [(midi_note_or_None, duration_seconds)] for the most active single line."""
    b = open(path, 'rb').read()
    if b[:4] != b'MThd':
        raise SystemExit('not a MIDI file')
    ntrk, div = struct.unpack('>HH', b[10:14])
    if div & 0x8000:
        raise SystemExit('SMPTE time division not supported')
    tempo = 500000                      # default 120 bpm
    tracks, pos = [], 14
    for _ in range(ntrk):
        if b[pos:pos + 4] != b'MTrk':
            break
        ln = struct.unpack('>I', b[pos + 4:pos + 8])[0]
        tracks.append(b[pos + 8:pos + 8 + ln])
        pos += 8 + ln

    # Pick the MELODY track, not the busiest one. A drum kit easily has three times the
    # note count of the tune, and MIDI channel 10 (index 9) is percussion by definition;
    # a timbale part is hundreds of hits on ONE pitch. So: drop channel 10, then take the
    # track with the most DISTINCT pitches. --track overrides when a file disagrees.
    best, best_events, best_score = None, [], -1
    for tn, t in enumerate(tracks, 1):
        i, tick, status, ev = 0, 0, 0, []
        pitches, drum = set(), False
        while i < len(t):
            d, i = _vlq(t, i)
            tick += d
            if t[i] & 0x80:
                status = t[i]; i += 1
            if status == 0xFF:
                mt = t[i]; i += 1
                ln, i = _vlq(t, i)
                if mt == 0x51:
                    tempo = int.from_bytes(t[i:i + 3], 'big')
                i += ln
            elif status in (0xF0, 0xF7):
                ln, i = _vlq(t, i)
                i += ln
            else:
                hi = status & 0xF0
                if hi in (0x80, 0x90, 0xA0, 0xB0, 0xE0):
                    n, v = t[i], t[i + 1]; i += 2
                    if (status & 0x0F) == 9:
                        drum = True
                    if hi == 0x90 and v > 0:
                        ev.append((tick, 'on', n)); pitches.add(n)
                    elif hi == 0x80 or (hi == 0x90 and v == 0):
                        ev.append((tick, 'off', n))
                else:
                    i += 1
        ons = sum(1 for e in ev if e[1] == 'on')
        if not ons:
            continue
        score = -1 if drum else len(pitches)
        if WANT_TRACK:
            score = 1000 if tn == WANT_TRACK else -1
        if score > best_score:
            best, best_events, best_score = ons, ev, score

    if not best_events:
        raise SystemExit('no notes found')

    # monophonic reduction: at any moment take the highest sounding note
    spt = tempo / 1e6 / div             # seconds per tick
    times = sorted({e[0] for e in best_events})
    out, sounding, prev = [], {}, times[0]
    for tk in times:
        if tk > prev:
            top = max(sounding) if sounding else None
            out.append((top, (tk - prev) * spt))
            prev = tk
        for e in best_events:
            if e[0] != tk:
                continue
            if e[1] == 'on':
                sounding[e[2]] = 1
            else:
                sounding.pop(e[2], None)
    return out


# ----------------------------------------------------------------- emit
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src')
    ap.add_argument('--unit', type=int, default=90)
    ap.add_argument('--beat', type=int, default=2)
    ap.add_argument('--from-sec', type=float, default=0.0,
                    help='skip this many seconds in, e.g. to reach a faster section')
    ap.add_argument('--max-sec', type=float, default=0.0)
    ap.add_argument('--name', default='MEL_EGG')
    ap.add_argument('--octave', type=int, default=0)
    ap.add_argument('--track', type=int, default=0, help='1-based MIDI track to use')
    ap.add_argument('--min-note', default='',
                    help='treat anything below this note (e.g. A4) as a rest -- in a piano '
                         'arrangement the melody rests while the accompaniment carries on, '
                         'and the "highest sounding note" reduction then picks up the BASS')
    a = ap.parse_args()
    global WANT_TRACK
    WANT_TRACK = a.track

    if a.src == '-':
        seq = []
        for tok in sys.stdin.read().split():
            if '/' in tok:
                nm, beats = tok.split('/', 1); beats = float(beats)
            else:
                nm, beats = tok, 1.0
            seq.append((name_to_midi(nm), beats * a.beat * a.unit / 1000.0))
    else:
        seq = read_midi(a.src)

    # Start partway in when asked. The opening of a march is usually the declamatory part;
    # the interesting one is later, and this is cheaper than hand-trimming a note table.
    # A one-track piano arrangement has no melody/accompaniment separation, so wherever the
    # tune rests the reduction drops to whatever is left -- usually a bass note an octave or
    # two down. Those read as random low blips. Flooring them to rests restores the phrasing.
    if a.min_note:
        floor = name_to_midi(a.min_note)
        seq = [(n if (n is not None and n >= floor) else None, d) for n, d in seq]

    if a.from_sec > 0:
        # Keep only notes that START at or after the mark. Clipping a note that merely
        # OVERLAPS it drags in the tail of the previous phrase, which then opens the tune
        # on a stray pickup note.
        t, trimmed = 0.0, []
        for note, dur in seq:
            if t >= a.from_sec - 1e-6:
                trimmed.append((note, dur))
            t += dur
        seq = trimmed
        while seq and seq[0][0] is None:      # don't open on a rest
            seq.pop(0)

    units, budget = [], (a.max_sec if a.max_sec > 0 else 1e9)
    used = 0.0
    for note, dur in seq:
        n = max(1, int(round(dur * 1000.0 / a.unit)))
        for _ in range(n):
            cost = (a.unit + (12 if note is not None else 0)) / 1000.0
            if used + cost > budget:
                units = units; note = None; break
            used += cost
            units.append(note)
        if used >= budget:
            break

    # collapse trailing rests, they add nothing
    while units and units[-1] is None:
        units.pop()

    rows, i = [], 0
    while i < len(units):
        row = units[i:i + 4]
        cells = []
        for u in row:
            if u is None:
                cells.append('{0,%d}' % a.unit)
            else:
                hz = int(round(midi_to_hz(u + 12 * a.octave)))
                hz = min(max(hz, 50), 5000)
                cells.append('{%4d,%d}' % (hz, a.unit))
        rows.append('    ' + ','.join(cells) + ',')
        i += 4

    total = sum((a.unit + (12 if u is not None else 0)) for u in units) / 1000.0
    print('/* %d notes, %.2f s at a %d ms unit. Sustains are repeated units on purpose:' %
          (len(units), total, a.unit))
    print(' * buzz_tone() blocks, so note length IS the animation frame rate. */')
    print('static const note_t %s[] = {' % a.name)
    print('\n'.join(rows).rstrip(','))
    print('};')
    print('#define %s_N (int)(sizeof(%s)/sizeof(%s[0]))' % (a.name, a.name, a.name))


if __name__ == '__main__':
    main()
