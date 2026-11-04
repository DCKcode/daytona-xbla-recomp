#include "render_probe.h"

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <vector>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/vulkan/command_processor.h>
#include <rex/graphics/vulkan/graphics_system.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/ui/presenter.h>
#include <rex/ui/renderdoc_api.h>

REXCVAR_DEFINE_STRING(daytona_render_probe_path, "", "Diagnostics",
                      "Write census and shaders here; create a capture file "
                      "there for an SDK screenshot and optional RenderDoc capture.");

namespace {
using rex::X_RESULT;
using rex::X_STATUS;
namespace gpu = rex::graphics;
namespace reg = rex::graphics::reg;
namespace xenos = rex::graphics::xenos;

class ProbeCommandProcessor final : public gpu::vulkan::VulkanCommandProcessor {
public:
  ProbeCommandProcessor(gpu::vulkan::VulkanGraphicsSystem *graphics,
                        rex::system::KernelState *kernel)
      : VulkanCommandProcessor(graphics, kernel),
        root_(REXCVAR_GET(daytona_render_probe_path)) {
    std::filesystem::create_directories(root_);
    census_.open(root_ / "frames.jsonl");
    renderdoc_ = rex::ui::RenderDocAPI::CreateIfConnected();
  }

protected:
  bool IssueDraw(xenos::PrimitiveType primitive, uint32_t count,
                 IndexBufferInfo *indices, bool explicit_mode) override {
    if (register_file_->Get<reg::RB_MODECONTROL>().edram_mode !=
        xenos::EdramMode::kCopy) {
      auto *vs = active_vertex_shader();
      auto *ps = active_pixel_shader();
      RecordShader(vs);
      RecordShader(ps);
      const auto &r = *register_file_;
      ++draws_[{vs ? vs->ucode_data_hash() : 0, ps ? ps->ucode_data_hash() : 0,
                uint32_t(primitive), r[reg::RB_SURFACE_INFO::register_index],
                r[reg::RB_COLOR_INFO::register_index],
                r[reg::RB_DEPTH_INFO::register_index],
                r[reg::RB_DEPTHCONTROL::register_index],
                r[reg::RB_BLENDCONTROL::register_index]}];
      return VulkanCommandProcessor::IssueDraw(
          primitive, count, indices, explicit_mode);
    }
    return VulkanCommandProcessor::IssueDraw(primitive, count, indices,
                                             explicit_mode);
  }

  bool IssueCopy() override {
    ++copies_;
    return VulkanCommandProcessor::IssueCopy();
  }

