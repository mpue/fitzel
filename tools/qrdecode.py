#!/usr/bin/env python3
"""Decode the codes qrcheck wrote with a reader that is not ours.

    build/linux/bin/qrcheck <dir>
    python3 tools/qrdecode.py <dir>

Reads <dir>/qr-manifest.tsv (file, expected text as hex) and decodes every PNG
with zxing-cpp (pip install zxing-cpp pillow); exit 1 when one does not read
or reads as something else. qrcheck's own reader shares the encoder's tables;
this one shares nothing with it.
"""
import sys
from pathlib import Path

import zxingcpp
from PIL import Image


def main() -> int:
    d = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
    bad = 0
    rows = [l.split("\t") for l in (d / "qr-manifest.tsv").read_text().splitlines() if l]
    for name, want_hex in rows:
        want = bytes.fromhex(want_hex)
        hits = zxingcpp.read_barcodes(Image.open(d / name), formats=zxingcpp.BarcodeFormat.QRCode)
        got = hits[0].bytes if hits else None
        ok = got == want
        bad += 0 if ok else 1
        if not ok:
            print(f"FAIL  {name}: {'nothing read' if got is None else got[:60]!r}")
    print(f"{len(rows) - bad} of {len(rows)} codes read as written")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
