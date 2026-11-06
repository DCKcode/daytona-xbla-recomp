import argparse
import pathlib
import struct
import re
import subprocess
import json

from native_shader_tools import build_translator, find_dxc

parser = argparse.ArgumentParser(
    description="Translate captured Daytona shaders using a local XenosRecomp checkout."
)
parser.add_argument("probe", type=pathlib.Path)
parser.add_argument("output", type=pathlib.Path)
parser.add_argument("--xenosrecomp", type=pathlib.Path, required=True)
parser.add_argument("--dxc", type=pathlib.Path, help="Override the native DXC executable")
args = parser.parse_args()
root = args.xenosrecomp.resolve()
probe = args.probe.resolve()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
expected_revision = "990d03b28a27b50277ee5d8d942e1c5f873869d1"
revision = subprocess.check_output(
    ["git", "-C", str(root), "rev-parse", "HEAD"], text=True
).strip()
if revision != expected_revision:
    parser.error(f"Use XenosRecomp revision {expected_revision}; found {revision}.")


# Captured microcode has no D3D reflection container. These shaders use a
# fixed float-constant array and sequential interpolators; reconstruct that
# metadata so XenosRecomp can consume the original instructions.
def replace_expected(text, old, new, count):
    if text.count(old) != count:
        raise ValueError(f"Unexpected translator output for {old!r}")
    return text.replace(old, new)


be = lambda *x: struct.pack(">" + str(len(x)) + "I", *x)
shader_paths = sorted((*probe.glob("*.vs.bin"), *probe.glob("*.ps.bin")))
if not shader_paths:
    parser.error("No captured shader microcode found.")
try:
    dxc, env = find_dxc(root, args.dxc)
except ValueError as error:
    parser.error(str(error))
translator = build_translator(root, out)
for path in shader_paths:
    name = path.stem
    vertex = name.endswith(".vs")
    text = path.with_suffix(".txt").read_text()
    raw = path.read_bytes()
    metadata = json.loads(path.with_suffix(".json").read_text())
    if metadata["memexport"] or metadata["unsupported_constants"]:
        raise ValueError("Unsupported shader memory exports or constant addressing")
    attrs = [int(x) for x in re.findall(r"/\*\s*(\d+)\s*\*/\s*vfetch_", text)]
    if vertex and len(attrs) != len(metadata["vertex_bindings"]):
        raise ValueError("Shader fetch instructions do not match binding metadata")
    constants = [("gC", 2, 0, 256)] + [
        (f"gS{x}", 3, x, 1) for x in metadata["texture_slots"]
    ]
    n = len(constants)
    ct = bytearray(be(28, 0, 0, n, 28, 0, 0) + bytes(n * 20))
    for i, (cn, rs, index, count) in enumerate(constants):
        no = len(ct)
        ct += cn.encode() + b"\0"
        ct += bytes((-len(ct)) % 4)
        ti = len(ct)
        ct += struct.pack(
            ">6HI", 4 if rs == 3 else 1, 12 if rs == 3 else 3, 1, 4, count, 0, 0
        )
        struct.pack_into(">I4H2I", ct, 28 + i * 20, no, rs, index, count, 0, ti, 0)
    ct = be(len(ct) + 4) + ct
    shader_offset = 36 + len(ct)
    interps = [i | (5 << 4) | (i << 8) for i in range(16)]
    shader = be(0, len(raw), 0, 0xFF00, 0, 16 << 5)
    if vertex:
        shader += be(
            0,
            len(attrs),
            0,
            *[pc | (5 << 12) | (i << 16) for i, pc in enumerate(attrs)],
            *interps,
        )
    else:
        outputs = sum(1 << int(i) for i in set(re.findall(r"oC(\d)", text))) | (
            16 if "oDepth" in text else 0
        )
        shader += be(0, outputs, *interps)
    virtual = (
        be(
            0x102A1101 if vertex else 0x102A1100,
            0,
            len(raw),
            0,
            36,
            0,
            shader_offset,
            0,
            0,
        )
        + ct
        + shader
    )
    virtual += bytes((-len(virtual)) % 16)
    virtual = virtual[:4] + be(len(virtual)) + virtual[8:]
    dest = out / (name + ".container")
    dest.write_bytes(virtual + raw)
    r = subprocess.run(
        [
            str(translator),
            str(dest),
            str(out / (name + ".hlsl")),
            str(root / "XenosRecomp/shader_common.h"),
        ],
        env=env,
        capture_output=True,
        text=True,
    )
    if r.returncode:
        raise RuntimeError(r.stderr or r.stdout)
    print("Translated", name)

