#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Decode Nothing X EQ share QR codes and diff each one against a baseline.

The experiment: export a flat EQ as the baseline, then change one value per
export. Every byte (or token) that differs from the baseline is then that one
value, and seeing it as several number types usually gives away the scaling:
+3 dB showing up as an int16 delta of 300 means gain is stored x100.

    qr_diff.py flat.png b1_gain+3.png b1_gain+6.png b1_q2.png
    qr_diff.py --save decoded/ flat.png b1_gain+3.png
    qr_diff.py flat.txt b1_gain+3.txt       # strings you already decoded
    qr_diff.py --selftest                   # demo on a made-up payload

The first input is the baseline. A label is the file name without extension,
so name files after what you changed. Images need the zxing-cpp package
(pip install zxing-cpp pillow); .txt files are read as the raw QR string.
"""

import argparse
import base64
import binascii
import gzip
import json
import re
import struct
import sys
import zlib
from pathlib import Path
from urllib.parse import parse_qsl, unquote, urlparse


def decode_image(path):
    try:
        import zxingcpp
        from PIL import Image
    except ImportError:
        sys.exit("decoding images needs: pip install zxing-cpp pillow "
                 "(or decode them yourself and pass .txt files)")
    results = zxingcpp.read_barcodes(Image.open(path))
    if not results:
        sys.exit(f"{path}: no QR code found")
    if len(results) > 1:
        print(f"note: {path} has {len(results)} codes, using the first")
    return results[0].text


HEX = re.compile(r"^[0-9a-fA-F]+$")
B64 = re.compile(r"^[A-Za-z0-9+/_-]+={0,2}$")


def unwrap(raw):
    """Peel the layers off a QR string. Returns (data, layers), where data is
    bytes, and layers says what was peeled, outermost first."""
    layers = []
    text = raw.strip()

    if re.match(r"^[A-Za-z][A-Za-z0-9+.-]*://", text):
        url = urlparse(text)
        fields = parse_qsl(url.query, keep_blank_values=True)
        if url.fragment:
            fields.append(("#fragment", url.fragment))
        if not fields and url.path.strip("/"):
            fields.append(("last path segment", url.path.rstrip("/").rsplit("/", 1)[-1]))
        if fields:
            # The payload is almost always the longest field; the rest are
            # things like a version or a share id.
            name, value = max(fields, key=lambda f: len(f[1]))
            others = [f[0] for f in fields if f[0] != name]
            layers.append(f"url {url.scheme}://{url.netloc}{url.path}, field '{name}'"
                          + (f" (also: {', '.join(others)})" if others else ""))
            text = unquote(value)

    data = None
    if len(text) >= 8 and len(text) % 2 == 0 and HEX.match(text):
        data = bytes.fromhex(text)
        layers.append("hex")
    elif len(text) >= 8 and B64.match(text):
        padded = text.replace("-", "+").replace("_", "/")
        padded += "=" * (-len(padded) % 4)
        try:
            data = base64.b64decode(padded, validate=True)
            layers.append("base64url" if re.search(r"[-_]", text) else "base64")
        except (binascii.Error, ValueError):
            data = None
    if data is None:
        data = text.encode("utf-8")
        layers.append("plain text")

    for _ in range(3):
        for name, fn in (("zlib", zlib.decompress),
                         ("gzip", gzip.decompress),
                         ("raw deflate", lambda d: zlib.decompress(d, -15))):
            try:
                out = fn(data)
            except (zlib.error, OSError, EOFError):
                continue
            if out:
                data = out
                layers.append(name)
                break
        else:
            break
    return data, layers


def as_text(data):
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError:
        return None
    return text if all(c.isprintable() or c in "\r\n\t" for c in text) else None


def flatten(obj, prefix=""):
    if isinstance(obj, dict):
        for k, v in obj.items():
            yield from flatten(v, f"{prefix}.{k}" if prefix else str(k))
    elif isinstance(obj, list):
        for i, v in enumerate(obj):
            yield from flatten(v, f"{prefix}[{i}]")
    else:
        yield prefix, obj


def diff_json(base, new):
    a, b = dict(flatten(base)), dict(flatten(new))
    changed = [(k, a.get(k), b.get(k)) for k in sorted(set(a) | set(b)) if a.get(k) != b.get(k)]
    if not changed:
        print("    no fields changed")
    for key, old, now in changed:
        delta = ""
        if isinstance(old, (int, float)) and isinstance(now, (int, float)):
            delta = f"   (delta {now - old:+g})"
        print(f"    {key}: {old!r} -> {now!r}{delta}")


def diff_tokens(base, new):
    split = re.compile(r"(-?[0-9]+(?:\.[0-9]+)?|[A-Za-z_]+)")
    a, b = split.findall(base), split.findall(new)
    if len(a) != len(b):
        print(f"    token count differs ({len(a)} -> {len(b)}); compare by eye:")
        print(f"      base: {base}")
        print(f"      new:  {new}")
        return
    changed = [(i, x, y) for i, (x, y) in enumerate(zip(a, b)) if x != y]
    if not changed:
        print("    no tokens changed")
    for i, x, y in changed:
        delta = ""
        try:
            delta = f"   (delta {float(y) - float(x):+g})"
        except ValueError:
            pass
        print(f"    token {i}: {x} -> {y}{delta}")


TYPES = (("<b", "i8"), ("<B", "u8"), ("<h", "i16le"), (">h", "i16be"),
         ("<H", "u16le"), (">H", "u16be"), ("<i", "i32le"), (">i", "i32be"),
         ("<f", "f32le"), (">f", "f32be"))


def plausible_float(v):
    # Integer bytes read as a float come out as denormals or huge exponents;
    # a real EQ value is neither.
    return v == 0 or 1e-4 <= abs(v) <= 1e6


def readings(base, new, lo, hi):
    """Every field that could hold the changed bytes lo..hi-1: it must cover
    all of them, and be at most twice their width (2 bytes for a 1-byte
    change), or it is mostly bytes that did not change."""
    out = []
    width = hi - lo
    for fmt, name in TYPES:
        size = struct.calcsize(fmt)
        if size < width or size > max(2, width * 2):
            continue
        for off in range(hi - size, lo + 1):
            if off < 0 or off % size or off + size > min(len(base), len(new)):
                continue
            was = struct.unpack_from(fmt, base, off)[0]
            now = struct.unpack_from(fmt, new, off)[0]
            if name.startswith("f"):
                if plausible_float(was) and plausible_float(now):
                    out.append((off, f"{name} {was:.4g}->{now:.4g}"))
            else:
                out.append((off, f"{name} {was}->{now} ({now - was:+d})"))
    return out


def diff_bytes(base, new):
    if len(base) != len(new):
        print(f"    length differs: {len(base)} -> {len(new)} bytes "
              "(variable-length encoding; offsets past the first change may shift)")
    runs, start = [], None
    for i in range(min(len(base), len(new))):
        if base[i] != new[i] and start is None:
            start = i
        elif base[i] == new[i] and start is not None:
            runs.append((start, i))
            start = None
    if start is not None:
        runs.append((start, min(len(base), len(new))))
    if not runs:
        print("    no bytes changed")
    for lo, hi in runs:
        print(f"    bytes {lo}..{hi - 1}: {base[lo:hi].hex(' ')} -> {new[lo:hi].hex(' ')}")
        # A multi-byte field can change only its low byte, so a field may
        # start before the change; readings() tries each aligned start.
        by_offset = {}
        for off, text in readings(base, new, lo, hi):
            by_offset.setdefault(off, []).append(text)
        for off in sorted(by_offset):
            print(f"      @{off}: " + ", ".join(by_offset[off]))


def load(path):
    path = Path(path)
    if path.suffix.lower() == ".txt":
        return path.stem, path.read_text(encoding="utf-8").strip()
    return path.stem, decode_image(path)


def report(entries, save=None):
    decoded = []
    for label, raw in entries:
        data, layers = unwrap(raw)
        decoded.append((label, raw, data, layers))
        if save:
            save.mkdir(parents=True, exist_ok=True)
            (save / f"{label}.txt").write_text(raw + "\n", encoding="utf-8")
            (save / f"{label}.bin").write_bytes(data)

    label, raw, base, layers = decoded[0]
    print(f"baseline '{label}'")
    print(f"  raw:    {raw if len(raw) <= 160 else raw[:157] + '...'}")
    print(f"  layers: {' > '.join(layers)}")
    base_text = as_text(base)
    if base_text is None:
        print(f"  {len(base)} bytes: {base.hex(' ')}")
    else:
        print(f"  text:   {base_text if len(base_text) <= 400 else base_text[:397] + '...'}")

    for label, raw, data, layers in decoded[1:]:
        print(f"\n'{label}' vs baseline   [{' > '.join(layers)}]")
        text = as_text(data)
        if base_text is not None and text is not None:
            try:
                diff_json(json.loads(base_text), json.loads(text))
            except ValueError:
                diff_tokens(base_text, text)
        else:
            diff_bytes(base, data)


def selftest():
    """A made-up payload in the shape these often take: a packed band table,
    deflated, base64url'd and carried in a URL. Not Nothing's real format."""
    def payload(bands):
        body = bytes([1, len(bands)])
        for freq, gain, q in bands:
            body += struct.pack("<Hhh", freq, round(gain * 100), round(q * 100))
        packed = base64.urlsafe_b64encode(zlib.compress(body)).decode().rstrip("=")
        return f"https://example.invalid/eq?v=1&d={packed}"

    flat = [(f, 0.0, 1.0) for f in (60, 150, 400, 1000, 2400, 5000, 8000, 12000)]
    gain3 = [(60, 3.0, 1.0)] + flat[1:]
    q2 = [(60, 0.0, 2.0)] + flat[1:]
    print("selftest: band 1 is freq u16, gain i16 x100, Q i16 x100\n")
    report([("flat", payload(flat)), ("b1_gain+3", payload(gain3)), ("b1_q2", payload(q2))])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("inputs", nargs="*", help="QR images or .txt files; the first is the baseline")
    ap.add_argument("--save", type=Path, help="write each raw string and decoded payload here")
    ap.add_argument("--selftest", action="store_true", help="run on a synthetic payload")
    args = ap.parse_args()
    if args.selftest:
        selftest()
        return
    if len(args.inputs) < 2:
        ap.error("give a baseline and at least one changed export")
    report([load(p) for p in args.inputs], args.save)


if __name__ == "__main__":
    main()
