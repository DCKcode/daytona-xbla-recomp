#include "native_frame_builder.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <rex/graphics/pipeline/texture/util.h>
#include <stdexcept>
#include <xxhash.h>

namespace daytona::native {
namespace gpu = rex::graphics;
namespace xenos = gpu::xenos;
namespace {
void Require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
uint32_t Swap(uint32_t value, uint32_t endian) {
  return xenos::GpuSwap(value, xenos::Endian(endian));
}
std::array<float, 4> DecodeVertex(const uint8_t *source,
                                  xenos::VertexFormat format, uint32_t endian) {
  std::array<float, 4> value{};
  if (format == xenos::VertexFormat::k_8_8_8_8) {
    uint32_t word;
    std::memcpy(&word, source, 4);
    word = Swap(word, endian);
    for (uint32_t i = 0; i < 4; ++i)
      value[i] = float((word >> (i * 8)) & 255) / 255;
  } else {
    uint32_t count = xenos::GetVertexFormatComponentCount(format);
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t word;
      std::memcpy(&word, source + i * 4, 4);
      value[i] = std::bit_cast<float>(Swap(word, endian));
    }
  }
  return value;
}
std::array<uint8_t, 4> RGB565(uint32_t c) {
  uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
  return {uint8_t((r << 3) | (r >> 2)), uint8_t((g << 2) | (g >> 4)),
          uint8_t((b << 3) | (b >> 2)), 255};
}
std::array<std::array<uint8_t, 4>, 16> DecodeBC(const uint8_t *bytes,
                                                uint32_t format) {
  const auto *color = bytes + (format == 18 ? 0 : 8);
  uint16_t c0, c1;
  uint32_t bits;
  std::memcpy(&c0, color, 2);
  std::memcpy(&c1, color + 2, 2);
  std::memcpy(&bits, color + 4, 4);
  std::array<std::array<uint8_t, 4>, 4> palette{RGB565(c0), RGB565(c1)};
  if (c0 > c1 || format != 18) {
    for (uint32_t i = 0; i < 3; ++i) {
      palette[2][i] = (2 * palette[0][i] + palette[1][i]) / 3;
      palette[3][i] = (palette[0][i] + 2 * palette[1][i]) / 3;
    }
    palette[2][3] = palette[3][3] = 255;
  } else {
    for (uint32_t i = 0; i < 3; ++i)
      palette[2][i] = (palette[0][i] + palette[1][i]) / 2;
    palette[2][3] = 255;
  }
  std::array<std::array<uint8_t, 4>, 16> pixels;
  for (uint32_t i = 0; i < 16; ++i)
    pixels[i] = palette[(bits >> (2 * i)) & 3];
  uint64_t alpha;
  std::memcpy(&alpha, bytes, 8);
  if (format == 19)
    for (uint32_t i = 0; i < 16; ++i)
      pixels[i][3] = ((alpha >> (4 * i)) & 15) * 17;
  if (format == 20) {
    std::array<uint8_t, 8> values{bytes[0], bytes[1]};
    if (values[0] > values[1]) {
      for (uint32_t i = 1; i < 7; ++i)
        values[i + 1] = ((7 - i) * values[0] + i * values[1]) / 7;
    } else {
      for (uint32_t i = 1; i < 5; ++i)
        values[i + 1] = ((5 - i) * values[0] + i * values[1]) / 5;
      values[6] = 0;
      values[7] = 255;
    }
    for (uint32_t i = 0; i < 16; ++i)
      pixels[i][3] = values[(alpha >> (16 + i * 3)) & 7];
  }
  return pixels;
}
void Constants(const gpu::RegisterFile &r, gpu::Shader &shader, uint32_t base,
               std::array<uint8_t, 4096> &result) {
  const auto &map = shader.constant_register_map();
  bool bools = false;
  for (auto bits : map.bool_bitmap)
    bools |= bits != 0;
  Require(
      !shader.memexport_eM_written() && !bools && !map.loop_bitmap &&
          !map.float_dynamic_addressing,
      "Native shaders require static float constants and no memory exports");
  for (uint32_t group = 0; group < 4; ++group) {
    uint64_t bits = map.float_bitmap[group];
    while (bits) {
      const uint32_t i = group * 64 + std::countr_zero(bits);
      bits &= bits - 1;
      if (i == 255) {
        const float helper[] = {0, 1, 0, 1};
        std::memcpy(result.data() + i * 16, helper, 16);
      } else {
        for (uint32_t j = 0; j < 4; ++j) {
          const uint32_t word = r[base + i * 4 + j];
          std::memcpy(result.data() + i * 16 + j * 4, &word, 4);
        }
      }
    }
  }
}
std::array<uint32_t, 4> Scissor(const gpu::RegisterFile &r) {
  return {r[0x2081] & 32767, (r[0x2081] >> 16) & 32767, r[0x2082] & 32767,
          (r[0x2082] >> 16) & 32767};
}
} // namespace
FrameBuilder::FrameBuilder(const uint8_t *memory, rex::memory::Memory *guest,
                           SceneInputs scene_inputs)
    : memory_(memory), scene_inputs_(scene_inputs), guest_(guest) {
  if (guest_) {
    watch_ = guest_->RegisterPhysicalMemoryInvalidationCallback(
        [](void *context, uint32_t address, uint32_t length, bool) {
          static_cast<FrameBuilder *>(context)->texture_generation_.fetch_add(
              1, std::memory_order_release);
          return std::pair{address, length};
        },
        this);
  }
}
FrameBuilder::~FrameBuilder() {
  if (watch_)
    guest_->UnregisterPhysicalMemoryInvalidationCallback(watch_);
}
const uint8_t *FrameBuilder::Read(uint32_t address, size_t size) const {
  Require(address <= 0x20000000 && size <= 0x20000000 - address,
          "Native source exceeds physical memory");
  return memory_ + address;
}
const std::vector<FrameBuilder::VertexAttribute> &
FrameBuilder::Attributes(const gpu::Shader &shader) {
  auto [entry, inserted] = attributes_.try_emplace(shader.ucode_data_hash());
  if (!inserted)
    return entry->second;
  auto &result = entry->second;
  for (const auto &binding : shader.vertex_bindings())
    for (const auto &attribute : binding.attributes) {
      const auto &a = attribute.fetch_instr.attributes;
      auto format = a.data_format;
      Require((format == xenos::VertexFormat::k_8_8_8_8 && !a.is_signed &&
               !a.is_integer) ||
                  ((format == xenos::VertexFormat::k_32_FLOAT ||
                    format == xenos::VertexFormat::k_32_32_FLOAT ||
                    format == xenos::VertexFormat::k_32_32_32_32_FLOAT ||
                    format == xenos::VertexFormat::k_32_32_32_FLOAT) &&
                   a.is_signed && a.is_integer),
              "Unsupported native vertex interpretation");
      result.push_back({binding.fetch_constant, binding.stride_words,
                        uint32_t(a.offset), format});
    }
  Require(!result.empty() && result.size() <= 8,
          "Unsupported native vertex attribute count");
  return result;
}
std::array<float, 4> FrameBuilder::Vertex(const Registers &r,
                                          const VertexAttribute &a,
                                          uint32_t index) {
  uint32_t f0 = r[0x4800 + a.fetch * 2], f1 = r[0x4801 + a.fetch * 2];
  Require((f0 & 3) == 3, "Expected native vertex fetch constant");
  uint64_t address =
      uint64_t(f0 & ~3u) + 4ull * (uint64_t(index) * a.stride + a.offset);
  Require(address < 0x20000000, "Invalid native vertex address");
  uint32_t bytes = a.format == xenos::VertexFormat::k_8_8_8_8
                       ? 4
                       : 4 * xenos::GetVertexFormatComponentCount(a.format);
  return DecodeVertex(Read(address, bytes), a.format, f1 & 3);
}
void FrameBuilder::DecodeLevel(Texture &texture,
                               const std::array<uint32_t, 6> &f, uint32_t w,
                               uint32_t h, uint32_t address, uint32_t pitch,
                               uint32_t ox, uint32_t oy) {
  uint32_t format = f[1] & 63, endian = (f[1] >> 6) & 3,
           swizzle = (f[3] >> 1) & 4095;
  bool compressed = format >= 18, tiled = f[0] >> 31;
  uint32_t block = compressed ? 4 : 1, bs = format == 18   ? 8
                                            : compressed   ? 16
                                            : format == 10 ? 2
                                                           : 4;
  size_t start = texture.pixels.size();
  texture.pixels.resize(start + size_t(w) * h * 4);
  uint32_t bw = (w + block - 1) / block, bh = (h + block - 1) / block;
  for (uint32_t by = 0; by < bh; ++by)
    for (uint32_t bx = 0; bx < bw; ++bx) {
      uint32_t offset =
          tiled ? gpu::texture_util::GetTiledOffset2D(bx + ox, by + oy, pitch,
                                                      std::countr_zero(bs))
                : ((by + oy) * pitch + bx + ox) * bs;
      // Fetch endian applies to whole dwords even for an individual RG8 texel.
      uint32_t word_base = (address + offset) & ~3u,
               within = (address + offset) & 3;
      std::array<uint8_t, 16> bytes{};
      for (uint32_t i = 0; i < std::max(bs, 4u); i += 4) {
        uint32_t word;
        std::memcpy(&word, Read(word_base + i, 4), 4);
        word = Swap(word, endian);
        std::memcpy(bytes.data() + i, &word, 4);
      }
      std::array<std::array<uint8_t, 4>, 16> pixels{};
      if (compressed)
        pixels = DecodeBC(bytes.data(), format);
      else if (format == 10)
        pixels[0] = {bytes[within], bytes[within + 1], bytes[within + 1],
                     bytes[within + 1]};
      else
        std::copy_n(bytes.begin(), 4, pixels[0].begin());
      for (uint32_t i = 0; i < (compressed ? 16u : 1u); ++i) {
        uint32_t x = compressed ? bx * 4 + i % 4 : bx,
                 y = compressed ? by * 4 + i / 4 : by;
        if (x >= w || y >= h)
          continue;
        std::array<uint8_t, 8> channels{pixels[i][0],
                                        pixels[i][1],
                                        pixels[i][2],
                                        pixels[i][3],
                                        0,
                                        255,
                                        0,
                                        0};
        for (uint32_t c = 0; c < 4; ++c)
          texture.pixels[start + (size_t(y) * w + x) * 4 + c] =
              channels[(swizzle >> (c * 3)) & 7];
      }
    }
}
uint32_t
FrameBuilder::TextureGuest(const Registers &r,
                           const gpu::Shader::TextureBinding &binding) {
  uint32_t slot = binding.fetch_constant;
  Require(slot < 2,
          "Native renderer supports only pixel texture slots zero and one");
  auto fetch = r.GetTextureFetch(slot);
  auto f = std::bit_cast<std::array<uint32_t, 6>>(fetch);
  const auto &a = binding.fetch_instr.attributes;
  uint32_t mag = uint32_t(a.mag_filter), min = uint32_t(a.min_filter),
           mip = uint32_t(a.mip_filter), aniso = uint32_t(a.aniso_filter);
  if (mag == 3)
    mag = (f[3] >> 19) & 3;
  if (min == 3)
    min = (f[3] >> 21) & 3;
  if (mip == 3)
    mip = (f[3] >> 23) & 3;
  if (aniso == 7)
    aniso = (f[3] >> 25) & 7;
  uint32_t cu = (f[0] >> 10) & 7, cv = (f[0] >> 13) & 7,
           border = (cu == 6 || cv == 6) ? f[5] & 3 : 0;
  Require(mag < 2 && min < 2 && aniso < 2 && mip < 3,
          "Unsupported native texture filtering");
  Require((cu < 3 || cu == 6) && (cv < 3 || cv == 6) && border < 2,
          "Unsupported native texture addressing");
  Require((f[0] & 3) == 2 && ((f[5] >> 9) & 3) == 1 && !(f[1] & (1 << 10)),
          "Only unstacked 2D textures are supported");
  Require(!((f[0] >> 2) & 255) && !(f[3] & 1) && !((f[3] >> 13) & 63),
          "Signed, integer or exponent-adjusted textures are unsupported");
  uint32_t format = f[1] & 63, address = f[1] & 0x1ffff000,
           w = (f[2] & 8191) + 1, h = ((f[2] >> 13) & 8191) + 1;
  Require(format == 6 || format == 10 || format == 18 || format == 19 ||
              format == 20,
          "Unsupported native texture format");
  Require(!(f[4] & 0x3c), "Disabled texture base mip is unsupported");
  uint32_t sampler = mag | (min << 1) | (cu << 2) | (cv << 5) | (border << 8);
  auto texture = std::make_shared<Texture>();
  texture->width = w;
  texture->height = h;
  texture->sampler = sampler;
  if (resolves_.contains(address)) {
    uint32_t swizzle = (f[3] >> 1) & 4095;
    Require(address == first_resolve_ && w == kWidth && h == kHeight &&
                format == 6 && (swizzle == 0x60a || swizzle == 0xa0a),
            "Unsupported native resolve fetch");
    texture->kind =
        swizzle == 0xa0a ? TextureKind::OpaqueScene : TextureKind::Scene;
    texture->id = AllocateTextureId();
  } else {
    Require(!(f[5] & (1 << 11)) || std::min(w, h) > 16,
            "Packed base mip is unverified");
    uint32_t width, height, depth, base_page, mip_page, mip_min, mip_max;
    gpu::texture_util::GetSubresourcesFromFetchConstant(
        fetch, &width, &height, &depth, &base_page, &mip_page, &mip_min,
        &mip_max);
    mip_max = (mip == 2 || !mip_page) ? 0 : mip_max;
    const auto layout = gpu::texture_util::GetGuestTextureLayout(
        fetch.dimension, fetch.pitch, w, h, 1, fetch.tiled, fetch.format,
        fetch.packed_mips, true, mip_max);
    texture->levels = mip_max + 1;
    texture->sampler |= (uint32_t(mip == 1) << 9) | (mip_max << 10) |
                        (((f[4] >> 12) & 1023) << 14);
    std::array<uint32_t, 7> key;
    std::copy(f.begin(), f.end(), key.begin());
    key[6] = texture->sampler;
    const uint64_t generation =
        texture_generation_.load(std::memory_order_acquire);
    auto found = textures_.find(key);
    uint64_t digest;
    if (guest_ && found != textures_.end() &&
        found->second.generation == generation) {
      digest = found->second.digest;
    } else {
      // Reused descriptors are safe to cache only while their source pages
      // remain protected from guest writes.
      if (guest_) {
        guest_->EnablePhysicalMemoryAccessCallbacks(
            address, layout.base.level_data_extent_bytes, true, false);
        if (mip_max)
          guest_->EnablePhysicalMemoryAccessCallbacks(
              mip_page << 12, layout.mips_total_extent_bytes, true, false);
      }
      ++work_counts_.hashes;
      digest = XXH3_64bits(Read(address, layout.base.level_data_extent_bytes),
                           layout.base.level_data_extent_bytes);
      if (mip_max)
        digest = XXH3_64bits_withSeed(
            Read(mip_page << 12, layout.mips_total_extent_bytes),
            layout.mips_total_extent_bytes, digest);
    }
    if (found != textures_.end() && found->second.digest == digest) {
      found->second.frame = frame_number_;
      found->second.generation = generation;
      if (auto index = texture_indices_.find(found->second.texture->id);
          index != texture_indices_.end()) {
        return index->second;
      }
      uint32_t index = frame_.textures.size();
      frame_.textures.push_back(found->second.texture);
      texture_indices_[found->second.texture->id] = index;
      return index;
    }
    ++work_counts_.decodes;
    auto *info = gpu::FormatInfo::Get(fetch.format);
    for (uint32_t level = 0; level <= mip_max; ++level) {
      uint32_t storage_level = std::min(level, layout.packed_level), x = 0,
               y = 0, z = 0;
      const auto &storage = level ? layout.mips[storage_level] : layout.base;
      if (fetch.packed_mips)
        gpu::texture_util::GetPackedMipOffset(w, h, 1, fetch.format, level, x,
                                              y, z);
      DecodeLevel(
          *texture, f, std::max(w >> level, 1u), std::max(h >> level, 1u),
          level ? (mip_page << 12) + layout.mip_offsets_bytes[storage_level]
                : address,
          storage.row_pitch_bytes / info->bytes_per_block(), x, y);
    }
    texture->id = AllocateTextureId();
    textures_[key] = {digest, texture, frame_number_, generation};
  }
  uint32_t index = frame_.textures.size();
  frame_.textures.push_back(texture);
  texture_indices_[texture->id] = index;
  return index;
}
void FrameBuilder::DrawGuest(const Registers &r, gpu::Shader &vs,
                             gpu::Shader &ps, uint32_t primitive,
                             uint32_t count, bool indexed) {
  uint64_t vhash = vs.ucode_data_hash(), phash = ps.ucode_data_hash();
  if (vhash == kSyncVertexShader)
    return;
  Require(!indexed, "Indexed native draws are not verified");
  uint32_t samples = (r[0x2000] >> 16) & 3;
  Require(samples == 0 || samples == 2, "Unsupported native sample count");
  bool world = samples == 2;
  if (vhash == kClearVertexShader) {
    const auto &attrs = Attributes(vs);
    if (!(primitive == 8 && count == 3 && attrs.size() == 2 &&
          phash == kClearPixelShader))
      throw std::runtime_error("Unsupported native clear draw: primitive=" +
                               std::to_string(primitive) +
                               " count=" + std::to_string(count) +
                               " attributes=" + std::to_string(attrs.size()) +
                               " ps=" + std::to_string(phash) +
                               " depth=" + std::to_string(r[0x2200]));
    Require(world ? !seen_ui_ && tile_ == 0 && scene_[0].empty() &&
                        frame_.draws.empty()
                  : !seen_ui_,
            "Native clear after drawing");
    auto color = Vertex(r, attrs[1], 0), position = Vertex(r, attrs[0], 0);

    const auto second = Vertex(r, attrs[0], 1), third = Vertex(r, attrs[0], 2);
    // During startup, the guest clears the last eight output rows, then clears
    // the remaining pixels through a 960x536 four-sample view of the same
    // memory. Together these are one full output clear; no scene resolve is
    // involved.
    if (!world && position[0] == 0 && position[1] == 1072 &&
        second[0] == 1920 && second[1] == 1072 && third[0] == 1920 &&
        third[1] == 1080 && ui_.empty()) {
      initialization_clear_ = true;
    } else if (world && (r[0x2000] & 0x3fff) == 960) {
      Require(initialization_clear_ && position[0] == 0 && position[1] == 0 &&
                  second[0] == 960 && second[1] == 0 && third[0] == 960 &&
                  third[1] == 536 &&
                  std::equal(color.begin(), color.end(),
                             frame_.ui_clear.color.begin()),
              "Unverified startup clear views");
      initialization_clear_ = false;
      return;
    }
    for (uint32_t i = 1; i < 3; ++i)
      Require(
          Vertex(r, attrs[1], i) == color &&
              (!(r[0x2200] & 4) || Vertex(r, attrs[0], i)[2] == position[2]),
          "Varying native clear");
    for (float value : color)
      Require(std::isfinite(value), "Non-finite native clear");
    Require(!(r[0x2200] & 4) || (position[2] >= 0 && position[2] <= 1),
            "Invalid native depth clear");
    auto &clear = world ? frame_.world_clear : frame_.ui_clear;
    clear.color = color;
    clear.depth = r[0x2200] & 4 ? position[2] : 1;
    frame_.world |= world;
    return;
  }
  const auto &attrs = Attributes(vs);
  Require(!final_copy_, "Native drawing after output copy");
  Require(r[0x2206] == 0x43f && r[0x2204] == 0x80000,
          "Unsupported native clip convention");
  Require(!(r[0x2200] & 1) && !(r[0x2202] & 0x18),
          "Native stencil or alpha test is unverified");
  std::optional<Draw> temporary;
  if (scene_inputs_ != SceneInputs::SinglePass)
    temporary.emplace();
  auto &draw = scene_inputs_ == SceneInputs::SinglePass
                   ? frame_.draws.emplace_back()
                   : *temporary;
  draw.primitive = Primitive(primitive);
  draw.programs = {vhash, phash};
  if (world) {
    Require(!seen_ui_ && frame_.world && tile_ < SceneTileCount(),
            "Unexpected native world pass");
    const auto bottom = scene_inputs_ == SceneInputs::SinglePass
                            ? kHeight
                            : kTileEdges[tile_ + 1];
    Require(Scissor(r) == std::array<uint32_t, 4>{0, kTileEdges[tile_], kWidth,
                                                  bottom} &&
                r[0x2080] == (((-kTileEdges[tile_]) & 32767) << 16),
            "Unverified native world scissor or offset");
    draw.scissor = {0, 0, kWidth, kHeight};
  } else {
    Require(!frame_.world || tile_ == SceneTileCount(),
            "Native UI before world resolve");
    Require(!r[0x2080], "Native UI window offset is unverified");
    seen_ui_ = true;
    auto rect = Scissor(r);
    draw.scissor = {rect[0], rect[1], rect[2], rect[3]};
  }
  Require((primitive == 4 && count % 3 == 0) ||
              (primitive == 6 && count >= 3) ||
              (primitive == 13 && count % 4 == 0),
          "Unverified native primitive topology");
  static constexpr uint32_t locations[] = {4, 5, 6, 7, 12, 13, 14, 15};
  std::copy_n(locations, attrs.size(), draw.locations.begin());
  draw.vertex_count = count;
  draw.attribute_count = attrs.size();
  draw.blend_control = r[0x2201];
  draw.depth_control = r[0x2200];
  draw.color_mask = r[0x2104];
  draw.raster_control = r[0x2205];
  draw.world = world;
  for (uint32_t i = 0; i < 6; ++i)
    draw.viewport[i] = std::bit_cast<float>(r[0x210f + i]);
  std::array<const uint8_t *, 96> sources{};
  for (const auto &binding : vs.vertex_bindings()) {
    uint32_t fetch = binding.fetch_constant;
    Require(fetch < sources.size(), "Invalid native vertex fetch slot");
    uint32_t f0 = r[0x4800 + fetch * 2];
    Require((f0 & 3) == 3, "Expected vertex source buffer");
    uint64_t address = uint64_t(f0 & ~3u) +
                       4ull * (r[0x2102] & 0xffffff) * binding.stride_words;
    uint64_t length = 4ull * count * binding.stride_words;
    Require(address < 0x20000000 && length <= 0x20000000 - address,
            "Invalid vertex source range");
    sources[fetch] = Read(address, length);
  }
  Require(vs.texture_bindings().empty(),
          "Native vertex texture sampling is unverified");
  uint32_t slots = 0;
  for (const auto &binding : ps.texture_bindings()) {
    uint32_t slot = binding.fetch_constant;
    Require(slot < 2 && !(slots & (1u << slot)),
            "Multiple native samplers per slot are unverified");
    slots |= 1u << slot;
    draw.textures[slot] = TextureGuest(r, binding);
    Require(!world || frame_.textures[draw.textures[slot]]->kind ==
                          TextureKind::Image,
            "Native world resolve feedback is unverified");
  }
  Constants(r, vs, 0x4000, draw.vs_constants);
  Constants(r, ps, 0x4400, draw.ps_constants);
  // The translated UBO stores texture indices at 0 and sampler indices at 192.
  uint32_t one = 1;
  std::memcpy(draw.shared.data() + 4, &one, 4);
  std::memcpy(draw.shared.data() + 196, &one, 4);
  for (uint32_t slot = 0; slot < 2; ++slot) {
    if (!(slots & (1u << slot)))
      continue;
    auto f = r.GetTextureFetch(slot);
    if (uint32_t(f.format) == 10 && f.swizzle == 0x200)
      std::memcpy(draw.shared.data() + 288 + slot * 4, &one, 4);
  }
  const size_t vertex_bytes = attrs.size() * 16;
  auto decode = [&](uint32_t relative, uint8_t *dest) {
    for (const auto &attr : attrs) {
      const auto *source =
          sources[attr.fetch] + 4 * (relative * attr.stride + attr.offset);
      auto value =
          DecodeVertex(source, attr.format, r[0x4801 + attr.fetch * 2] & 3);
      std::memcpy(dest, value.data(), 16);
      dest += 16;
    }
  };
  draw.vertices.resize(size_t(count) * vertex_bytes);
  for (uint32_t vertex = 0; vertex < count; ++vertex)
    decode(vertex, draw.vertices.data() + vertex * vertex_bytes);
  if (scene_inputs_ == SceneInputs::SinglePass)
    return;
  if (world)
    scene_[tile_].push_back(std::move(draw));
  else
    ui_.push_back(std::move(draw));
}
void FrameBuilder::CopyGuest(const Registers &r) {
  uint32_t samples = (r[0x2000] >> 16) & 3;
  if (samples == 2) {
    Require(frame_.world && tile_ < SceneTileCount() && !seen_ui_,
            "Unexpected native world resolve");
    uint32_t address = r[0x2319];
    if (!tile_)
      first_resolve_ = address;
    Require(address == first_resolve_ + kWidth * kTileEdges[tile_] * 4,
            "Noncontiguous native world resolve");
    Require(r[0x231a] == 0x4380780 && r[0x231b] == 0x1000300,
            "Unverified native world resolve format");
    if (r[0x2318] != (tile_ + 1 == SceneTileCount() ? 0x100060u : 0x100360u))
      throw std::runtime_error("Unverified native world resolve operation: " +
                               std::to_string(r[0x2318]) + " tile " +
                               std::to_string(tile_) + " count " +
                               std::to_string(SceneTileCount()));
    if (tile_ + 1 < SceneTileCount()) {
      Require(r[0x231d] == 0xffffff00 && r[0x231e] == r[0x231f],
              "Unverified native resolve clear");
      auto &clear = frame_.tile_clears[tile_];
      for (uint32_t i = 0; i < 4; ++i)
        clear.color[i] = float((r[0x231e] >> (i * 8)) & 255) / 255;
      clear.depth = 1;
    }
    resolves_.insert(address);
    ++tile_;
  } else {
    Require(samples == 0 && (!frame_.world || tile_ == SceneTileCount()),
            "Incomplete native world resolve");
    Require(!final_copy_ && r[0x2318] == 0x100000,
            "Unverified native output copy");
    final_copy_ = true;
  }
}
Frame FrameBuilder::Finish() {
  if (scene_inputs_ != SceneInputs::SinglePass)
    frame_.draws.reserve(scene_[0].size() + ui_.size());
  Require(!frame_.world || tile_ == SceneTileCount(),
          "Incomplete native world frame");
  if (frame_.world && scene_inputs_ == SceneInputs::SinglePass) {
    for (auto &clear : frame_.tile_clears)
      clear = frame_.world_clear;
  } else if (frame_.world) {
    for (const auto &tile : scene_)
      Require(tile.size() == scene_[0].size(),
              "Native tile draw counts differ");
    for (size_t index = 0; index < scene_[0].size(); ++index) {
      auto &first = scene_[0][index];
      bool identical = true;
      for (uint32_t tile = 1; tile < 7; ++tile)
        identical &= scene_[tile][index] == first;
      if (identical)
        frame_.draws.push_back(std::move(first));
      else {
        for (uint32_t tile = 0; tile < 7; ++tile) {
          auto &draw = scene_[tile][index];
          draw.scissor.top = kTileEdges[tile];
          draw.scissor.bottom = kTileEdges[tile + 1];
          frame_.draws.push_back(std::move(draw));
        }
      }
    }
  }
  for (auto &draw : ui_)
    frame_.draws.push_back(std::move(draw));
  draw_capacity_ = std::max(draw_capacity_, frame_.draws.size());
  Frame result = std::move(frame_);
  frame_ = Frame{};
  if (scene_inputs_ == SceneInputs::SinglePass)
    frame_.draws.reserve(draw_capacity_);
  for (auto &tile : scene_)
    tile.clear();
  ui_.clear();
  resolves_.clear();
  texture_indices_.clear();
  first_resolve_ = tile_ = 0;
  seen_ui_ = final_copy_ = initialization_clear_ = false;
  ++frame_number_;
  for (auto it = textures_.begin(); it != textures_.end();) {
    if (frame_number_ - it->second.frame > 120)
      it = textures_.erase(it);
    else
      ++it;
  }
  return result;
}
} // namespace daytona::native
