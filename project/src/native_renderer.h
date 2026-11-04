#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>
#include <vulkan/vulkan.h>

namespace daytona::native {
inline constexpr uint32_t kWidth = 1920, kHeight = 1080;
inline constexpr std::array<uint32_t, 8> kTileEdges = {0,   160, 320, 480,
                                                       640, 800, 928, 1080};
// These fixed guest programs implement synchronization and clear rectangles.
inline constexpr uint64_t kSyncVertexShader = 0xb6c9863f710683ec;
inline constexpr uint64_t kClearVertexShader = 0x1e6883fccde1f688;
inline constexpr uint64_t kClearPixelShader = 0xa4a965c189287b99;
uint64_t AllocateTextureId();
enum class TextureKind : uint32_t { Image = 0, Scene = 1, OpaqueScene = 3 };
struct Texture {
  uint32_t width = 1, height = 1, sampler = 0, levels = 1;
  TextureKind kind = TextureKind::Image;
  uint64_t id = 0;
  std::vector<uint8_t> pixels;
};
enum class Primitive : uint32_t {
  Triangles = 4,
  TriangleStrip = 6,
  Quads = 13
};
struct Draw {
  Primitive primitive = Primitive::Triangles;
  std::array<uint64_t, 2> programs{};
  uint32_t vertex_count = 0, attribute_count = 0;
  uint32_t blend_control = 0, depth_control = 0, color_mask = 0,
           raster_control = 0;
  bool world = false;
  std::array<float, 6> viewport{};
  struct Scissor {
    uint32_t left = 0, top = 0, right = 0, bottom = 0;
    bool operator==(const Scissor &) const = default;
  } scissor;
  std::array<uint32_t, 2> textures{};
  std::array<uint32_t, 8> locations{};
  std::array<uint8_t, 4096> vs_constants{}, ps_constants{};
  std::array<uint8_t, 512> shared{};
  std::vector<uint8_t> vertices;
  bool operator==(const Draw &) const = default;
};
struct ClearValue {
  std::array<float, 4> color{};
  float depth = 1;
  bool operator==(const ClearValue &) const = default;
};
struct Frame {
  uint32_t width = kWidth, height = kHeight;
  bool world = false;
  ClearValue ui_clear, world_clear;
  std::array<ClearValue, 6> tile_clears{};
  std::array<uint8_t, 768> gamma{};
  std::vector<std::shared_ptr<const Texture>> textures;
  std::vector<Draw> draws;
  Frame() {
    for (size_t i = 0; i < gamma.size(); ++i)
      gamma[i] = i / 3;
  }
  size_t UploadSize() const {
    size_t bytes = 0;
    for (const auto &draw : draws)
      bytes += draw.vs_constants.size() + draw.ps_constants.size() +
               draw.shared.size() + draw.vertices.size();
    return bytes;
  }
};
Frame LoadFrame(const std::filesystem::path &path);
void SaveFrame(const Frame &frame, const std::filesystem::path &path);
struct Output {
  VkImage image = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  bool initialized = false;
};
struct RenderTimings {
  double record_ms = 0, queue_ms = 0;
};
class Renderer {
public:
  Renderer();
  Renderer(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
           uint32_t family, PFN_vkGetInstanceProcAddr get_instance_proc_addr);
  ~Renderer();
  std::vector<uint8_t> Render(const Frame &frame,
                              const std::filesystem::path &shaders = {},
                              Output target = {},
                              RenderTimings *timings = nullptr);

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace daytona::native
