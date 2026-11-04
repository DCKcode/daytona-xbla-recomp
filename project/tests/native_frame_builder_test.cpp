#include "native_frame_builder.h"
#include <algorithm>
#include <bit>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
namespace native = daytona::native;
namespace gpu = rex::graphics;
namespace xenos = gpu::xenos;

void Expect(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

template <typename F> void Reject(F operation, const char *message) {
  try {
    operation();
  } catch (const std::runtime_error &) {
    return;
  }
  throw std::runtime_error(message);
}

// Supply SDK reflection at the builder boundary without a game shader fixture.
class TestShader : public gpu::Shader {
public:
  TestShader(xenos::ShaderType type, uint64_t hash)
      : Shader(type, hash, &empty_, 1) {}
  void Vertex(uint32_t fetch, uint32_t stride, uint32_t offset,
              xenos::VertexFormat format) {
    auto binding =
        std::find_if(vertex_bindings_.begin(), vertex_bindings_.end(),
                     [&](const auto &b) { return b.fetch_constant == fetch; });
    if (binding == vertex_bindings_.end()) {
      vertex_bindings_.push_back(
          {int(vertex_bindings_.size()), fetch, stride, {}});
      binding = vertex_bindings_.end() - 1;
    }
    VertexBinding::Attribute attribute{};
    auto &a = attribute.fetch_instr.attributes;
    a.data_format = format;
    a.offset = offset;
    a.is_signed = a.is_integer = true;
    binding->attributes.push_back(attribute);
  }
  void Texture() {
    TextureBinding binding{};
    binding.fetch_constant = 0;
    texture_bindings_.push_back(binding);
  }
  void Constant(uint32_t index) {
    constant_register_map_.float_bitmap[index / 64] |= uint64_t(1)
                                                       << (index % 64);
  }

private:
  static constexpr uint32_t empty_ = 0;
};

struct Fixture {
  std::vector<uint8_t> memory = std::vector<uint8_t>(1 << 20);
  TestShader vs{xenos::ShaderType::kVertex, 1},
      ps{xenos::ShaderType::kPixel, 2};
  TestShader clear_vs{xenos::ShaderType::kVertex, native::kClearVertexShader};
  TestShader clear_ps{xenos::ShaderType::kPixel, native::kClearPixelShader};

  Fixture() {
    vs.Vertex(95, 3, 0, xenos::VertexFormat::k_32_32_32_FLOAT);
    vs.Constant(72);
    ps.Texture();
    ps.Constant(255);
    clear_vs.Vertex(0, 7, 0, xenos::VertexFormat::k_32_32_32_FLOAT);
    clear_vs.Vertex(0, 7, 3, xenos::VertexFormat::k_32_32_32_32_FLOAT);
    const float positions[] = {0, 1, 0, 0, 0, 0, 1, 0, 0, 1, 1, 0};
    std::memcpy(memory.data() + 0x1000, positions, sizeof(positions));
    std::fill_n(memory.data() + 0x5000, 4096, uint8_t(7));
    const float clear[] = {0,    0,   .75f, .25f, .5f, .75f, 1,
                           1920, 0,   .75f, .25f, .5f, .75f, 1,
                           1920, 160, .75f, .25f, .5f, .75f, 1};
    std::memcpy(memory.data() + 0x4000, clear, sizeof(clear));
  }
  gpu::RegisterFile Registers(uint32_t samples = 0) const {
    gpu::RegisterFile r;
    std::fill_n(r.values, gpu::RegisterFile::kRegisterCount, 0);
    r[0x2000] = samples << 16;
    r[0x2200] = 0x700710;
    r[0x2201] = 0x10001;
    r[0x2204] = 0x80000;
    r[0x2205] = 0x18000;
    r[0x2206] = 0x43f;
    r[0x2104] = 15;
    r[0x2082] = (native::kHeight << 16) | native::kWidth;
    const float viewport[] = {960, 960, -540, 540, 1, 0};
    for (uint32_t i = 0; i < 6; ++i)
      r[0x210f + i] = std::bit_cast<uint32_t>(viewport[i]);
    r[0x4120] = std::bit_cast<uint32_t>(2.0f);
    r[0x4800 + 95 * 2] = 0x1003;
    r[0x4801 + 95 * 2] = 12 << 2;
    const uint32_t texture[] = {(1 << 22) | 2, 0x5006, (31 << 13) | 31,
                                0x688 << 1,    0,      1 << 9};
    std::copy_n(texture, 6, r.values + 0x4800);
    return r;
  }
  void Clear(native::FrameBuilder &builder) {
    auto r = Registers(2);
    r[0x4800] = 0x4003;
    r[0x4801] = 21 << 2;
    r[0x2200] = 0x76;
    builder.DrawGuest(r, clear_vs, clear_ps, 8, 3, false);
  }
  void Copy(native::FrameBuilder &builder, uint32_t tile) {
    auto r = Registers(2);
    r[0x2319] = 0x10000 + native::kWidth * native::kTileEdges[tile] * 4;
    r[0x231a] = 0x4380780;
    r[0x231b] = 0x1000300;
    r[0x2318] = tile == 6 ? 0x100060 : 0x100360;
    r[0x231d] = 0xffffff00;
    builder.CopyGuest(r);
  }
};

float Position(const native::Draw &draw) {
  float value;
  std::memcpy(&value, draw.vertices.data(), sizeof(value));
  return value;
}

void OwnedVerticesAndQuads() {
  Fixture f;
  native::FrameBuilder builder(f.memory.data(), nullptr,
                               native::SceneInputs::SinglePass);
  auto r = f.Registers();
  builder.DrawGuest(r, f.vs, f.ps, 13, 4, false);
  const float changed = .25f;
  std::memcpy(f.memory.data() + 0x1000, &changed, sizeof(changed));
  builder.DrawGuest(r, f.vs, f.ps, 13, 4, false);
  auto frame = builder.Finish();
  Expect(frame.draws.size() == 2 && Position(frame.draws[0]) == 0 &&
             Position(frame.draws[1]) == changed,
         "Draws must own their vertex data before guest buffers are reused");
  Expect(frame.draws[0].primitive == native::Primitive::Quads &&
             frame.draws[0].vertex_count == 4 &&
             frame.draws[0].vertices.size() == 64,
         "Native quads must retain their four source vertices");
  builder.DrawGuest(r, f.vs, f.ps, 6, 4, false);
  Expect(builder.Finish().draws[0].primitive ==
             native::Primitive::TriangleStrip,
         "Triangle strips must retain their topology across frames");
}

void TextureReuploads() {
  Fixture f;
  native::FrameBuilder builder(f.memory.data(), nullptr,
                               native::SceneInputs::SinglePass);
  auto r = f.Registers();
  builder.DrawGuest(r, f.vs, f.ps, 13, 4, false);
  builder.DrawGuest(r, f.vs, f.ps, 13, 4, false);
  f.memory[0x5000] = 77;
  builder.DrawGuest(r, f.vs, f.ps, 13, 4, false);
  auto frame = builder.Finish();
  Expect(frame.textures.size() == 2 &&
             frame.draws[0].textures[0] == frame.draws[1].textures[0] &&
             frame.draws[2].textures[0] != frame.draws[0].textures[0],
         "Changed texture contents require a new immutable texture");
  Expect(frame.textures[0]->pixels[0] == 7 &&
             frame.textures[1]->pixels[0] == 77,
         "Texture reuploads must preserve earlier draws' pixels");
}

void Constants() {
  Fixture f;
  native::FrameBuilder builder(f.memory.data(), nullptr,
                               native::SceneInputs::SinglePass);
  builder.DrawGuest(f.Registers(), f.vs, f.ps, 13, 4, false);
  auto frame = builder.Finish();
  float value;
  std::memcpy(&value, frame.draws[0].vs_constants.data() + 72 * 16, 4);
  Expect(value == 2, "Used guest constants must be copied");
  std::array<float, 4> helper;
  std::memcpy(helper.data(), frame.draws[0].ps_constants.data() + 255 * 16, 16);
  Expect(helper == std::array<float, 4>{0, 1, 0, 1},
         "The effective SDK helper constant must override guest memory");
}

void ClearOnly() {
  Fixture f;
  native::FrameBuilder builder(f.memory.data());
  f.Clear(builder);
  for (uint32_t tile = 0; tile < 7; ++tile)
    f.Copy(builder, tile);
  auto frame = builder.Finish();
  Expect(frame.world && frame.draws.empty() &&
             frame.world_clear.color ==
                 std::array<float, 4>{.25f, .5f, .75f, 1} &&
             frame.world_clear.depth == .75f,
         "A clear-only world must retain binding-zero color and depth");
}

void OutputCopies() {
  Fixture f;
  native::FrameBuilder builder(f.memory.data(), nullptr,
                               native::SceneInputs::SinglePass);
  Expect(!builder.has_output_copy(),
         "A repeated present must not produce a new output frame");
  const float clear[] = {0, 0, 1, 0, 0, 0, 1,
                        1920, 0, 1, 0, 0, 0, 1,
                        1920, 1080, 1, 0, 0, 0, 1};
  std::memcpy(f.memory.data() + 0x4000, clear, sizeof(clear));
  auto r = f.Registers();
  r[0x4800] = 0x4003;
  r[0x4801] = 21 << 2;
  r[0x2200] = 0x76;
  builder.DrawGuest(r, f.clear_vs, f.clear_ps, 8, 3, false);
  Expect(!builder.has_output_copy(),
         "Drawing must not replace the displayed buffer before its copy");
  r[0x2318] = 0x100000;
  builder.CopyGuest(r);
  Expect(builder.has_output_copy(),
         "An intentional clear-only output must be presented");
  auto frame = builder.Finish();
  Expect(frame.draws.empty() &&
             frame.ui_clear.color == std::array<float, 4>{0, 0, 0, 1},
         "Clear-only output must retain an intentional black image");
  Expect(!builder.has_output_copy(),
         "The next swap must reuse output until another copy completes");
}

void ChangedTiles() {
  Fixture f;
  native::FrameBuilder builder(f.memory.data());
  f.Clear(builder);
  for (uint32_t tile = 0; tile < 7; ++tile) {
    auto r = f.Registers(2);
    r[0x2080] = ((-native::kTileEdges[tile]) & 32767) << 16;
    r[0x2081] = native::kTileEdges[tile] << 16;
    r[0x2082] = (native::kTileEdges[tile + 1] << 16) | native::kWidth;
    const float x = tile == 1 ? .25f : 0;
    std::memcpy(f.memory.data() + 0x1000, &x, sizeof(x));
    if (tile == 2)
      r[0x2201] = 0x1000706;
    builder.DrawGuest(r, f.vs, f.ps, 13, 4, false);
    f.Copy(builder, tile);
  }
  auto frame = builder.Finish();
  Expect(frame.draws.size() == 7, "Changed tiles must retain separate draws");
  for (uint32_t tile = 0; tile < 7; ++tile) {
    const auto &draw = frame.draws[tile];
    Expect(draw.scissor.top == native::kTileEdges[tile] &&
               draw.scissor.bottom == native::kTileEdges[tile + 1],
           "Changed tiles must keep their screen bands");
  }
  Expect(Position(frame.draws[1]) == .25f &&
             frame.draws[2].blend_control == 0x1000706,
         "Tile verification must compare vertices and render state");
}

void RejectedArrangements() {
  Fixture f;
  native::FrameBuilder builder(f.memory.data(), nullptr,
                               native::SceneInputs::SinglePass);
  f.Clear(builder);
  auto r = f.Registers(2);
  --r[0x2082];
  Reject([&] { builder.DrawGuest(r, f.vs, f.ps, 13, 4, false); },
         "Unrecognized world scissors must fail loudly");
  native::FrameBuilder ui_first(f.memory.data(), nullptr,
                                native::SceneInputs::SinglePass);
  ui_first.DrawGuest(f.Registers(), f.vs, f.ps, 13, 4, false);
  Reject([&] { f.Clear(ui_first); }, "A world pass after UI must be rejected");
}

void Serialization(const std::filesystem::path &path) {
  Fixture f;
  native::FrameBuilder builder(f.memory.data(), nullptr,
                               native::SceneInputs::SinglePass);
  builder.DrawGuest(f.Registers(), f.vs, f.ps, 13, 4, false);
  auto original = builder.Finish();
  native::SaveFrame(original, path);
  auto restored = native::LoadFrame(path);
  std::filesystem::remove(path);
  Expect(restored.draws == original.draws &&
             restored.ui_clear == original.ui_clear &&
             restored.gamma == original.gamma &&
             restored.textures.size() == 1 &&
             restored.textures[0]->pixels == original.textures[0]->pixels,
         "Frame serialization must preserve the builder's named state");
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  try {
    OwnedVerticesAndQuads();
    TextureReuploads();
    Constants();
    ClearOnly();
    OutputCopies();
    ChangedTiles();
    RejectedArrangements();
    Serialization(argv[1]);
    std::cout << "8 native frame builder cases passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
