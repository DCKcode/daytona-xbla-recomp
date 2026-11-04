# Independent capture reference; the live decoder is C++ FrameBuilder.
import argparse
import functools
import hashlib
import json
import math
import mmap
import pathlib
import struct

WIDTH, HEIGHT = 1920, 1080
MULTISAMPLED = 0x10000000
# Verified internal shaders: a degenerate synchronization point and a
# position/color passthrough used for full-target rectangle clears.
SYNC_SHADER = 0xB6C9863F710683EC
CLEAR_SHADER = 0x1E6883FCCDE1F688
TILE_EDGES = (0, 160, 320, 480, 640, 800, 928, 1080)
LOCATIONS = (4, 5, 6, 7, 12, 13, 14, 15)

parser = argparse.ArgumentParser(
    description="Prepare a native Daytona draw capture for Vulkan replay."
)
parser.add_argument("capture", type=pathlib.Path)
parser.add_argument("output", type=pathlib.Path)
parser.add_argument(
    "--shaders", type=pathlib.Path, help="Defaults to the capture's parent directory."
)
args = parser.parse_args()
probe = args.shaders or args.capture.parent
mem = mmap.mmap(-1, 1 << 29)
pages = set()


def read_memory(address, size):
    if address < 0 or address + size > len(mem):
        raise ValueError("Source memory is outside the physical address space")
    if any(
        page not in pages
        for page in range(address // 4096, (address + size + 4095) // 4096)
    ):
        raise ValueError(f"Uncaptured source memory at {address:08X}")
    return mem[address : address + size]


@functools.cache
def metadata(shader, stage):
    data = json.loads((probe / f"{shader:016X}.{stage}.json").read_text())
    if data["memexport"] or data["unsupported_constants"]:
        raise ValueError(
            "Memory exports and dynamic, boolean or loop constants are unsupported"
        )
    return data


@functools.cache
def attributes(shader):
    bindings = metadata(shader, "vs")["vertex_bindings"]
    if not 1 <= len(bindings) <= len(LOCATIONS):
        raise ValueError("Unsupported vertex attribute count")
    for fetch, stride, offset, fmt, signed, integer in bindings:
        if not (0 <= fetch < 96 and stride > 0 and offset >= 0):
            raise ValueError("Invalid vertex binding")
        if (fmt == 6 and (signed or integer)) or (
            fmt != 6 and (not signed or not integer)
        ):
            raise ValueError("Unsupported vertex attribute interpretation")
        if fmt not in (6, 36, 37, 38, 57):
            raise ValueError(f"Unsupported vertex format {fmt}")
    return bindings


def vertex_value(registers, binding, index):
    fetch, stride, offset, fmt, _, _ = binding
    f0, f1 = registers[0x4800 + fetch * 2 : 0x4802 + fetch * 2]
    if f0 & 3 != 3:
        raise ValueError("Expected a vertex fetch constant")
    address = (f0 & ~3) + 4 * (index * stride + offset)
    if fmt == 6:
        return [x / 255 for x in swap(read_memory(address, 4), f1 & 3)]
    count = {36: 1, 37: 2, 38: 4, 57: 3}[fmt]
    raw = read_memory(address, count * 4)
    values = list(
        struct.unpack(
            f"<{count}f",
            b"".join(swap(raw[i : i + 4], f1 & 3) for i in range(0, len(raw), 4)),
        )
    )
    return values + [0.0] * (4 - count)


def swap(b, e):
    if e == 0:
        return b
    if e == 2:
        return b[::-1]
    if e == 1:
        return b[1::-1] + b[3:1:-1]
    if e == 3:
        return b[2:4] + b[0:2]
    raise ValueError(e)


def tiled(x, y, pitch, lb):
    ma = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (lb + 7)
    mi = ((x & 7) + ((y & 14) << 2)) << lb
    v = ma + ((mi & ~15) << 1) + (mi & 15) + ((y & 1) << 4)
    return (
        ((v & ~511) << 3)
        + ((y & 16) << 7)
        + ((v & 448) << 2)
        + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)
        + (v & 63)
    )


def rgb565(c):
    r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
    # BC endpoints expand by replicating high bits, as in the SDK decoder.
    return (
        (r << 3) | (r >> 2),
        (g << 2) | (g >> 4),
        (b << 3) | (b >> 2),
    )


