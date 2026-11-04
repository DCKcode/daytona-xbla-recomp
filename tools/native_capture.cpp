#include "native_frame_builder.h"
#include <fstream>
#include <iostream>
#include <map>
#include <rex/string/buffer.h>

int main(int argc, char **argv) {
  if (argc < 4) {
    std::cerr << "usage: daytona_native_capture CAPTURE SHADERS OUTPUT.rgba "
                 "[--single-pass] [--save-frame=PATH]\n";
    return 2;
  }
  try {
    std::filesystem::path capture = argv[1], shaders = capture.parent_path();
    std::vector<uint8_t> memory(1 << 29);
    auto mode = daytona::native::SceneInputs::VerifyEveryTile;
    std::filesystem::path frame_output;
    for (int i = 4; i < argc; ++i) {
      const std::string_view option(argv[i]);
      if (option == "--single-pass")
        mode = daytona::native::SceneInputs::SinglePass;
      else if (option.starts_with("--save-frame=") && option.size() > 13)
        frame_output = option.substr(13);
      else
        throw std::runtime_error("Unknown native capture option");
    }
    daytona::native::FrameBuilder builder(memory.data(), nullptr, mode);
    uint32_t source_tile = 0;
    std::map<uint64_t, std::unique_ptr<rex::graphics::Shader>> programs;
    auto load = [&](uint64_t hash, bool vertex) {
      if (programs.contains(hash))
        return programs.at(hash).get();
      char name[40];
      std::snprintf(name, sizeof(name), "%016llX.%s.bin",
                    (unsigned long long)hash, vertex ? "vs" : "ps");
      std::ifstream file(shaders / name, std::ios::binary | std::ios::ate);
      if (!file)
        throw std::runtime_error("Missing shader");
      auto size = file.tellg();
      file.seekg(0);
      std::vector<uint32_t> data(size_t(size) / 4);
      file.read(reinterpret_cast<char *>(data.data()), size);
      auto shader = std::make_unique<rex::graphics::Shader>(
          vertex ? rex::graphics::xenos::ShaderType::kVertex
                 : rex::graphics::xenos::ShaderType::kPixel,
          hash, data.data(), data.size());
      rex::string::StringBuffer disassembly;
      shader->AnalyzeUcode(disassembly);
      auto *result = shader.get();
      programs.emplace(hash, std::move(shader));
      return result;
    };
    std::ifstream events(capture / "events.bin", std::ios::binary);
    events.exceptions(std::ios::badbit);
    auto read = [&](void *data, size_t size) {
      if (!events.read(static_cast<char *>(data), size))
        throw std::runtime_error("Truncated capture");
    };
    uint32_t header[3];
    read(header, sizeof(header));
    if (header[0] != 0x44544350 || header[1] != 3 || header[2] != 0x5003)
      throw std::runtime_error("Unsupported capture");
    rex::graphics::RegisterFile registers;
    while (events.peek() != EOF) {
      uint32_t event[8];
      read(event, sizeof(event));
      if (event[0] == 3) {
        read(memory.data() + event[1], event[2]);
        continue;
      }
      read(registers.values, 0x5003 * 4);
      uint64_t hashes[2];
      read(hashes, sizeof(hashes));
      if (mode == daytona::native::SceneInputs::SinglePass &&
          ((registers[0x2000] >> 16) & 3) == 2) {
        if (event[0] == 2) {
          if (source_tile++)
            continue;
          registers[0x2318] = 0x100060;
        } else {
          if (source_tile)
            continue;
          registers[0x2080] = registers[0x2081] = 0;
          registers[0x2082] = (1080 << 16) | 1920;
        }
      }
      if (event[0] == 2)
        builder.CopyGuest(registers);
      else if (hashes[0] != daytona::native::kSyncVertexShader)
        builder.DrawGuest(registers, *load(hashes[0], true),
                          *load(hashes[1], false), event[1], event[2],
                          event[3]);
    }
    if (mode == daytona::native::SceneInputs::SinglePass && source_tile != 0 &&
        source_tile != 1 && source_tile != 7)
      throw std::runtime_error("Unsupported source capture tile count");
    auto frame = builder.Finish();
    std::ifstream gamma(capture / "gamma.rgb", std::ios::binary);
    if (!gamma.read(reinterpret_cast<char *>(frame.gamma.data()),
                    frame.gamma.size()))
      throw std::runtime_error("Missing or truncated capture gamma ramp");
    if (!frame_output.empty())
      daytona::native::SaveFrame(frame, frame_output);
    daytona::native::Renderer renderer;
    auto pixels = renderer.Render(frame, argv[2]);
    std::ofstream output(argv[3], std::ios::binary);
    output.exceptions(std::ios::failbit | std::ios::badbit);
    output.write(reinterpret_cast<const char *>(pixels.data()), pixels.size());
    std::cout << frame.draws.size() << " draws, " << frame.textures.size()
              << " textures\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
