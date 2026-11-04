#include "native_graphics.h"
#include <rex/ui/vulkan/device.h>
#include "native_frame_builder.h"
#include <algorithm>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <optional>
#include <rex/cvar.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/vulkan/graphics_system.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/thread.h>
#include <rex/ui/vulkan/instance.h>
#include <rex/ui/vulkan/presenter.h>
#include <thread>
#include <xxhash.h>

REXCVAR_DEFINE_BOOL(daytona_native_renderer, false, "GPU",
                    "Render Daytona with native Vulkan targets.");
REXCVAR_DEFINE_BOOL(daytona_native_profile, false, "Diagnostics",
                    "Log native frame CPU and queue timings.");
REXCVAR_DEFINE_UINT64(daytona_native_cpu_mask, 0, "GPU",
                      "CPU affinity mask for native preparation and rendering; "
                      "0 keeps the scheduler default.");
REXCVAR_DEFINE_STRING(
    daytona_native_capture_path, "", "Diagnostics",
    "Save native frame inputs when a capture file appears here.");
namespace {
namespace gpu = rex::graphics;
class NativeCommandProcessor final : public gpu::CommandProcessor {
public:
  NativeCommandProcessor(gpu::GraphicsSystem *graphics,
                         rex::system::KernelState *kernel)
      : CommandProcessor(graphics, kernel) {}
  void IssueSwap(uint32_t address, uint32_t width, uint32_t height) override {
    auto finish_start = profile_ ? std::chrono::steady_clock::now()
                                 : std::chrono::steady_clock::time_point{};
    auto frame = builder_->Finish();
    if (profile_)
      finish_ms_ += std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - finish_start)
                        .count();
    started_ = true;
    for (size_t i = 0; i < 256; ++i) {
      const auto &entry = gamma_ramp_256_entry_table()[i];
      frame.gamma[i * 3] = (entry.color_10_red * 255 + 511) / 1023;
      frame.gamma[i * 3 + 1] = (entry.color_10_green * 255 + 511) / 1023;
      frame.gamma[i * 3 + 2] = (entry.color_10_blue * 255 + 511) / 1023;
    }
    if (frame_number_ % 120 == 0)
      REXLOG_INFO("Daytona native frame {}: {} draws, {} textures",
                  frame_number_, frame.draws.size(), frame.textures.size());
    if (profile_ && frame_number_ % 120 == 119) {
      auto counts = builder_->work_counts();
      auto generation = builder_->texture_generation();
      profile_logger_->info(
          "Native input profile ms: scene {:.2f}, UI {:.2f}, "
          "finish {:.2f}; hashes {:.1f}, decodes {:.1f}, invalidations {:.1f}",
          input_ms_[0] / 120, input_ms_[1] / 120, finish_ms_ / 120,
          double(counts.hashes - last_counts_.hashes) / 120,
          double(counts.decodes - last_counts_.decodes) / 120,
          double(generation - last_generation_) / 120);
      input_ms_ = {};
      finish_ms_ = 0;
      last_counts_ = counts;
      last_generation_ = generation;
    }
    ++frame_number_;
    {
      std::lock_guard lock(render_mutex_);
      if (profile_) {
        ++submitted_;
        replaced_ += pending_.has_value();
      }
      pending_ = PendingFrame{std::move(frame), width, height};
    }
    render_ready_.notify_one();
  }
  void WaitForInputs() {
    std::unique_lock lock(input_mutex_);
    input_ready_.wait(lock, [this] {
      return !worker_running_ ||
             (inputs_idle_ && idle_pointer_ == write_ptr_index_.load());
    });
  }

protected:
  void PrepareForWait() override {
    CommandProcessor::PrepareForWait();
    if (read_ptr_index_ == write_ptr_index_.load()) {
      std::lock_guard lock(input_mutex_);
      idle_pointer_ = read_ptr_index_;
      inputs_idle_ = true;
      input_ready_.notify_all();
    }
  }
  void ReturnFromWait() override {
    CommandProcessor::ReturnFromWait();
    std::lock_guard lock(input_mutex_);
    inputs_idle_ = false;
  }
  bool SetupContext() override {
    if (auto mask = REXCVAR_GET(daytona_native_cpu_mask))
      rex::thread::Thread::GetCurrentThread()->set_affinity_mask(mask);
    auto *presenter = static_cast<rex::ui::vulkan::VulkanPresenter *>(
        graphics_system_->presenter());
    device_ = presenter->vulkan_device();
    REXLOG_INFO("Daytona native Vulkan device: {}",
                device_->properties().deviceName);
    profile_ = REXCVAR_GET(daytona_native_profile);
    if (profile_) {
      // Profile without enabling the much more frequent guest hook messages.
      profile_logger_ =
          rex::GetLoggerRaw(rex::RegisterLogCategory("daytona.native.profile"));
      profile_logger_->set_level(spdlog::level::info);
    }
    capture_root_ = REXCVAR_GET(daytona_native_capture_path);
    if (!capture_root_.empty())
      std::filesystem::create_directories(capture_root_);
    render_thread_ = std::thread([this] { RenderFrames(); });
    builder_ = std::make_unique<daytona::native::FrameBuilder>(
        memory_->TranslatePhysical<const uint8_t *>(0), memory_,
        daytona::native::SceneInputs::SinglePass);
    REXLOG_INFO("Daytona native Vulkan renderer active");
    return true;
  }
  void ShutdownContext() override {
    {
      std::lock_guard lock(input_mutex_);
      input_ready_.notify_all();
    }
    {
      std::lock_guard lock(render_mutex_);
      stopping_ = true;
      pending_.reset();
    }
    render_ready_.notify_one();
    render_thread_.join();
    builder_.reset();
  }
  gpu::Shader *LoadShader(gpu::xenos::ShaderType type, uint32_t,
                          const uint32_t *data, uint32_t count) override {
    uint64_t hash = XXH3_64bits(data, size_t(count) * 4);
    if (auto found = shaders_.find(hash); found != shaders_.end())
      return found->second.get();
    auto shader = std::make_unique<gpu::Shader>(type, hash, data, count,
                                                std::endian::big);
    rex::string::StringBuffer disassembly;
    shader->AnalyzeUcode(disassembly);
    auto *result = shader.get();
    shaders_.emplace(hash, std::move(shader));
    return result;
  }
  bool IssueDraw(gpu::xenos::PrimitiveType primitive, uint32_t count,
                 IndexBufferInfo *indices, bool) override {
    if (!started_)
      return true;
    if (register_file_->Get<gpu::reg::RB_MODECONTROL>().edram_mode ==
        gpu::xenos::EdramMode::kCopy)
      return IssueCopy();
    if (!count)
      return true;
    auto *vs = active_vertex_shader(), *ps = active_pixel_shader();
    if (vs && vs->ucode_data_hash() == daytona::native::kSyncVertexShader)
      return true;
    if (!vs || !ps)
      throw std::runtime_error("Native draw has no shader");
    auto input_start = profile_ ? std::chrono::steady_clock::now()
                                : std::chrono::steady_clock::time_point{};
    uint32_t bucket = (((*register_file_)[0x2000] >> 16) & 3) == 2 ? 0 : 1;
    builder_->DrawGuest(*register_file_, *vs, *ps, uint32_t(primitive), count,
                        indices && indices->guest_base);
    if (profile_)
      input_ms_[bucket] += std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - input_start)
                               .count();
    return true;
  }
  bool IssueCopy() override {
    if (started_)
      builder_->CopyGuest(*register_file_);
    return true;
  }

