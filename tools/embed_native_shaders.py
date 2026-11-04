"""Embed the offline translator's SPIR-V for the live renderer."""

import pathlib
import struct
import sys

source = pathlib.Path(sys.argv[1]) if sys.argv[1] else None
output = pathlib.Path(sys.argv[2])
files = sorted(source.glob("*.spv")) if source else []
if source and not files:
    raise SystemExit(f"No translated SPIR-V shaders in {source}")
with output.open("w") as out:
    out.write("#include <span>\n#include <stdexcept>\n#include <string_view>\n")
    out.write(
        "static std::span<const uint32_t> EmbeddedShader(std::string_view name) {\n"
    )
    for index, file in enumerate(files):
        data = file.read_bytes()
        words = struct.unpack(f"<{len(data)//4}I", data)
        out.write(f"  static constexpr uint32_t shader_{index}[] = {{\n")
        for i in range(0, len(words), 8):
            out.write("    " + ",".join(f"0x{w:08X}" for w in words[i : i + 8]) + ",\n")
        out.write("  };\n")
        out.write(f'  if (name == "{file.name}") return shader_{index};\n')
    out.write(
        '  throw std::runtime_error("Untranslated Daytona shader " + std::string(name));\n}\n'
    )
