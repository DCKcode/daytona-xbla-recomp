import json
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest

PACKER = pathlib.Path(__file__).with_name("pack_native_frame.py")
WIDTH, HEIGHT = 1920, 1080
EDGES = (0, 160, 320, 480, 640, 800, 928, 1080)
VS, PS = 0xFA44706CA015263D, 0x7ABDFEFBCADB4BF9
CLEAR_VS, CLEAR_PS = 0x1E6883FCCDE1F688, 0xA4A965C189287B99


class Capture:
    def __init__(
        self,
        root,
        *,
        changed_tile=False,
        changed_texture=False,
        clear_only=False,
        bad_scissor=False,
        changed_vertex=False,
        ui_before_world=False,
        mipmapped=False,
        luminance=False,
    ):
        self.root = root
        self.mipmapped = mipmapped
        self.luminance = luminance
        self.records = bytearray(struct.pack("<3I", 0x44544350, 3, 0x5003))
        bindings = [[95, 3, 0, 57, 1, 1], [92, 1, 0, 6, 0, 0], [87, 2, 0, 37, 1, 1]]
        for shader, stage, attrs, slots, constants in (
            (VS, "vs", bindings, [], [72, 73, 74, 75]),
            (PS, "ps", [], [0], []),
            (CLEAR_VS, "vs", [[0, 7, 0, 57, 1, 1], [0, 7, 3, 38, 1, 1]], [], []),
            (CLEAR_PS, "ps", [], [], []),
        ):
            (root / f"{shader:016X}.{stage}.json").write_text(
                json.dumps(
                    {
                        "memexport": 0,
                        "unsupported_constants": 0,
                        "vertex_bindings": attrs,
                        "texture_slots": slots,
                        "sampler_bindings": [[slot, 3, 3, 3, 7] for slot in slots],
                        "float_constants": constants,
                    }
                )
            )
        self.page(0x1000, struct.pack("<12f", 0, 1, 0, 0, 0, 0, 1, 0, 0, 1, 1, 0))
        self.page(0x2000, bytes([255]) * 16)
        self.page(0x3000, struct.pack("<8f", 0, 0, 0, 1, 1, 1, 1, 0))
        self.page(
            0x4000,
            b"".join(
                struct.pack("<7f", x, y, 0.75, 0.25, 0.5, 0.75, 1)
                for x, y in ((0, 0), (WIDTH, 0), (WIDTH, 160))
            ),
        )
        self.page(0x5000, bytes([0, 0, 255, 255]) * 1024)
        if mipmapped:
            self.page(0x7000, bytes([255, 0, 0, 255]) * 1024)
        if ui_before_world:
            self.draw(self.registers(0), VS, PS)
        r = self.registers(2)
        r[0x4800:0x4802] = [0x4003, 21 << 2]
        r[0x2200], r[0x2104], r[0x2206], r[0x2204] = 0x76, 0xFFFF, 0x300, 0x10000
        self.draw(r, CLEAR_VS, CLEAR_PS, 8, 3)
        for tile, (top, bottom) in enumerate(zip(EDGES, EDGES[1:])):
            if changed_vertex and tile == 1:
                self.page(
                    0x1000, struct.pack("<12f", 0.25, 1, 0, 0, 0, 0, 1, 0, 0, 1, 1, 0)
                )
            r = self.registers(2)
            r[0x2080] = ((-top) & 32767) << 16
            r[0x2081], r[0x2082] = top << 16, (bottom << 16) | WIDTH
            if bad_scissor and tile == 1:
                r[0x2082] -= 1
            if changed_tile and tile == 1:
                r[0x2201] = 0x1000706
            if not clear_only:
                self.draw(r, VS, PS)
            self.copy(r, top, bottom, 0x10000 + WIDTH * top * 4, tile)
        if changed_vertex:
            self.page(0x1000, struct.pack("<12f", 0, 1, 0, 0, 0, 0, 1, 0, 0, 1, 1, 0))
        r = self.registers(0)
        r[0x4801], r[0x4802], r[0x4803] = (
            0x10006,
            ((HEIGHT - 1) << 13) | (WIDTH - 1),
            0x60A << 1,
        )
        self.draw(r, VS, PS)
        if changed_texture:
            self.page(0x5000, bytes([255, 0, 0, 255]) * 1024)
            self.draw(self.registers(0), VS, PS)
        self.copy(self.registers(0), 0, HEIGHT, 0x800000)
        self.path = root / "capture"
        self.path.mkdir()
        (self.path / "events.bin").write_bytes(self.records)
        (self.path / "gamma.rgb").write_bytes(
            bytes(value for value in range(256) for _ in range(3))
        )

    def page(self, address, data):
        self.records += struct.pack(
            "<8I", 3, address, 4096, 0, 0, 0, 0, 0
        ) + data.ljust(4096, b"\0")

    def registers(self, samples):
        r = [0] * 0x5003
        r[0x2000] = samples << 16
        r[0x2200], r[0x2201], r[0x2204], r[0x2205], r[0x2206] = (
            0x700710,
            0x10001,
            0x80000,
            0x18000,
            0x43F,
        )
        r[0x2104], r[0x2082] = 15, (HEIGHT << 16) | WIDTH
        r[0x210F:0x2115] = struct.unpack(
            "<6I", struct.pack("<6f", 960, 960, -540, 540, 1, 0)
        )
        r[0x4120:0x4130] = struct.unpack(
            "<16I",
            struct.pack("<16f", 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, -1, 0, -1, -1, 0, 1),
        )
        for slot, address, words in (
            (95, 0x1000, 12),
            (92, 0x2000, 4),
            (87, 0x3000, 8),
        ):
            r[0x4800 + slot * 2 : 0x4802 + slot * 2] = [address | 3, words << 2]
        r[0x4800:0x4806] = [
            (1 << 22) | 2,
            0x5006,
            (31 << 13) | 31,
            0x688 << 1,
            0,
            1 << 9,
        ]
        if self.luminance:
            r[0x4801], r[0x4803] = 0x500A, 0x200 << 1
        if self.mipmapped:
            r[0x4804] = 1 << 6
            r[0x4805] |= 0x7000
            name = "texture_" + "_".join(f"{v:08X}" for v in r[0x4800:0x4806]) + ".json"
            (self.root / name).write_text(
                json.dumps(
                    {
                        "sources": [[0x5000, 4096], [0x7000, 4096]],
                        "levels": [
                            [32, 32, 0x5000, 32, 0, 0],
                            [16, 16, 0x7000, 32, 0, 0],
                        ],
                    }
                )
            )
        return r

    def draw(self, r, vs, ps, primitive=13, count=4):
        self.event((1, primitive, count, 0, 0, 0, 0, 0), r, vs, ps)

    def event(self, header, r, vs=VS, ps=PS):
        self.records += (
            struct.pack("<8I", *header)
            + struct.pack(f"<{len(r)}I", *r)
            + struct.pack("<2Q", vs, ps)
        )

    def copy(self, r, top, bottom, address, tile=None):
        self.page(0x6000, struct.pack("<6f", 0, top, WIDTH, top, WIDTH, bottom))
        r[0x4800:0x4802] = [0x6003, 6 << 2]
        r[0x2302], r[0x2319], r[0x231A], r[0x231B] = 5, address, 0x4380780, 0x1000300
        r[0x2318] = 0x100000 if tile is None else 0x100060 if tile == 6 else 0x100360
        r[0x231D] = 0xFFFFFF00
        self.event((2, 0, 0, 0, 0, 0, 0, 0), r)

    def pack(self):
        return subprocess.run(
            [sys.executable, str(PACKER), str(self.path), str(self.root / "output")],
            capture_output=True,
            text=True,
        )


