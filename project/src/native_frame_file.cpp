#include "native_renderer.h"
#include <cmath>
#include <fstream>
#include <stdexcept>
namespace daytona::native {
// Keep the v4/v5 file layout independent of the named in-memory fields.
namespace {
constexpr uint32_t kLegacyWorldFlag = 0x10000000;
}
void SaveFrame(const Frame &frame, const std::filesystem::path &path) {
  std::ofstream output(path, std::ios::binary);
  output.exceptions(std::ios::failbit | std::ios::badbit);
  auto write = [&](const void *data, size_t size) {
    output.write(static_cast<const char *>(data), size);
  };
  const std::array<uint32_t, 7> header{0x314e5444,
                                       5,
                                       frame.width,
                                       frame.height,
                                       uint32_t(frame.draws.size()),
                                       uint32_t(frame.textures.size()),
                                       uint32_t(frame.world)};
  write(header.data(), sizeof(header));
  auto clear = [&](const ClearValue &value) {
    write(value.color.data(), sizeof(value.color));
    write(&value.depth, sizeof(value.depth));
  };
  clear(frame.ui_clear);
  clear(frame.world_clear);
  for (const auto &value : frame.tile_clears)
    clear(value);
  write(frame.gamma.data(), frame.gamma.size());
  for (const auto &texture : frame.textures) {
    const std::array<uint32_t, 6> t{texture->width,
                                    texture->height,
                                    uint32_t(texture->pixels.size()),
                                    uint32_t(texture->kind),
                                    texture->sampler,
                                    texture->levels};
    write(t.data(), sizeof(t));
    write(texture->pixels.data(), texture->pixels.size());
  }
  for (const auto &draw : frame.draws) {
    write(draw.programs.data(), sizeof(draw.programs));
    const uint32_t state[] = {
        draw.vertex_count,
        draw.attribute_count,
        draw.blend_control,
        draw.depth_control,
        draw.color_mask,
        draw.raster_control | (draw.world ? kLegacyWorldFlag : 0)};
    write(state, sizeof(state));
    write(draw.viewport.data(), sizeof(draw.viewport));
    const uint32_t rect[] = {draw.scissor.left,  draw.scissor.top,
                             draw.scissor.right, draw.scissor.bottom,
                             draw.textures[0],   draw.textures[1]};
    write(rect, sizeof(rect));
    write(&draw.primitive, sizeof(draw.primitive));
    write(draw.locations.data(), draw.attribute_count * 4);
    write(draw.vs_constants.data(), draw.vs_constants.size());
    write(draw.ps_constants.data(), draw.ps_constants.size());
    write(draw.shared.data(), draw.shared.size());
    write(draw.vertices.data(), draw.vertices.size());
  }
}
Frame LoadFrame(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  auto read = [&](void *data, size_t size) {
    if (!input.read(static_cast<char *>(data), size))
      throw std::runtime_error("Truncated native frame");
  };
  std::array<uint32_t, 7> header;
  read(header.data(), sizeof(header));
  if (header[0] != 0x314e5444 || (header[1] != 4 && header[1] != 5) ||
      header[6] > 1)
    throw std::runtime_error("Unsupported native frame");
  Frame frame;
  frame.width = header[2];
  frame.height = header[3];
  frame.world = header[6];
  auto clear = [&](ClearValue &value) {
    read(value.color.data(), sizeof(value.color));
    read(&value.depth, sizeof(value.depth));
    for (float channel : value.color)
      if (!std::isfinite(channel))
        throw std::runtime_error("Non-finite target clear");
    if (!std::isfinite(value.depth))
      throw std::runtime_error("Non-finite target clear");
    if (value.depth < 0 || value.depth > 1)
      throw std::runtime_error("Invalid depth clear");
  };
  clear(frame.ui_clear);
  clear(frame.world_clear);
  for (auto &value : frame.tile_clears)
    clear(value);
  read(frame.gamma.data(), frame.gamma.size());
  for (uint32_t i = 0; i < header[5]; ++i) {
    std::array<uint32_t, 6> t;
    read(t.data(), sizeof(t));
    auto texture = std::make_shared<Texture>();
    texture->width = t[0];
    texture->height = t[1];
    texture->kind = TextureKind(t[3]);
    texture->sampler = t[4];
    texture->levels = t[5];
    texture->id = AllocateTextureId();
    texture->pixels.resize(t[2]);
    read(texture->pixels.data(), texture->pixels.size());
    frame.textures.push_back(texture);
  }
  for (uint32_t i = 0; i < header[4]; ++i) {
    Draw d;
    read(d.programs.data(), sizeof(d.programs));
    uint32_t state[6];
    read(state, sizeof(state));
    d.vertex_count = state[0];
    d.attribute_count = state[1];
    d.blend_control = state[2];
    d.depth_control = state[3];
    d.color_mask = state[4];
    d.raster_control = state[5] & ~kLegacyWorldFlag;
    d.world = (state[5] & kLegacyWorldFlag) != 0;
    read(d.viewport.data(), sizeof(d.viewport));
    uint32_t rect[6];
    read(rect, sizeof(rect));
    d.scissor = {rect[0], rect[1], rect[2], rect[3]};
    d.textures = {rect[4], rect[5]};
    if (header[1] >= 5)
      read(&d.primitive, sizeof(d.primitive));
    if (d.attribute_count == 0 || d.attribute_count > d.locations.size() ||
        (d.primitive != Primitive::Triangles &&
         d.primitive != Primitive::TriangleStrip &&
         d.primitive != Primitive::Quads))
      throw std::runtime_error("Invalid native vertex layout");
    read(d.locations.data(), d.attribute_count * 4);
    read(d.vs_constants.data(), d.vs_constants.size());
    read(d.ps_constants.data(), d.ps_constants.size());
    read(d.shared.data(), d.shared.size());
    d.vertices.resize(size_t(d.vertex_count) * d.attribute_count * 16);
    read(d.vertices.data(), d.vertices.size());
    frame.draws.push_back(std::move(d));
  }
  return frame;
}
} // namespace daytona::native
