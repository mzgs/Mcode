#!/bin/sh
set -eu

# Regenerate resources/app.icns (macOS) and resources/app.ico (Windows)
# from resources/app.png (expected to be a square PNG, ideally 1024x1024).
# Requires macOS (sips, iconutil) and python3.

SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)"
if [ "$#" -ge 1 ]; then
    case "$1" in
        /*) SRC="$1" ;;
        *)  SRC="$(pwd)/$1" ;;
    esac
else
    SRC="$SCRIPT_DIR/app.png"
fi

cd -- "$SCRIPT_DIR"

if [ ! -f "$SRC" ]; then
    echo "Source icon not found: $SRC" >&2
    exit 1
fi

if ! command -v sips >/dev/null 2>&1 || ! command -v iconutil >/dev/null 2>&1; then
    echo "This script requires macOS 'sips' and 'iconutil'." >&2
    exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
    echo "This script requires 'python3'." >&2
    exit 1
fi

TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/app-icons.XXXXXX")"
trap 'rm -rf "$TMP_DIR"' EXIT

ICONSET="$TMP_DIR/app.iconset"
mkdir -p "$ICONSET"

resize_png() {
    sips -s format png -z "$1" "$1" "$SRC" --out "$2" >/dev/null
}

resize_png 16   "$ICONSET/icon_16x16.png"
resize_png 32   "$ICONSET/icon_16x16@2x.png"
resize_png 32   "$ICONSET/icon_32x32.png"
resize_png 64   "$ICONSET/icon_32x32@2x.png"
resize_png 128  "$ICONSET/icon_128x128.png"
resize_png 256  "$ICONSET/icon_128x128@2x.png"
resize_png 256  "$ICONSET/icon_256x256.png"
resize_png 512  "$ICONSET/icon_256x256@2x.png"
resize_png 512  "$ICONSET/icon_512x512.png"
resize_png 1024 "$ICONSET/icon_512x512@2x.png"

iconutil -c icns "$ICONSET" -o app.icns

ICO_DIR="$TMP_DIR/ico"
mkdir -p "$ICO_DIR"
for sz in 16 32 48 64 128 256; do
    resize_png "$sz" "$ICO_DIR/${sz}.png"
done

python3 - "$ICO_DIR" "app.ico" <<'PY'
import os
import struct
import sys
import zlib

ico_dir, out_path = sys.argv[1], sys.argv[2]
sizes = [16, 32, 48, 64, 128, 256]

def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c

def png_to_dib(png_bytes):
    """Decode an 8-bit RGB/RGBA non-interlaced PNG into a 32-bit BGRA ICO DIB."""
    if png_bytes[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    pos = 8
    w = h = bd = ct = interlace = 0
    idat = bytearray()
    while pos + 8 <= len(png_bytes):
        length = struct.unpack(">I", png_bytes[pos:pos + 4])[0]
        ctype = png_bytes[pos + 4:pos + 8]
        chunk = png_bytes[pos + 8:pos + 8 + length]
        pos += 12 + length
        if ctype == b"IHDR":
            w, h, bd, ct, _, _, interlace = struct.unpack(">IIBBBBB", chunk)
        elif ctype == b"IDAT":
            idat.extend(chunk)
        elif ctype == b"IEND":
            break
    if bd != 8 or ct not in (2, 6) or interlace != 0:
        return None
    bpp = 4 if ct == 6 else 3
    raw = zlib.decompress(bytes(idat))
    stride = w * bpp
    rows = []
    prev = bytearray(stride)
    idx = 0
    for _ in range(h):
        flt = raw[idx]
        idx += 1
        cur = bytearray(raw[idx:idx + stride])
        idx += stride
        if flt == 1:
            for i in range(bpp, stride):
                cur[i] = (cur[i] + cur[i - bpp]) & 0xFF
        elif flt == 2:
            for i in range(stride):
                cur[i] = (cur[i] + prev[i]) & 0xFF
        elif flt == 3:
            for i in range(stride):
                left = cur[i - bpp] if i >= bpp else 0
                cur[i] = (cur[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif flt == 4:
            for i in range(stride):
                left = cur[i - bpp] if i >= bpp else 0
                up = prev[i]
                ul = prev[i - bpp] if i >= bpp else 0
                cur[i] = (cur[i] + paeth(left, up, ul)) & 0xFF
        elif flt != 0:
            return None
        rows.append(cur)
        prev = cur

    # BITMAPINFOHEADER (40 bytes): height is 2*h (XOR + AND masks)
    xor_buf = bytearray()
    and_row_bytes = ((w + 31) // 32) * 4
    and_buf = bytearray()
    for y in range(h - 1, -1, -1):
        row = rows[y]
        mask_row = bytearray(and_row_bytes)
        for x in range(w):
            off = x * bpp
            r, g, b = row[off], row[off + 1], row[off + 2]
            a = row[off + 3] if bpp == 4 else 255
            xor_buf.extend((b, g, r, a))
            if a == 0:
                mask_row[x >> 3] |= 0x80 >> (x & 7)
        and_buf.extend(mask_row)

    header = struct.pack(
        "<IIIHHIIIIII",
        40, w, h * 2, 1, 32, 0, len(xor_buf) + len(and_buf), 0, 0, 0, 0
    )
    return header + bytes(xor_buf) + bytes(and_buf)

entries = []
for sz in sizes:
    with open(os.path.join(ico_dir, f"{sz}.png"), "rb") as f:
        png_data = f.read()
    # Embed 256px as PNG (Windows Vista+ standard) and smaller sizes as 32-bit DIB for max compatibility.
    data = png_data if sz >= 256 else (png_to_dib(png_data) or png_data)
    entries.append((sz, data))

header = struct.pack("<HHH", 0, 1, len(entries))
offset = 6 + 16 * len(entries)
dir_entries = bytearray()
payload = bytearray()
for sz, data in entries:
    dim = 0 if sz >= 256 else sz
    dir_entries.extend(struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset))
    payload.extend(data)
    offset += len(data)

with open(out_path, "wb") as f:
    f.write(header)
    f.write(dir_entries)
    f.write(payload)
PY

echo "Generated app.icns and app.ico from $SRC"
SRC_REAL="$(cd -- "$(dirname -- "$SRC")" && pwd)/$(basename -- "$SRC")"
if [ "$SRC_REAL" != "$SCRIPT_DIR/app.png" ]; then
    echo "Note: resources/app.png (used on Linux) was not updated; copy your icon there too if replacing the app icon."
fi