for p in out.glob("*.hlsl"):
    if ".ubo." in p.name:
        continue
    s = p.read_text()
    a = s.index("#ifdef __spirv__")
    b = s.index("#else", a)
    c = s.index("#endif", b)
    block = replace_expected(
        s[b + len("#else") : c],
        "uint g_SpecConstants();",
        "[[vk::constant_id(0)]] const uint g_SpecConstants = 0;\n#define g_SpecConstants() g_SpecConstants",
        1,
    )
    s = s[:a] + block + s[c + len("#endif") :]
    a = s.index("#ifdef __spirv__\n\n#define gC")
    s = s[:a] + s[a:].replace("#ifdef __spirv__", "#if 0", 1)
    # TEXCOORD indices are below 32; avoid an unnecessary shaderInt64 requirement.
    s = replace_expected(s, "1ull << semanticIndex", "1u << semanticIndex", 1)
    s = replace_expected(s, "DescriptorHeap[]", "DescriptorHeap[2]", 4)
    # Match the SDK's subtexel rounding bias. Palette lookups land exactly
    # between rows; interpolation error can otherwise select the previous row.
    s = replace_expected(
        s,
        "texCoord + offset / getTexture2DDimensions(texture)",
        "texCoord + (offset + (1.5 / 1024.0)) / getTexture2DDimensions(texture)",
        1,
    )
    # Preserve the SDK's luminance/alpha interpretation after filtering;
    # converting uploaded texels would change bilinear and mip interpolation.
    s = replace_expected(
        s,
        "float4 tfetch2D(uint resourceDescriptorIndex",
        "float4 nativeTextureSample(float4 value, uint index);\n\n"
        "float4 tfetch2D(uint resourceDescriptorIndex",
        1,
    )
    s = replace_expected(
        s,
        "return texture.Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord + (offset + (1.5 / 1024.0)) / getTexture2DDimensions(texture));",
        "return nativeTextureSample(texture.Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord + (offset + (1.5 / 1024.0)) / getTexture2DDimensions(texture)), resourceDescriptorIndex);",
        1,
    )
    s = replace_expected(
        s,
        "    float g_AlphaThreshold : packoffset(c17.x);",
        "    float g_AlphaThreshold : packoffset(c17.x); \\\n    uint2 g_TextureGamma : packoffset(c18);",
        1,
    )
    helper = r"""
float4 nativeTextureSample(float4 value, uint index)
{
    if (g_TextureGamma[index])
    {
        float gamma = saturate(value.r);
        float scale = select(gamma >= 192.0 / 255.0, 8.0 / 1024.0, 4.0 / 1024.0);
        float offset = select(gamma >= 192.0 / 255.0, -1024.0, -256.0);
        scale = select(gamma >= 96.0 / 255.0, scale, select(gamma >= 64.0 / 255.0, 2.0 / 1024.0, 1.0 / 1024.0));
        offset = select(gamma >= 96.0 / 255.0, offset, select(gamma >= 64.0 / 255.0, -64.0, 0.0));
        precise float decoded = gamma * (255.0 * 1024.0);
        decoded = decoded * scale + offset;
        decoded = decoded + trunc(decoded * scale);
        value.rgb = decoded * (1.0 / 1023.0);
    }
    return value;
}

"""
    s = replace_expected(s, "void main(", helper + "void main(", 1)
    if ".ps." in p.name:
        s = replace_expected(s, "(INDEX) < 224", "(INDEX) < 256", 2)
        s = replace_expected(s, "min(INDEX, 223)", "min(INDEX, 255)", 2)

    def sorted_block(m):
        lines = m[2].splitlines()
        members = []
        rest = []
        for line in lines:
            offset = re.search(r"packoffset\(c(\d+)(?:\.([xyzw]))?\)", line)
            if offset:
                members.append(
                    (int(offset[1]) * 4 + "xyzw".index(offset[2] or "x"), line)
                )
            else:
                rest.append(line)
        return (
            m[1]
            + "\n"
            + "\n".join(line for _, line in sorted(members))
            + "\n"
            + "\n".join(rest)
            + m[3]
        )

    s, constant_buffers = re.subn(
        r"(cbuffer \w+ : register\(b\d, space4\)\s*\{)(.*?)(\n\};)",
        sorted_block,
        s,
        flags=re.S,
    )
    if constant_buffers != 2:
        raise ValueError("Expected shader and shared constant buffers")
    # Keep 2D images, samplers and constants within Vulkan's four-set minimum.
    # Unused 3D and cube declarations are optimized out.
    spaces = {0: 0, 1: 3, 2: 4, 3: 1, 4: 2}
    s = re.sub(r"\bspace([0-4])\b", lambda m: f"space{spaces[int(m[1])]}", s)
    q = p.with_suffix(".ubo.hlsl")
    q.write_text(s)
    cmd = [
        str(dxc),
        "-T",
        "vs_6_0" if ".vs." in p.name else "ps_6_0",
        "-E",
        "main",
        "-spirv",
        "-fspv-target-env=vulkan1.1",
        "-fvk-use-dx-layout",
        "-O3",
        str(q),
        "-Fo",
        str(p.with_suffix(".spv")),
    ]
    r = subprocess.run(cmd, env=env, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(r.stderr or r.stdout)
    print("Compiled", p.name)

# The present pass applies the guest's display lookup into the SDK mailbox.
for stage in ("vs", "ps"):
    source = (
        pathlib.Path(__file__).resolve().parent.parent
        / "project/shaders"
        / f"present.{stage}.hlsl"
    )
    subprocess.run(
        [
            str(dxc),
            "-T",
            f"{stage}_6_0",
            "-E",
            "main",
            "-spirv",
            "-fspv-target-env=vulkan1.1",
            "-O3",
            str(source),
            "-Fo",
            str(out / f"present.{stage}.spv"),
        ],
        env=env,
        check=True,
    )
