#pragma once
#include "native_renderer.h"
#include <atomic>
#include <map>
#include <optional>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/register_file.h>
#include <rex/memory.h>
#include <set>

namespace daytona::native {
enum class SceneInputs { VerifyEveryTile, SinglePass };
class FrameBuilder {
public:
  explicit FrameBuilder(
      const uint8_t *memory, rex::memory::Memory *guest = nullptr,
      SceneInputs scene_inputs = SceneInputs::VerifyEveryTile);
  ~FrameBuilder();
  void DrawGuest(const rex::graphics::RegisterFile &registers,
                 rex::graphics::Shader &vs, rex::graphics::Shader &ps,
                 uint32_t primitive, uint32_t count, bool indexed);
  void CopyGuest(const rex::graphics::RegisterFile &registers);
  Frame Finish();
  struct WorkCounts {
    uint64_t hashes = 0, decodes = 0;
  };
  WorkCounts work_counts() const { return work_counts_; }
  uint64_t texture_generation() const { return texture_generation_.load(); }
  uint32_t tile_index() const { return tile_; }

private:
  using Registers = rex::graphics::RegisterFile;
  uint32_t SceneTileCount() const {
    return scene_inputs_ == SceneInputs::SinglePass ? 1 : 7;
  }
  const uint8_t *Read(uint32_t address, size_t size) const;
  struct VertexAttribute {
    uint32_t fetch, stride, offset;
    rex::graphics::xenos::VertexFormat format;
  };
  const std::vector<VertexAttribute> &
  Attributes(const rex::graphics::Shader &shader);
  std::array<float, 4> Vertex(const Registers &r,
                              const VertexAttribute &binding, uint32_t index);
  uint32_t TextureGuest(const Registers &r,
                        const rex::graphics::Shader::TextureBinding &binding);
  void DecodeLevel(Texture &texture, const std::array<uint32_t, 6> &fetch,
                   uint32_t width, uint32_t height, uint32_t address,
                   uint32_t pitch, uint32_t x, uint32_t y);
  const uint8_t *memory_;
  SceneInputs scene_inputs_;
  rex::memory::Memory *guest_ = nullptr;
  void *watch_ = nullptr;
  std::atomic<uint64_t> texture_generation_{0};
  WorkCounts work_counts_;
  Frame frame_;
  std::array<std::vector<Draw>, 7> scene_;
  std::map<uint64_t, std::vector<VertexAttribute>> attributes_;
  size_t draw_capacity_ = 0;
  std::vector<Draw> ui_;
  std::set<uint32_t> resolves_;
  uint32_t first_resolve_ = 0, tile_ = 0;
  bool seen_ui_ = false, final_copy_ = false, initialization_clear_ = false;
  struct CachedTexture {
    uint64_t digest;
    std::shared_ptr<const Texture> texture;
    uint64_t frame, generation;
  };
  std::map<std::array<uint32_t, 7>, CachedTexture> textures_;
  std::map<uint64_t, uint32_t> texture_indices_;
  uint64_t frame_number_ = 0;
};
} // namespace daytona::native