private:
  struct PendingFrame {
    daytona::native::Frame frame;
    uint32_t width, height;
  };
  void RenderFrames() {
    rex::thread::set_current_thread_name("Daytona Vulkan");
    if (auto mask = REXCVAR_GET(daytona_native_cpu_mask))
      rex::thread::Thread::GetCurrentThread()->set_affinity_mask(mask);
    daytona::native::Renderer renderer(
        device_->vulkan_instance()->instance(), device_->physical_device(),
        device_->device(), device_->queue_family_graphics_compute(),
        device_->vulkan_instance()->functions().vkGetInstanceProcAddr);
    uint64_t refreshed = 0, consumed = 0;
    uint64_t last_submitted = 0, last_replaced = 0, refresh_skips = 0;
    double record_ms = 0, queue_ms = 0, presenter_ms = 0;
    double max_render_ms = 0, max_refresh_ms = 0, idle_ms = 0;
    auto last_report = std::chrono::steady_clock::now();
    for (;;) {
      std::optional<PendingFrame> pending;
      auto wait_start = profile_ ? std::chrono::steady_clock::now()
                                 : std::chrono::steady_clock::time_point{};
      {
        std::unique_lock lock(render_mutex_);
        render_ready_.wait(
            lock, [this] { return stopping_ || pending_.has_value(); });
        if (stopping_)
          break;
        pending = std::move(pending_);
        pending_.reset();
      }
      if (profile_)
        idle_ms += std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - wait_start)
                       .count();
      auto &frame = pending->frame;
      const auto frame_index = consumed++;
      if (!capture_root_.empty()) {
        const auto request = capture_root_ / "capture";
        if (std::filesystem::exists(request)) {
          std::ifstream input(request);
          uint32_t count = 1;
          input >> count;
          if (!input || count == 0 || count > 120)
            throw std::runtime_error("Native capture count must be 1..120");
          capture_remaining_ = count;
          std::filesystem::remove(request);
        }
        if (capture_remaining_) {
          auto path = capture_root_ / (std::to_string(frame_index) + ".bin");
          daytona::native::SaveFrame(frame, path);
          --capture_remaining_;
          REXLOG_INFO("Saved native frame inputs to {}", path.string());
        }
      }
      auto refresh_start = profile_ ? std::chrono::steady_clock::now()
                                    : std::chrono::steady_clock::time_point{};
      daytona::native::RenderTimings timings;
      bool rendered = false;
      graphics_system_->presenter()->RefreshGuestOutput(
          frame.width, frame.height, pending->width, pending->height,
          [&](rex::ui::Presenter::GuestOutputRefreshContext &context) {
            auto &vk =
                static_cast<rex::ui::vulkan::VulkanPresenter::
                                VulkanGuestOutputRefreshContext &>(context);
            // Renderer borrows the SDK queue and must share its submission
            // lock.
            auto queue_lock = device_->AcquireQueue(
                device_->queue_family_graphics_compute(), 0);
            renderer.Render(frame, {},
                            {vk.image(), vk.image_view(),
                             vk.image_ever_written_previously()},
                            profile_ ? &timings : nullptr);
            rendered = true;
            context.SetIs8bpc(true);
            return true;
          });
      if (!rendered) {
        ++refresh_skips;
        continue;
      }
      if (profile_) {
        double refresh_ms =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - refresh_start)
                .count();
        double render_ms = timings.record_ms + timings.queue_ms;
        presenter_ms += refresh_ms - render_ms;
        max_render_ms = std::max(max_render_ms, render_ms);
        max_refresh_ms = std::max(max_refresh_ms, refresh_ms);
      }
      record_ms += timings.record_ms;
      queue_ms += timings.queue_ms;
      if (++refreshed % 120 == 0) {
        auto now = std::chrono::steady_clock::now();
        double seconds =
            std::chrono::duration<double>(now - last_report).count();
        REXLOG_INFO("Daytona native refreshed {} guest frames ({:.1f} fps)",
                    refreshed, 120.0 / seconds);
        if (profile_) {
          uint64_t submitted, replaced;
          {
            std::lock_guard lock(render_mutex_);
            submitted = submitted_;
            replaced = replaced_;
          }
          profile_logger_->info(
              "Native frame flow: {:.2f} fps, {} swaps, {} replaced, "
              "{} refresh skips; idle {:.2f}, max render {:.2f}, "
              "max refresh {:.2f} ms",
              120.0 / seconds, submitted - last_submitted,
              replaced - last_replaced, refresh_skips, idle_ms / 120,
              max_render_ms, max_refresh_ms);
          profile_logger_->info(
              "Native render profile ms: record {:.2f}, queue {:.2f}, "
              "presenter {:.2f}",
              record_ms / 120, queue_ms / 120, presenter_ms / 120);
          last_submitted = submitted;
          last_replaced = replaced;
        }
        record_ms = queue_ms = presenter_ms = 0;
        max_render_ms = max_refresh_ms = idle_ms = 0;
        refresh_skips = 0;
        last_report = now;
      }
    }
  }
  // Consume guest buffers promptly while Vulkan compiles or renders. Keep only
  // the newest waiting frame so GPU stalls cannot build up input latency.
  std::mutex render_mutex_;
  std::condition_variable render_ready_;
  std::optional<PendingFrame> pending_;
  std::thread render_thread_;
  bool stopping_ = false;
  uint64_t submitted_ = 0, replaced_ = 0;
  spdlog::logger *profile_logger_ = nullptr;
  const rex::ui::vulkan::VulkanDevice *device_ = nullptr;
  std::unique_ptr<daytona::native::FrameBuilder> builder_;
  std::map<uint64_t, std::unique_ptr<gpu::Shader>> shaders_;
  uint64_t frame_number_ = 0;
  bool started_ = false, profile_ = false;
  std::array<double, 2> input_ms_{};
  double finish_ms_ = 0;
  daytona::native::FrameBuilder::WorkCounts last_counts_;
  uint64_t last_generation_ = 0;
  std::filesystem::path capture_root_;
  uint32_t capture_remaining_ = 0;
  std::mutex input_mutex_;
  std::condition_variable input_ready_;
  bool inputs_idle_ = false;
  uint32_t idle_pointer_ = 0;
};
class NativeGraphicsSystem final : public gpu::vulkan::VulkanGraphicsSystem {
  std::unique_ptr<gpu::CommandProcessor> CreateCommandProcessor() override {
    return std::make_unique<NativeCommandProcessor>(this, kernel_state_);
  }
};
} // namespace
std::unique_ptr<rex::system::IGraphicsSystem> CreateDaytonaNativeGraphics() {
  if (!REXCVAR_GET(daytona_native_renderer))
    return nullptr;
  return std::make_unique<NativeGraphicsSystem>();
}
void WaitForDaytonaNativeInputs() {
  auto *graphics = static_cast<gpu::GraphicsSystem *>(
      REX_KERNEL_STATE()->emulator()->graphics_system());
  static_cast<NativeCommandProcessor *>(graphics->command_processor())
      ->WaitForInputs();
}
