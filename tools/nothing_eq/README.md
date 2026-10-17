# Nothing Ear EQ: decoding the share format

Goal: send EQ bands from our fitter (`dsp/fit/peq_fitter.h`) to a Nothing Ear
(B171) over its RFCOMM control channel, so the earbuds' own 8-band advanced EQ
runs our curve. The firmware is not touched. Nothing X's "share EQ" QR code
holds the same band data the app sends, and it is the easiest place to read
the format from.

Gadgetbridge supports Ear (1), (2), (a) and Stick, but not this model. Ear (a)
launched alongside it and probably shares the protocol family. That is
unverified.

## Exporting

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
that could hold it, with the delta. A +3 dB change showing up as `i16le +300`
means gain is stored as a little-endian int16 in hundredths of a dB.
`--save` keeps each raw string (`.txt`) and decoded payload (`.bin`).
`--selftest` runs it on a made-up payload to show what the output looks like.

## Limits to record

Drag each value to its ends in Nothing X and fill this in. The fitter needs
these numbers, or it can produce bands the earbuds will clamp or refuse.

| Value     | Min | Max | Step | Field (from the diff) |
|-----------|-----|-----|------|-----------------------|
| Frequency |     |     |      |                       |
| Gain      |     |     |      |                       |
| Q         |     |     |      |                       |
| Band type |  –  |  –  |  –   | fixed per band? which? |
| Preamp    |     |     |      | exists?               |

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

## Then

Capture the Bluetooth traffic (Developer options → Bluetooth HCI snoop log,
toggle Bluetooth, change one band, `adb bugreport`, filter `btrfcomm` in
Wireshark). The frame sent right after the change should carry the same
values the QR diff found, which gives the command to build the app around.