class ReferencePackerTests(unittest.TestCase):
    def test_quad_diagonal_matches_sdk_triangle_strip(self):
        with tempfile.TemporaryDirectory() as folder:
            capture = Capture(pathlib.Path(folder))
            result = capture.pack()
            self.assertEqual(result.returncode, 0, result.stderr)
            data = (capture.root / "output/frame.bin").read_bytes()
            offset = 188 + 768
            for _ in range(struct.unpack_from("<7I", data)[5]):
                texture = struct.unpack_from("<6I", data, offset)
                offset += 24 + texture[2]
            draw = struct.unpack_from("<2Q6I6f6I", data, offset)
            vertices = offset + 88 + draw[3] * 4 + 8704
            positions = [
                struct.unpack_from("<2f", data, vertices + i * draw[3] * 16)
                for i in range(6)
            ]
            self.assertEqual(
                positions, [(0, 1), (0, 0), (1, 1), (0, 0), (1, 0), (1, 1)]
            )

    def test_luminance_gamma_flag_is_per_texture_slot(self):
        with tempfile.TemporaryDirectory() as folder:
            capture = Capture(pathlib.Path(folder), luminance=True)
            result = capture.pack()
            self.assertEqual(result.returncode, 0, result.stderr)
            data = (capture.root / "output/frame.bin").read_bytes()
            offset = 188 + 768
            for _ in range(struct.unpack_from("<7I", data)[5]):
                texture = struct.unpack_from("<6I", data, offset)
                offset += 24 + texture[2]
            draw = struct.unpack_from("<2Q6I6f6I", data, offset)
            shared = offset + 88 + draw[3] * 4 + 8192
            self.assertEqual(struct.unpack_from("<2I", data, shared + 288), (1, 0))

    def test_captured_mip_level_is_uploaded_with_its_pixels(self):
        with tempfile.TemporaryDirectory() as folder:
            capture = Capture(pathlib.Path(folder), mipmapped=True)
            result = capture.pack()
            self.assertEqual(result.returncode, 0, result.stderr)
            packed = (capture.root / "output/frame.bin").read_bytes()
            texture = struct.unpack_from("<6I", packed, 188 + 768)
            self.assertEqual((texture[2], texture[5]), (5120, 2))
            mip = packed[188 + 768 + 24 + 4096 : 188 + 768 + 24 + 5120]
            self.assertEqual(mip, bytes([255, 0, 0, 255]) * 256)

    def test_helper_constant_matches_sdk_effective_value(self):
        with tempfile.TemporaryDirectory() as folder:
            capture = Capture(pathlib.Path(folder))
            path = capture.root / f"{PS:016X}.ps.json"
            shader = json.loads(path.read_text())
            shader["float_constants"] = [255]
            path.write_text(json.dumps(shader))
            result = capture.pack()
            self.assertEqual(result.returncode, 0, result.stderr)
            data = (capture.root / "output/frame.bin").read_bytes()
            offset = 188 + 768
            for _ in range(struct.unpack_from("<7I", data)[5]):
                texture = struct.unpack_from("<6I", data, offset)
                offset += 24 + texture[2]
            draw = struct.unpack_from("<2Q6I6f6I", data, offset)
            constants = offset + 88 + draw[3] * 4 + 4096
            self.assertEqual(
                struct.unpack_from("<4f", data, constants + 255 * 16), (0, 1, 0, 1)
            )

    def test_clear_binding_and_depth_are_preserved(self):
        with tempfile.TemporaryDirectory() as folder:
            capture = Capture(pathlib.Path(folder))
            result = capture.pack()
            self.assertEqual(result.returncode, 0, result.stderr)
            header = struct.unpack(
                "<7I40f", (capture.root / "output/frame.bin").read_bytes()[:188]
            )
            self.assertEqual(header[12:17], (0.25, 0.5, 0.75, 1.0, 0.75))

    def test_clear_only_world_is_still_a_pass(self):
        with tempfile.TemporaryDirectory() as folder:
            capture = Capture(pathlib.Path(folder), clear_only=True)
            result = capture.pack()
            self.assertEqual(result.returncode, 0, result.stderr)
            header = struct.unpack(
                "<7I", (capture.root / "output/frame.bin").read_bytes()[:28]
            )
            self.assertEqual((header[4], header[6]), (1, 1))

    def test_changed_tile_state_keeps_separate_bands(self):
        with tempfile.TemporaryDirectory() as folder:
            capture = Capture(pathlib.Path(folder), changed_tile=True)
            result = capture.pack()
            self.assertEqual(result.returncode, 0, result.stderr)
            data = (capture.root / "output/frame.bin").read_bytes()
            header = struct.unpack_from("<7I", data)
            self.assertEqual(header[4], 8)
            offset = 188 + 768
            for _ in range(header[5]):
                texture = struct.unpack_from("<6I", data, offset)
                offset += 24 + texture[2]
            for tile in range(7):
                draw = struct.unpack_from("<2Q6I6f6I", data, offset)
                self.assertEqual(draw[14:18], (0, EDGES[tile], WIDTH, EDGES[tile + 1]))
                self.assertEqual(draw[4], 0x1000706 if tile == 1 else 0x10001)
                offset += 88 + draw[3] * 4 + 8704 + draw[2] * draw[3] * 16

    def test_unrecognized_scissor_is_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            result = Capture(pathlib.Path(folder), bad_scissor=True).pack()
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("tile scissor", result.stderr)

    def test_changed_vertex_data_is_not_deduplicated(self):
        with tempfile.TemporaryDirectory() as folder:
            capture = Capture(pathlib.Path(folder), changed_vertex=True)
            result = capture.pack()
            self.assertEqual(result.returncode, 0, result.stderr)
            header = struct.unpack(
                "<7I", (capture.root / "output/frame.bin").read_bytes()[:28]
            )
            self.assertEqual(header[4], 8)

    def test_ui_world_ui_is_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            result = Capture(pathlib.Path(folder), ui_before_world=True).pack()
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("world pass followed by UI", result.stderr)

    def test_texture_reupload_gets_a_new_texture(self):
        with tempfile.TemporaryDirectory() as folder:
            capture = Capture(pathlib.Path(folder), changed_texture=True)
            result = capture.pack()
            self.assertEqual(result.returncode, 0, result.stderr)
            header = struct.unpack(
                "<7I", (capture.root / "output/frame.bin").read_bytes()[:28]
            )
            self.assertEqual(header[5], 3)

    def test_shader_filter_override_preserves_clamped_edges(self):
        with tempfile.TemporaryDirectory() as folder:
            capture = Capture(pathlib.Path(folder))
            path = capture.root / f"{PS:016X}.ps.json"
            shader = json.loads(path.read_text())
            shader["sampler_bindings"] = [[0, 1, 0, 3, 7]]
            path.write_text(json.dumps(shader))
            # Change every source descriptor from repeat to clamp-to-edge.
            data = capture.records
            offset = 12
            while offset < len(data):
                kind = struct.unpack_from("<I", data, offset)[0]
                if kind == 3:
                    offset += 32 + 4096
                    continue
                address = offset + 32 + 0x4800 * 4
                fetch = struct.unpack_from("<I", data, address)[0]
                if kind == 1 and fetch & 3 == 2:
                    struct.pack_into("<I", data, address, fetch | (2 << 10) | (2 << 13))
                offset += 32 + 0x5003 * 4 + 16
            (capture.path / "events.bin").write_bytes(data)
            result = capture.pack()
            self.assertEqual(result.returncode, 0, result.stderr)
            packed = (capture.root / "output/frame.bin").read_bytes()
            texture = struct.unpack_from("<6I", packed, 188 + 768)
            self.assertEqual(texture[4], 1 | (2 << 2) | (2 << 5))


if __name__ == "__main__":
    unittest.main()