  void IssueSwap(uint32_t address, uint32_t width, uint32_t height) override {
    const auto finished_capture = capture_;
    if (!finished_capture.empty()) {
      rex::chrono::Clock::set_guest_time_scalar(capture_time_scale_);
      capture_.clear();
    }
    census_ << "{\"frame\":" << frame_++ << ",\"copies\":" << copies_
            << ",\"draws\":[";
    bool first = true;
    for (const auto &[key, count] : draws_) {
      if (!first)
        census_ << ',';
      first = false;
      census_ << '[' << count;
      for (auto value : key)
        census_ << ',' << value;
      census_ << ']';
    }
    census_ << "]}\n";
    if ((frame_ % 120) == 0)
      census_.flush();
    draws_.clear();
    copies_ = 0;

    std::error_code error;
    const bool start_capture =
        std::filesystem::remove(root_ / "capture", error);
    if (start_capture) {
      if (REXCVAR_GET(clock_no_scaling) || REXCVAR_GET(clock_source_raw))
        throw std::runtime_error("Capture requires a scalable guest clock");
      capture_time_scale_ = rex::chrono::Clock::guest_time_scalar();
      // Guest video interrupts can recycle dynamic buffers while recording
      // stalls the GPU worker. Slow them until this frame has been consumed.
      rex::chrono::Clock::set_guest_time_scalar(capture_time_scale_ * 0.01);
      capture_ = root_ / std::to_string(frame_);
      std::filesystem::create_directories(capture_);
    }
    VulkanCommandProcessor::IssueSwap(address, width, height);
    if (!finished_capture.empty()) {
      WriteReference(finished_capture);
      if (renderdoc_)
        renderdoc_->api_1_0_0()->EndFrameCapture(nullptr, nullptr);
    }
    if (start_capture && renderdoc_) {
      const auto path = (capture_ / "sdk").string();
      renderdoc_->api_1_0_0()->SetLogFilePathTemplate(path.c_str());
      renderdoc_->api_1_0_0()->StartFrameCapture(nullptr, nullptr);
    }
  }

private:
  void WriteReference(const std::filesystem::path &directory) {
    if (register_file_->GetTextureFetch(0).format !=
        xenos::TextureFormat::k_8_8_8_8)
      throw std::runtime_error("SDK reference capture requires 8-bit display output");
    std::array<uint8_t, 256 * 3> gamma;
    for (size_t i = 0; i < 256; ++i) {
      const auto &entry = gamma_ramp_256_entry_table()[i];
      gamma[i * 3] = (entry.color_10_red * 255 + 511) / 1023;
      gamma[i * 3 + 1] = (entry.color_10_green * 255 + 511) / 1023;
      gamma[i * 3 + 2] = (entry.color_10_blue * 255 + 511) / 1023;
    }
    std::ofstream lut(directory / "gamma.rgb", std::ios::binary);
    lut.exceptions(std::ios::failbit | std::ios::badbit);
    lut.write(reinterpret_cast<const char *>(gamma.data()), gamma.size());
    rex::ui::RawImage image;
    if (!graphics_system_->presenter()->CaptureGuestOutput(image))
      throw std::runtime_error("Cannot capture the SDK guest output");
    std::ofstream output(directory / "reference.rgba", std::ios::binary);
    output.exceptions(std::ios::failbit | std::ios::badbit);
    for (uint32_t y = 0; y < image.height; ++y)
      output.write(
          reinterpret_cast<const char *>(image.data.data() + y * image.stride),
          image.width * 4);
    std::ofstream(directory / "reference.json")
        << "{\"width\":" << image.width << ",\"height\":" << image.height
        << ",\"guest_time_scale\":" << capture_time_scale_ * 0.01 << "}";
  }

  void RecordShader(gpu::Shader *shader) {
    if (!shader || !shaders_.insert(shader->ucode_data_hash()).second)
      return;
    const auto name =
        fmt::format("{:016X}.{}", shader->ucode_data_hash(),
                    shader->type() == xenos::ShaderType::kVertex ? "vs" : "ps");
    std::ofstream binary(root_ / (name + ".bin"), std::ios::binary);
    for (auto word : shader->ucode_data()) {
      word = std::byteswap(word);
      binary.write(reinterpret_cast<const char *>(&word), sizeof(word));
    }
    rex::string::StringBuffer disassembly;
    if (!shader->is_ucode_analyzed())
      shader->AnalyzeUcode(disassembly);
    std::ofstream(root_ / (name + ".txt")) << shader->ucode_disassembly();
    std::ofstream metadata(root_ / (name + ".json"));
    metadata << "{\"memexport\":" << uint32_t(shader->memexport_eM_written())
             << ",\"vertex_bindings\":[";
    bool first = true;
    for (const auto &binding : shader->vertex_bindings()) {
      for (const auto &attribute : binding.attributes) {
        if (!first)
          metadata << ',';
        first = false;
        const auto &a = attribute.fetch_instr.attributes;
        metadata << '[' << binding.fetch_constant << ',' << binding.stride_words
                 << ',' << a.offset << ',' << uint32_t(a.data_format) << ','
                 << a.is_signed << ',' << a.is_integer << ']';
      }
    }
    metadata << "],\"texture_slots\":[";
    first = true;
    std::set<uint32_t> texture_slots;
    for (const auto &binding : shader->texture_bindings())
      texture_slots.insert(binding.fetch_constant);
    for (auto slot : texture_slots) {
      if (!first)
        metadata << ',';
      first = false;
      metadata << slot;
    }
    metadata << "],\"sampler_bindings\":[";
    first = true;
    for (const auto &binding : shader->texture_bindings()) {
      if (!first)
        metadata << ',';
      first = false;
      const auto &a = binding.fetch_instr.attributes;
      metadata << '[' << binding.fetch_constant << ',' << uint32_t(a.mag_filter)
               << ',' << uint32_t(a.min_filter) << ',' << uint32_t(a.mip_filter)
               << ',' << uint32_t(a.aniso_filter) << ']';
    }
    const auto &constants = shader->constant_register_map();
    metadata << "],\"float_constants\":[";
    first = true;
    for (uint32_t i = 0; i < 256; ++i) {
      if (!(constants.float_bitmap[i / 64] & (uint64_t(1) << (i % 64))))
        continue;
      if (!first)
        metadata << ',';
      first = false;
      metadata << i;
    }
    bool uses_bools = false;
    for (auto bits : constants.bool_bitmap)
      uses_bools |= bits != 0;
    metadata << "],\"unsupported_constants\":"
             << (uses_bools || constants.loop_bitmap ||
                 constants.float_dynamic_addressing)
             << "}";
  }