def dxt(b, fmt):
    col = b if fmt == 18 else b[8:]
    c0, c1, bits = struct.unpack("<HHI", col)
    a = rgb565(c0) + (255,)
    z = rgb565(c1) + (255,)
    palette = [a, z]
    if c0 > c1 or fmt != 18:
        palette += [
            tuple((2 * a[i] + z[i]) // 3 for i in range(3)) + (255,),
            tuple((a[i] + 2 * z[i]) // 3 for i in range(3)) + (255,),
        ]
    else:
        palette += [tuple((a[i] + z[i]) // 2 for i in range(3)) + (255,), (0, 0, 0, 0)]
    pixels = [list(palette[(bits >> (2 * i)) & 3]) for i in range(16)]
    if fmt == 19:
        alpha = int.from_bytes(b[:8], "little")
        for i, pix in enumerate(pixels):
            pix[3] = ((alpha >> (i * 4)) & 15) * 17
    if fmt == 20:
        a0, a1 = b[:2]
        alpha = [a0, a1]
        if a0 > a1:
            alpha += [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)]
        else:
            alpha += [((5 - i) * a0 + i * a1) // 5 for i in range(1, 5)] + [0, 255]
        bits = int.from_bytes(b[2:8], "little")
        for i, pix in enumerate(pixels):
            pix[3] = alpha[(bits >> (i * 3)) & 7]
    return pixels


textures = []
texture_cache = {}
resolve_addresses = set()


def decode_texture_level(f, w, h, address, pitch, ox=0, oy=0):
    fmt, endian = f[1] & 63, (f[1] >> 6) & 3
    compressed = fmt in (18, 19, 20)
    block = 4 if compressed else 1
    bs = 8 if fmt == 18 else 16 if compressed else 2 if fmt == 10 else 4
    bw, bh = (w + block - 1) // block, (h + block - 1) // block
    is_tiled = bool(f[0] >> 31)
    rgba = bytearray(w * h * 4)
    swizzle = (f[3] >> 1) & 4095
    for by in range(bh):
        for bx in range(bw):
            off = (
                tiled(bx + ox, by + oy, pitch, bs.bit_length() - 1)
                if is_tiled
                else ((by + oy) * pitch + bx + ox) * bs
            )
            if bs == 2:
                word = swap(read_memory(address + (off & ~3), 4), endian)
                raw = word[off & 3 : (off & 3) + 2]
            else:
                raw = read_memory(address + off, bs)
                raw = b"".join(swap(raw[i : i + 4], endian) for i in range(0, bs, 4))
            # Xenos k_8_8 expands as RGGG before the fetch swizzle, matching
            # the SDK's host-format table rather than a Vulkan RG default.
            pixels = (
                dxt(raw, fmt)
                if compressed
                else [list(raw) if fmt != 10 else [raw[0], raw[1], raw[1], raw[1]]]
            )
            for j, pixel in enumerate(pixels):
                x, y = (bx * 4 + j % 4, by * 4 + j // 4) if compressed else (bx, by)
                if x >= w or y >= h:
                    continue
                channels = pixel + [0, 255, 0, 0]
                rgba[(y * w + x) * 4 : (y * w + x + 1) * 4] = bytes(
                    channels[(swizzle >> (i * 3)) & 7] for i in range(4)
                )
    return rgba


def texture(registers, binding):
    slot, mag, min_, mip, aniso = binding
    f = registers[0x4800 + slot * 6 : 0x4806 + slot * 6]
    mag = (f[3] >> 19) & 3 if mag == 3 else mag
    min_ = (f[3] >> 21) & 3 if min_ == 3 else min_
    mip = (f[3] >> 23) & 3 if mip == 3 else mip
    aniso = (f[3] >> 25) & 7 if aniso == 7 else aniso
    clamp_u, clamp_v = (f[0] >> 10) & 7, (f[0] >> 13) & 7
    border = f[5] & 3 if 6 in (clamp_u, clamp_v) else 0
    if mag not in (0, 1) or min_ not in (0, 1) or aniso not in (0, 1):
        raise ValueError("Unsupported texture filtering")
    if clamp_u not in (0, 1, 2, 6) or clamp_v not in (0, 1, 2, 6) or border > 1:
        raise ValueError("Unsupported texture address or border mode")
    if mip not in (0, 1, 2):
        raise ValueError("Unsupported mip filter")
    sampler = mag | (min_ << 1) | (clamp_u << 2) | (clamp_v << 5) | (border << 8)
    if f[0] & 3 != 2 or (f[5] >> 9) & 3 != 1 or f[1] & (1 << 10):
        raise ValueError("Only unstacked 2D textures are supported")
    if (f[0] >> 2) & 255 or f[3] & 1 or (f[3] >> 13) & 63:
        raise ValueError(
            "Signed/integer textures and exponent adjustment are unsupported"
        )
    fmt, endian = f[1] & 63, (f[1] >> 6) & 3
    if fmt not in (6, 10, 18, 19, 20):
        raise ValueError(f"Unsupported texture format {fmt}")
    address = f[1] & 0x1FFFF000
    w, h = (f[2] & 8191) + 1, ((f[2] >> 13) & 8191) + 1
    if f[5] & (1 << 11) and min(w, h) <= 16:
        raise ValueError("Packed base mip levels are unsupported")
    if f[4] & 0x3C:
        raise ValueError("A texture with the base mip disabled is unsupported")
    if address in resolve_addresses:
        swizzle = (f[3] >> 1) & 4095
        if (
            address != first_resolve
            or (w, h, fmt) != (WIDTH, HEIGHT, 6)
            or swizzle not in (0x60A, 0xA0A)
        ):
            raise ValueError("Unsupported native resolve texture")
        key = (tuple(f), sampler, "resolve")
        if key not in texture_cache:
            texture_cache[key] = len(textures)
            # The resolve swaps R/B and the fetch swaps them back. Preserve
            # the fetch's optional constant-one alpha in the native view.
            textures.append((w, h, b"", 3 if swizzle == 0xA0A else 1, sampler, 1))
        return texture_cache[key]
    compressed = fmt in (18, 19, 20)
    block = 4 if compressed else 1
    bs = 8 if fmt == 18 else 16 if compressed else 2 if fmt == 10 else 4
    bw, bh = (w + block - 1) // block, (h + block - 1) // block
    pitch = (((f[0] >> 22) & 511) * 32) // block
    is_tiled = bool(f[0] >> 31)
    if is_tiled:
        pitch = (pitch + 31) & ~31
    if pitch < bw:
        raise ValueError("Texture pitch is smaller than its width")
    span = pitch * ((bh + 31) & ~31) * bs if is_tiled else ((bh - 1) * pitch + bw) * bs
    levels = [[w, h, address, pitch, 0, 0]]
    sources = [[address, span]]
    if mip != 2 and (f[4] >> 6) & 15 and f[5] >> 12:
        name = "texture_" + "_".join(f"{word:08X}" for word in f) + ".json"
        layout = json.loads((probe / name).read_text())
        levels, sources = layout["levels"], layout["sources"]
    max_level = len(levels) - 1
    lod_bias = (f[4] >> 12) & 1023
    sampler |= (int(mip == 1) << 9) | (max_level << 10) | (lod_bias << 14)
    # Texture uploads can change without changing their descriptor.
    digest = hashlib.sha256()
    for source, size in sources:
        digest.update(read_memory(source, size))
    key = (tuple(f), sampler, digest.digest())
    if key in texture_cache:
        return texture_cache[key]
    rgba = b"".join(decode_texture_level(f, *level) for level in levels)
    index = len(textures)
    texture_cache[key] = index
    textures.append((w, h, rgba, 0, sampler, len(levels)))
    print("texture", index, w, h, fmt)
    return index


def constants(registers, shader, stage):
    base = 0x4000 if stage == "vs" else 0x4400
    result = bytearray(4096)
    for index in metadata(shader, stage)["float_constants"]:
        # Match the SDK's fixed helper value rather than the guest register.
        if index == 255:
            struct.pack_into("<4f", result, index * 16, 0, 1, 0, 1)
            continue
        struct.pack_into(
            "<4I",
            result,
            index * 16,
            *registers[base + index * 4 : base + index * 4 + 4],
        )
    return result


def scissor(registers):
    tl, br = registers[0x2081:0x2083]
    return tl & 32767, (tl >> 16) & 32767, br & 32767, (br >> 16) & 32767


def resolve_rectangle(registers, top, bottom):
    f0, f1 = registers[0x4800:0x4802]
    if f0 & 3 != 3 or ((f1 >> 2) & 0xFFFFFF) != 6 or not registers[0x2302] & 1:
        raise ValueError("Unsupported resolve vertex format or pixel center")
    raw = read_memory(f0 & ~3, 24)
    coordinates = struct.unpack(
        "<6f", b"".join(swap(raw[i : i + 4], f1 & 3) for i in range(0, 24, 4))
    )
    if coordinates != (
        0.0,
        float(top),
        float(WIDTH),
        float(top),
        float(WIDTH),
        float(bottom),
    ):
        raise ValueError("Unsupported resolve rectangle")


def initial_ui_clear(header, registers, vs, ps):
    # This game's first UI draw copies the full resolved image with replacement
    # blending. It needs no guest clear; reject partial first-use draws instead
    # of inventing contents for uncovered pixels.
    if (vs, ps) != (0xFA44706CA015263D, 0x7ABDFEFBCADB4BF9):
        raise ValueError(
            "UI without a clear must start with the verified full-screen copy"
        )
    if header[1:3] != (13, 4) or registers[0x2201] != 0x10001 or registers[0x2200] & 2:
        raise ValueError(
            "Initial UI copy must overwrite the output without depth testing"
        )
    if scissor(registers) != (0, 0, WIDTH, HEIGHT) or registers[0x2104] != 15:
        raise ValueError("Initial UI copy must cover the whole output")
    viewport = struct.unpack("<6f", struct.pack("<6I", *registers[0x210F:0x2115]))
    if (
        viewport != (960.0, 960.0, -540.0, 540.0, 1.0, 0.0)
        or registers[0x2102]
        or registers[0x2205] & 3
    ):
        raise ValueError("Unsupported initial UI viewport, index offset or culling")
    bindings = attributes(vs)
    positions = [vertex_value(registers, bindings[0], i) for i in range(4)]
    if {(p[0], p[1], p[2]) for p in positions} != {
        (0.0, 0.0, 0.0),
        (0.0, 1.0, 0.0),
        (1.0, 0.0, 0.0),
        (1.0, 1.0, 0.0),
    }:
        raise ValueError("Unsupported initial UI copy geometry")
    expected_matrix = (
        2.0,
        0.0,
        0.0,
        0.0,
        0.0,
        2.0,
        0.0,
        0.0,
        0.0,
        0.0,
        -1.0,
        0.0,
        -1.0,
        -1.0,
        0.0,
        1.0,
    )
    matrix = struct.unpack("<16f", struct.pack("<16I", *registers[0x4120:0x4130]))
    if matrix != expected_matrix:
        raise ValueError("Unsupported initial UI projection")
    return (0.0, 0.0, 0.0, 0.0, 1.0)


def events():
    with (args.capture / "events.bin").open("rb") as source:

        def read(size):
            data = source.read(size)
            if len(data) != size:
                raise ValueError("Truncated native capture")
            return data

        if struct.unpack("<3I", read(12)) != (0x44544350, 3, 0x5003):
            raise ValueError(
                "Capture again with the current probe; this capture format is unsupported"
            )
        while prefix := source.read(4):
            header = struct.unpack("<8I", prefix + read(28))
            if header[0] == 3:
                address, size = header[1:3]
                if size != 4096 or address % 4096 or address + size > len(mem):
                    raise ValueError("Invalid captured memory page")
                mem[address : address + size] = read(size)
                pages.add(address // 4096)
            elif header[0] in (1, 2):
                registers = struct.unpack(f"<{0x5003}I", read(0x5003 * 4))
                yield header, registers, *struct.unpack("<2Q", read(16))
            else:
                raise ValueError("Unknown native capture event")


ui_draws = []
scene_draws = [[] for _ in range(7)]
clears = [None] * 8
world_pass = False
seen_ui = False
tile = 0
final_copy = False
for hd, r, vs, ps in events():
    samples = (r[0x2000] >> 16) & 3
    if samples not in (0, 2):
        raise ValueError("Unsupported sample count")
    target = int(samples == 2)
    if hd[0] == 2:
        if target:
            if not world_pass or tile >= 7 or seen_ui:
                raise ValueError("Unexpected world resolve")
            resolve_rectangle(r, *TILE_EDGES[tile : tile + 2])
            if tile and r[0x2319] != first_resolve + WIDTH * TILE_EDGES[tile] * 4:
                raise ValueError(
                    "World resolves are not contiguous rows of one texture"
                )
            if not tile:
                first_resolve = r[0x2319]
            if tile and len(scene_draws[tile]) != len(scene_draws[0]):
                raise ValueError("EDRAM tiles contain different draw counts")
            if r[0x231A] != 0x4380780 or r[0x231B] != 0x1000300:
                raise ValueError("Unsupported world resolve dimensions or format")
            expected = 0x100060 if tile == 6 else 0x100360
            if r[0x2318] != expected:
                raise ValueError("Unsupported resolve operation or clear flags")
            if tile < 6:
                if r[0x231D] != 0xFFFFFF00 or r[0x231E] != r[0x231F]:
                    raise ValueError("Unsupported inter-tile depth or wide color clear")
                clears[tile + 2] = (
                    *((r[0x231E] >> (i * 8) & 255) / 255.0 for i in range(4)),
                    1.0,
                )
            resolve_addresses.add(r[0x2319])
            tile += 1
        else:
            if world_pass and tile != 7:
                raise ValueError("Incomplete world resolve sequence")
            resolve_rectangle(r, 0, HEIGHT)
            if final_copy or not seen_ui or r[0x2318] != 0x100000:
                raise ValueError("Unsupported final output copy")
            final_copy = True
        continue
    if vs == SYNC_SHADER:
        if hd[1:3] != (1, 1) or metadata(vs, "vs")["vertex_bindings"]:
            raise ValueError("Unexpected synchronization draw")
        continue
    if final_copy:
        raise ValueError("Drawing after the output copy is unsupported")
    if hd[3]:
        raise ValueError("Indexed draws are unsupported until captured and verified")
    if vs == CLEAR_SHADER:
        if target and seen_ui:
            raise ValueError("Only one world pass followed by UI is supported")
        if hd[1:3] != (8, 3) or ps != 0xA4A965C189287B99 or r[0x2200] != 0x76:
            raise ValueError("Unsupported clear draw")
        if (target and (tile or scene_draws[0])) or (not target and seen_ui):
            raise ValueError("A clear after drawing has started is unsupported")
        bindings = attributes(vs)
        if [a[3] for a in bindings] != [57, 38] or r[0x2104] != 0xFFFF:
            raise ValueError("Unsupported clear vertex format or write mask")
        values = [vertex_value(r, bindings[1], i) for i in range(3)]
        if any(v != values[0] for v in values[1:]):
            raise ValueError("Clear rectangle has varying colors")
        positions = [vertex_value(r, bindings[0], i) for i in range(3)]
        clear_height = TILE_EDGES[1] if target else HEIGHT
        expected_positions = {
            (0.0, 0.0),
            (float(WIDTH), 0.0),
            (float(WIDTH), float(clear_height)),
        }
        if (
            {(p[0], p[1]) for p in positions} != expected_positions
            or r[0x2206] != 0x300
            or r[0x2080] != 0
        ):
            raise ValueError("Unsupported clear rectangle or coordinate convention")
        depth = positions[0][2]
        if any(p[2] != depth for p in positions) or not 0 <= depth <= 1:
            raise ValueError("Unsupported depth clear")
        clear = (*values[0], depth)
        if not all(math.isfinite(x) for x in clear):
            raise ValueError("Non-finite clear value")
        clears[target] = clear
        world_pass |= bool(target)
        continue
    if r[0x2206] != 0x43F or r[0x2204] != 0x80000:
        raise ValueError("Unsupported position or clip convention")
    if r[0x2200] & 1 or r[0x2202] & 0x18:
        raise ValueError("Stencil, alpha test and alpha-to-coverage are unsupported")
    if target:
        if seen_ui or not world_pass or tile >= 7:
            raise ValueError("Only one world pass followed by UI is supported")
        top, bottom = TILE_EDGES[tile : tile + 2]
        if scissor(r) != (0, top, WIDTH, bottom) or r[0x2080] != (
            ((-top) & 32767) << 16
        ):
            raise ValueError("Unsupported world tile scissor or window offset")
        rect = (0, 0, WIDTH, HEIGHT)
    else:
        if world_pass and tile != 7:
            raise ValueError("UI started before the world resolve completed")
        if r[0x2080]:
            raise ValueError("UI window offsets are unsupported")
        if not seen_ui and clears[0] is None:
            clears[0] = initial_ui_clear(hd, r, vs, ps)
        seen_ui = True
        rect = scissor(r)
    bindings = attributes(vs)
    indices = [(i + r[0x2102]) & 0xFFFFFF for i in range(hd[2])]
    triangles = []
    if hd[1] == 4 and len(indices) % 3 == 0:
        triangles = indices
    elif hd[1] == 6:
        for i in range(2, len(indices)):
            triangles.extend(
                (indices[i - 2], indices[i - 1], indices[i])
                if i % 2 == 0
                else (indices[i - 1], indices[i - 2], indices[i])
            )
    elif hd[1] == 13 and len(indices) % 4 == 0:
        for i in range(0, len(indices), 4):
            # Match the SDK's 0, 1, 3, 2 triangle strip, including its diagonal.
            triangles.extend(
                [
                    indices[i],
                    indices[i + 1],
                    indices[i + 3],
                    indices[i + 1],
                    indices[i + 2],
                    indices[i + 3],
                ]
            )
    else:
        raise ValueError("Unsupported primitive topology")
    vertices = b"".join(
        struct.pack("<4f", *vertex_value(r, binding, index))
        for index in triangles
        for binding in bindings
    )
    slots = metadata(ps, "ps")["texture_slots"]
    if slots not in ([], [0], [0, 1]) or metadata(vs, "vs")["texture_slots"]:
        raise ValueError("Only pixel-shader 2D texture slots 0 and 1 are supported")
    samplers = {}
    for binding in metadata(ps, "ps")["sampler_bindings"]:
        slot = binding[0]
        if slot in samplers and samplers[slot] != binding:
            raise ValueError(
                "Multiple sampler configurations for one slot are unsupported"
            )
        samplers[slot] = binding
    if sorted(samplers) != slots:
        raise ValueError("Texture and sampler binding metadata disagree")
    tex = ([texture(r, samplers[slot]) for slot in slots] + [0, 0])[:2]
    if target and any(textures[index][3] for index in tex):
        raise ValueError("World feedback from a resolve texture is unsupported")
    viewport = struct.unpack("<6f", struct.pack("<6I", *r[0x210F:0x2115]))
    header = struct.pack(
        "<2Q6I6f6I",
        vs,
        ps,
        len(triangles),
        len(bindings),
        r[0x2201],
        r[0x2200],
        r[0x2104],
        r[0x2205] | (MULTISAMPLED if target else 0),
        *viewport,
        *rect,
        *tex,
    )
    shared = bytearray(512)
    # shader_common.h packs 2D descriptor indices at c0, samplers at c12.
    struct.pack_into("<2I", shared, 0, 0, 1)
    struct.pack_into("<2I", shared, 192, 0, 1)
    # Match the SDK: unsigned 8_8 with XXXY swizzle is gamma-encoded
    # luminance plus linear coverage. Decode after filtering in the shader.
    gamma_flags = []
    for slot in range(2):
        f = r[0x4800 + slot * 6 : 0x4806 + slot * 6]
        gamma_flags.append(
            int(slot in slots and (f[1] & 63) == 10 and ((f[3] >> 1) & 4095) == 0x200)
        )
    struct.pack_into("<2I", shared, 288, *gamma_flags)
    draw = (
        header
        + struct.pack(f"<{len(bindings)}I", *LOCATIONS[: len(bindings)])
        + constants(r, vs, "vs")
        + constants(r, ps, "ps")
        + shared
        + vertices
    )
    if target:
        scene_draws[tile].append(draw)
    else:
        ui_draws.append(draw)

# Draw order is preserved at every pixel. Identical tile versions can share a
# full-frame draw; changed versions retain their original non-overlapping bands.
# Only the validated tile scissor/window offset above is normalized away.
draws = []
for versions in zip(*scene_draws, strict=True):
    if all(draw == versions[0] for draw in versions[1:]):
        draws.append(versions[0])
    else:
        for index, draw in enumerate(versions):
            rect = struct.pack(
                "<4I", 0, TILE_EDGES[index], WIDTH, TILE_EDGES[index + 1]
            )
            draws.append(draw[:64] + rect + draw[80:])
draws.extend(ui_draws)
if not seen_ui or not final_copy or world_pass and tile != 7:
    raise ValueError("A complete frame with a final UI/output pass is required")
if clears[0] is None or world_pass and any(c is None for c in clears[1:]):
    raise ValueError("A target was used before an explicit clear")
args.output.mkdir(parents=True, exist_ok=True)
gamma = (args.capture / "gamma.rgb").read_bytes()
if len(gamma) != 256 * 3:
    raise ValueError("Invalid captured display gamma table")
with (args.output / "frame.bin").open("wb") as output:
    output.write(
        struct.pack(
            "<7I40f",
            0x314E5444,
            4,
            WIDTH,
            HEIGHT,
            len(draws),
            len(textures),
            int(world_pass),
            *(
                value
                for clear in clears
                for value in (clear or (0.0, 0.0, 0.0, 0.0, 1.0))
            ),
        )
    )
    output.write(gamma)
    for w, h, pixels, owned, sampler, levels in textures:
        output.write(struct.pack("<6I", w, h, len(pixels), owned, sampler, levels))
        output.write(pixels)
    for draw in draws:
        output.write(draw)
print(f"Prepared {len(draws)} draws, {len(textures)} textures.")
