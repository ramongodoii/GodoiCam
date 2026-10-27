"""Gera windows/app/godoicam.ico (lente laranja sobre fundo escuro) sem dependências."""
import struct
import sys
import zlib


def render(size):
    bg, ring, core, dot = (22, 24, 29), (255, 122, 69), (244, 245, 247), (22, 24, 29)
    c = (size - 1) / 2
    rows = []
    for y in range(size):
        row = bytearray([0])
        for x in range(size):
            # Superamostragem 4x4 para bordas suaves.
            acc = [0, 0, 0, 0]
            for sy in range(4):
                for sx in range(4):
                    px, py = x + (sx + 0.5) / 4 - 0.5, y + (sy + 0.5) / 4 - 0.5
                    d = ((px - c) ** 2 + (py - c) ** 2) ** 0.5 / (size / 2)
                    dd = ((px - c - size * 0.08) ** 2 + (py - c + size * 0.08) ** 2) ** 0.5 / (size / 2)
                    corner = max(abs(px - c), abs(py - c)) / (size / 2)
                    if corner > 1:
                        col, a = bg, 0
                    elif d < 0.30:
                        col, a = (dot if dd < 0.09 else core), 255
                    elif d < 0.50:
                        col, a = bg, 255
                    elif d < 0.74:
                        col, a = ring, 255
                    else:
                        col, a = bg, 255
                    for i in range(3):
                        acc[i] += col[i]
                    acc[3] += a
            row += bytes([acc[0] // 16, acc[1] // 16, acc[2] // 16, acc[3] // 16])
        rows.append(bytes(row))
    raw = b"".join(rows)

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


def main(out):
    sizes = [16, 24, 32, 48, 64, 256]
    images = [render(s) for s in sizes]
    header = struct.pack("<HHH", 0, 1, len(sizes))
    offset = 6 + 16 * len(sizes)
    entries = b""
    for s, png in zip(sizes, images):
        entries += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(png), offset)
        offset += len(png)
    with open(out, "wb") as f:
        f.write(header + entries + b"".join(images))


if __name__ == "__main__":
    main(sys.argv[1])