  std::filesystem::path root_, capture_;
  std::ofstream census_;
  std::map<std::array<uint64_t, 8>, uint32_t> draws_;
  std::set<uint64_t> shaders_;
  std::unique_ptr<rex::ui::RenderDocAPI> renderdoc_;
  double capture_time_scale_ = 1.0;
  uint32_t frame_ = 0, copies_ = 0;
};

class ProbeGraphicsSystem final : public gpu::vulkan::VulkanGraphicsSystem {
  std::unique_ptr<gpu::CommandProcessor> CreateCommandProcessor() override {
    return std::make_unique<ProbeCommandProcessor>(this, kernel_state_);
  }
};

// Headless captures need controller input even when the compositor has no seat.
class ProbeInputDriver final : public rex::input::InputDriver {
public:
  ProbeInputDriver()
      : InputDriver(nullptr, 0),
        path_(std::filesystem::path(REXCVAR_GET(daytona_render_probe_path)) /
              "input") {}
  rex::X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  void EnumerateDevices(std::vector<rex::input::DeviceInfo> &out) override {
    if (std::filesystem::exists(path_))
      out.push_back({kDevice, 0, "Daytona capture input", "", true});
  }
  rex::X_RESULT GetDeviceCapabilities(
      rex::input::DeviceId id, uint32_t,
      rex::input::X_INPUT_CAPABILITIES *out) override {
    if (id != kDevice || !std::filesystem::exists(path_))
      return X_ERROR_DEVICE_NOT_CONNECTED;
    if (out) {
      std::memset(out, 0, sizeof(*out));
      out->type = 1;
      out->sub_type = 1;
    }
    return X_ERROR_SUCCESS;
  }
  rex::X_RESULT GetDeviceState(rex::input::DeviceId id,
                               rex::input::X_INPUT_STATE *out) override {
    if (id != kDevice)
      return X_ERROR_DEVICE_NOT_CONNECTED;
    uint32_t buttons = 0, left = 0, right = 0;
    int32_t steering = 0;
    if (!(std::ifstream(path_) >> buttons >> left >> right >> steering))
      return X_ERROR_DEVICE_NOT_CONNECTED;
    if (out) {
      std::memset(out, 0, sizeof(*out));
      out->packet_number = ++packet_;
      out->gamepad.buttons = uint16_t(buttons);
      out->gamepad.left_trigger = uint8_t(left);
      out->gamepad.right_trigger = uint8_t(right);
      out->gamepad.thumb_lx = int16_t(steering);
    }
    return X_ERROR_SUCCESS;
  }
  rex::X_RESULT SetDeviceVibration(
      rex::input::DeviceId, rex::input::X_INPUT_VIBRATION *) override {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  rex::X_RESULT GetDeviceKeystroke(rex::input::DeviceId, uint32_t,
                                   rex::input::X_INPUT_KEYSTROKE *) override {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

private:
  static constexpr auto kDevice = static_cast<rex::input::DeviceId>(0x44545000);
  std::filesystem::path path_;
  uint32_t packet_ = 0;
};
} // namespace

std::unique_ptr<rex::system::IGraphicsSystem> CreateDaytonaSdkGraphics() {
  if (REXCVAR_GET(daytona_render_probe_path).empty())
    return std::make_unique<gpu::vulkan::VulkanGraphicsSystem>();
  return std::make_unique<ProbeGraphicsSystem>();
}

void InstallDaytonaProbeInput(rex::input::InputSystem &input) {
  if (!REXCVAR_GET(daytona_render_probe_path).empty())
    input.AddDriver(std::make_unique<ProbeInputDriver>());
}
