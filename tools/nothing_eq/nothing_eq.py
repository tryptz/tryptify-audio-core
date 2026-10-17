#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Read and write Nothing X advanced-EQ share codes.

Nothing X can import an EQ profile by scanning a QR code. Writing that code
ourselves puts any band set (the PEQ fitter's, say) on the earbuds through the
stock app, with no firmware or Bluetooth work.

    nothing_eq.py decode share.png          # or a .txt file, or the string itself
    nothing_eq.py encode --name "TrypT fit" --band 55:0:1 --band 110:0:1 ... (8 bands)
    nothing_eq.py encode --json bands.json --qr out.png
    nothing_eq.py --selftest

A band is FREQ:GAIN:Q. The JSON form is {"name": "...", "bands":
[{"freq": 55, "gain": 0, "q": 1}, ...]}. --qr needs the segno package; images
for decode need zxing-cpp and pillow.

Format, read from one export of an Ear (B171), so every field marked
"inferred" is a guess that fits that one sample:

    QR text  = base64(gzip(payload)), standard alphabet, padded
    payload  =
      u8      0x00      unknown; 0 in the sample
      u8      0x60      96, which is 8 bands x 12 bytes (inferred: table length)
      8 x {
        f32le gain      dB
        f32le freq      Hz
        f32le q
      }
      u8      0x01      unknown; 1 in the sample
      u8      n         name length (inferred)
      n bytes name      UTF-8 (inferred; the sample is ASCII)

Every band is a peaking filter; there is no type field. The sample's Q values
reach 0.1 and 10, which look like the app's ends but are not confirmed.
"""

import argparse
import base64
import gzip
import json
import struct
import sys
from pathlib import Path

NUM_BANDS = 8
BAND = struct.Struct("<fff")  # gain, freq, q

# The export this format was read from: "Custom 1" by TrypT, Nothing Ear.
SAMPLE = ("H4sIAAAAAAAA/2NIYACDGCcGhgZ7CPsOlK1wACjuDKQdGRgcgOw7zmfPnLGFqFnqAlfv4OcKVe/A4HDO9e"
          "yZNQ5QcTeQOCOHc2lxSX6ugiEADU1TRmwAAAA=")
SAMPLE_BANDS = [(55, 0.0, 1.0), (110, 0.0, 1.0), (220, -2.5, 10.0), (440, -3.0, 0.1),
                (1320, 0.0, 1.0), (3300, 0.0, 1.0), (6600, 2.5, 5.4), (13200, 0.0, 1.0)]


def parse(payload):
    """Payload bytes -> {"name", "bands": [(freq, gain, q)], "unknown": {...}}."""
    if len(payload) < 2:
        raise ValueError("payload too short")
    lead, table = payload[0], payload[1]
    if table % BAND.size:
        raise ValueError(f"byte 1 is {table}, not a whole number of {BAND.size}-byte bands")
    end = 2 + table
    if len(payload) < end + 2:
        raise ValueError(f"payload is {len(payload)} bytes, band table needs {end + 2}")
    bands = []
    for off in range(2, end, BAND.size):
        gain, freq, q = BAND.unpack_from(payload, off)
        bands.append((freq, gain, q))
    flag, n = payload[end], payload[end + 1]
    name = payload[end + 2:end + 2 + n].decode("utf-8", errors="replace")
    extra = payload[end + 2 + n:]
    return {"name": name, "bands": bands,
            "unknown": {"byte0": lead, "after_bands": flag, "trailing": extra.hex()}}


def build(bands, name):
    """(freq, gain, q) x 8 and a name -> payload bytes. The unknown bytes are
    written as the sample has them."""
    if len(bands) != NUM_BANDS:
        raise ValueError(f"need exactly {NUM_BANDS} bands, got {len(bands)}")
    raw_name = name.encode("utf-8")
    if len(raw_name) > 255:
        raise ValueError("name is longer than 255 bytes")
    out = bytes([0x00, NUM_BANDS * BAND.size])
    for freq, gain, q in bands:
        out += BAND.pack(gain, freq, q)
    return out + bytes([0x01, len(raw_name)]) + raw_name


def to_qr_text(payload):
    # mtime 0 and level 6 give the app's own header byte for byte (level 9
    # would set the "max compression" flag the app's export doesn't have).
    return base64.b64encode(gzip.compress(payload, compresslevel=6, mtime=0)).decode("ascii")


def from_qr_text(text):
    return gzip.decompress(base64.b64decode(text.strip(), validate=True))


def warn_out_of_range(bands):
    for i, (freq, gain, q) in enumerate(bands, 1):
        if not 20 <= freq <= 20000:
            print(f"warning: band {i} freq {freq:g} Hz is outside 20..20000", file=sys.stderr)
        if not 0.1 <= q <= 10:
            print(f"warning: band {i} Q {q:g} is outside 0.1..10, the range seen in the app",
                  file=sys.stderr)


def read_input(arg):
    path = Path(arg)
    if path.suffix.lower() in (".png", ".jpg", ".jpeg", ".webp", ".bmp"):
        try:
            import zxingcpp
            from PIL import Image
        except ImportError:
            sys.exit("decoding images needs: pip install zxing-cpp pillow")
        found = zxingcpp.read_barcodes(Image.open(path))
        if not found:
            sys.exit(f"{path}: no QR code found")
        return found[0].text
    if path.suffix.lower() == ".txt":
        return path.read_text(encoding="utf-8")
    return arg


def print_profile(profile):
    print(f"name: {profile['name']}")
    print(f"{'band':>4}  {'freq Hz':>9}  {'gain dB':>8}  {'Q':>6}")
    for i, (freq, gain, q) in enumerate(profile["bands"], 1):
        print(f"{i:>4}  {freq:>9.1f}  {gain:>+8.2f}  {q:>6.2f}")
    print(f"unknown fields: {profile['unknown']}")


def parse_band(text):
    try:
        freq, gain, q = (float(x) for x in text.split(":"))
    except ValueError:
        raise argparse.ArgumentTypeError(f"'{text}' is not FREQ:GAIN:Q")
    return freq, gain, q


def selftest():
    payload = from_qr_text(SAMPLE)
    profile = parse(payload)
    ok = profile["name"] == "Custom 1" and len(profile["bands"]) == NUM_BANDS
    for (f, g, q), (ef, eg, eq) in zip(profile["bands"], SAMPLE_BANDS):
        ok = ok and abs(f - ef) < 1e-3 and abs(g - eg) < 1e-3 and abs(q - eq) < 1e-3
    print(f"{'ok  ' if ok else 'FAIL'}  sample decodes to the bands set in the app")
    rebuilt = build(profile["bands"], profile["name"])
    same = rebuilt == payload
    print(f"{'ok  ' if same else 'FAIL'}  encoding those bands gives back the app's exact bytes")
    again = parse(from_qr_text(to_qr_text(rebuilt)))
    trip = again["bands"] == profile["bands"] and again["name"] == profile["name"]
    print(f"{'ok  ' if trip else 'FAIL'}  QR text round-trips")
    sys.exit(0 if ok and same and trip else 1)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--selftest", action="store_true", help="check against the captured sample")
    sub = ap.add_subparsers(dest="cmd")
    dec = sub.add_parser("decode", help="print the bands in a share code")
    dec.add_argument("code", help="QR image, .txt file, or the QR text itself")
    enc = sub.add_parser("encode", help="make a share code from bands")
    enc.add_argument("--name", default="TrypT", help="profile name shown in Nothing X")
    enc.add_argument("--band", type=parse_band, action="append", default=[], metavar="FREQ:GAIN:Q")
    enc.add_argument("--json", type=Path, help="bands from a JSON file instead of --band")
    enc.add_argument("--qr", type=Path, help="also write the QR code as a PNG (needs segno)")
    args = ap.parse_args()

    if args.selftest:
        selftest()
    elif args.cmd == "decode":
        print_profile(parse(from_qr_text(read_input(args.code))))
    elif args.cmd == "encode":
        name, bands = args.name, args.band
        if args.json:
            spec = json.loads(args.json.read_text(encoding="utf-8"))
            name = spec.get("name", name)
            bands = [(b["freq"], b["gain"], b["q"]) for b in spec["bands"]]
        warn_out_of_range(bands)
        try:
            text = to_qr_text(build(bands, name))
        except ValueError as e:
            sys.exit(f"error: {e}")
        print(text)
        if args.qr:
            try:
                import segno
            except ImportError:
                sys.exit("writing a QR image needs: pip install segno")
            segno.make(text, error="m").save(str(args.qr), scale=10, border=4)
            print(f"wrote {args.qr}", file=sys.stderr)
    else:
        ap.print_help()


if __name__ == "__main__":
    main()
