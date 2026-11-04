import argparse
import pathlib
import struct
import zlib


def chunk(kind: bytes, data: bytes) -> bytes:
    return (
        struct.pack(">I", len(data))
        + kind
        + data
        + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    )


parser = argparse.ArgumentParser(
    description="Convert native renderer RGBA output to PNG."
)
parser.add_argument("input", type=pathlib.Path)
parser.add_argument("output", type=pathlib.Path)
parser.add_argument("--width", type=int, default=1920)
parser.add_argument("--height", type=int, default=1080)
args = parser.parse_args()
pixels = args.input.read_bytes()
if args.width <= 0 or args.height <= 0 or len(pixels) != args.width * args.height * 4:
    parser.error("RGBA byte count does not match the image dimensions.")
stride = args.width * 4
scanlines = b"".join(
    b"\0" + pixels[y * stride : (y + 1) * stride] for y in range(args.height)
)
args.output.write_bytes(
    b"\x89PNG\r\n\x1a\n"
    + chunk(b"IHDR", struct.pack(">2I5B", args.width, args.height, 8, 6, 0, 0, 0))
    + chunk(b"IDAT", zlib.compress(scanlines))
    + chunk(b"IEND", b"")
)
