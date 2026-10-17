# Nothing Ear EQ: decoding the share format

Goal: get EQ bands from our fitter (`dsp/fit/peq_fitter.h`) onto a Nothing
Ear (B171), so the earbuds' own 8-band advanced EQ runs our curve. The
firmware is not touched. Nothing X shares and imports EQ profiles as QR codes,
so writing that code is enough to hand the app any 8 bands.

Gadgetbridge supports Ear (1), (2), (a) and Stick, but not this model. Ear (a)
launched alongside it and probably shares the protocol family. That is
unverified.

## The format (decoded)

Read from one export ("Custom 1"), and confirmed by re-encoding its bands into
the app's exact bytes. The QR text is `base64(gzip(payload))`:

| Bytes | Content |
|-------|---------|
| 0     | `00`, unknown |
| 1     | `60` = 96 = 8 bands × 12 bytes (inferred: table length) |
| 2–97  | 8 × { gain dB, freq Hz, Q }, each a little-endian float32 |
| 98    | `01`, unknown |
| 99    | name length (inferred) |
| 100–  | name, UTF-8 (inferred; the sample is ASCII) |

There is no band type field, so every band is a peaking filter. Default band
centres are 55, 110, 220, 440, 1320, 3300, 6600 and 13200 Hz.

## Writing a code for Nothing X

`nothing_eq.py` decodes and encodes this format, so any 8 bands, the
fitter's included, can go onto the earbuds through the stock app's "scan EQ"
import:

```
python3 tools/nothing_eq/nothing_eq.py decode share.png
python3 tools/nothing_eq/nothing_eq.py encode --json bands.json --qr out.png
python3 tools/nothing_eq/nothing_eq.py --selftest
```

`bands.json` is `{"name": "...", "bands": [{"freq": 55, "gain": 0, "q": 1}, ...]}`
with exactly 8 bands. Fit with `lim.numBands = 8` and no `types`, since every
band is a peak.

Confirmed on a device: Nothing X imports a code it did not write. A generated
"TrypT test" (the sample with band 1 raised to +2 dB) scanned in as a new
profile with every band where it was set. What the two unknown bytes do is
still open; writing them as the sample has them works.

## Exporting for diffs

Only needed now to pin down the unknown bytes and the limits.

Turn off Personal Sound Profile and Enhanced Bass in Nothing X first; both
change the EQ data too.

1. Export a baseline with every band at 0 dB. Name it `flat.png`.
2. Change one value per export and name the file after the change:
   `b1_gain+3.png`, `b1_gain+6.png`, `b1_q2.png`, `b1_freq1000.png`.
3. Run:

   ```
   pip install zxing-cpp pillow
   python3 tools/nothing_eq/qr_diff.py --save decoded/ flat.png b1_gain+3.png b1_gain+6.png b1_q2.png
   ```

The script peels the layers off (URL field, hex or base64, zlib/gzip/deflate)
and prints what changed against the baseline: JSON fields, text tokens, or
bytes. A changed byte range is also printed as every integer and float type
that could hold it, with the delta. In this format a gain change reads as,
say, `f32le -2.5->-4`: a little-endian float holding the dB value as is.
`--save` keeps each raw string (`.txt`) and decoded payload (`.bin`).
`--selftest` runs it on a made-up payload to show what the output looks like.

## Limits to record

Drag each value to its ends in Nothing X and fill this in. The fitter needs
these numbers, or it can produce bands the earbuds will clamp or refuse.

| Value     | Min | Max | Step | Field (from the diff) |
|-----------|-----|-----|------|-----------------------|
| Frequency | per band | per band |  | float32, bytes 6+12n (band 1: 20–99 Hz, others unmeasured; `PeqLimits::bandFreqRange`) |
| Gain      | -6? | +6? |      | float32, bytes 2+12n (slider geometry: dots 3 dB apart, ends at ±6) |
| Q         | 0.1 | 10  |      | float32, bytes 10+12n (confirmed at both ends in the app) |
| Band type |  –  |  –  |  –   | none: all peaking     |
| Preamp    |     |     |      | not in the share code |

They map one to one onto `PeqLimits`:

```cpp
tryptify::fit::PeqLimits lim;
lim.numBands = 8;
lim.minFreq = /*Min*/;   lim.maxFreq = /*Max*/;   lim.freqStep = /*Step*/;
lim.minGainDb = /*Min*/; lim.maxGainDb = /*Max*/; lim.gainStep = /*Step*/;
lim.minQ = /*Min*/;      lim.maxQ = /*Max*/;      lim.qStep = /*Step*/;
// lim.types = {...};   only if some bands are fixed as shelves
auto fit = tryptify::fit::fitPeq(target, lim);
```

## Then (only for a custom app)

The QR route needs no app. For one that sends EQ live, capture the Bluetooth
traffic (Developer options → Bluetooth HCI snoop log, toggle Bluetooth, change
one band, `adb bugreport`, filter `btrfcomm` in Wireshark). The frame sent
right after the change should carry the same floats the share code does,
which gives the command to build the app around.
