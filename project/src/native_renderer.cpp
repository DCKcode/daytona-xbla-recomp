#include "native_renderer.h"
#include "native_shaders.inc"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <span>
#include <stdexcept>
#include <tuple>
#include <vector>
#include <volk.h>

namespace daytona::native {
uint64_t AllocateTextureId() {
  static std::atomic<uint64_t> next{0};
  return ++next;
}

void Check(VkResult result) {
  if (result != VK_SUCCESS)
    throw std::runtime_error("Vulkan error " + std::to_string(result));
}
struct Buffer {
  VkBuffer handle;
  VkDeviceMemory memory;
  VkDeviceSize size;
};
struct BufferRange {
  VkBuffer handle;
  VkDeviceSize offset, size;
};
struct Image {
  VkImage handle;
  VkDeviceMemory memory;
  VkImageView view;
  uint32_t width, height;
};
class Renderer::Impl {
public:
  Impl() {
    Check(volkInitialize());
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Daytona native frame";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    Check(vkCreateInstance(&ci, nullptr, &instance_));
    volkLoadInstance(instance_);
    uint32_t count = 0;
    Check(vkEnumeratePhysicalDevices(instance_, &count, nullptr));
    std::vector<VkPhysicalDevice> devices(count);
    Check(vkEnumeratePhysicalDevices(instance_, &count, devices.data()));
    for (auto device : devices) {
      VkPhysicalDeviceProperties properties;
      vkGetPhysicalDeviceProperties(device, &properties);
      if (properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU) {
        physical_ = device;
        std::cout << properties.deviceName << '\n';
        break;
      }
    }
    if (!physical_)
      throw std::runtime_error("No hardware Vulkan device");
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &count,
                                             families.data());
    for (; family_ < count; ++family_)
      if (families[family_].queueFlags & VK_QUEUE_GRAPHICS_BIT)
        break;
    float priority = 1;
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = family_;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    VkPhysicalDeviceFeatures features{};
    features.shaderSampledImageArrayDynamicIndexing = true;
    VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dc.queueCreateInfoCount = 1;
    dc.pQueueCreateInfos = &queue;
    dc.pEnabledFeatures = &features;
    Check(vkCreateDevice(physical_, &dc, nullptr, &device_));
    volkLoadDevice(device_);
    vkGetDeviceQueue(device_, family_, 0, &queue_);
    InitializeCommands();
  }
  Impl(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
       uint32_t family, PFN_vkGetInstanceProcAddr get_instance_proc_addr)
      : instance_(instance), physical_(physical), device_(device),
        family_(family), owns_device_(false) {
    volkInitializeCustom(get_instance_proc_addr);
    volkLoadInstance(instance_);
    volkLoadDevice(device_);
    vkGetDeviceQueue(device_, family_, 0, &queue_);
    InitializeCommands();
  }
  void InitializeCommands() {
    vkGetPhysicalDeviceMemoryProperties(physical_, &memory_);
    VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cp.queueFamilyIndex = family_;
    Check(vkCreateCommandPool(device_, &cp, nullptr, &pool_));
    VkCommandBufferAllocateInfo ca{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ca.commandPool = pool_;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = 1;
    Check(vkAllocateCommandBuffers(device_, &ca, &command_));
  }
  ~Impl() {
    if (!device_)
      return;
    if (owns_device_)
      vkDeviceWaitIdle(device_);
    if (upload_.handle) {
      vkUnmapMemory(device_, upload_.memory);
      vkDestroyBuffer(device_, upload_.handle, nullptr);
      vkFreeMemory(device_, upload_.memory, nullptr);
    }
    for (auto image : target_images_)
      DestroyImage(image);
    for (auto [id, texture] : texture_cache_)
      DestroyImage(texture.image);
    if (present_framebuffer_)
      vkDestroyFramebuffer(device_, present_framebuffer_, nullptr);
    if (present_pipeline_)
      vkDestroyPipeline(device_, present_pipeline_, nullptr);
    if (present_pool_)
      vkDestroyDescriptorPool(device_, present_pool_, nullptr);
    if (present_layout_)
      vkDestroyPipelineLayout(device_, present_layout_, nullptr);
    if (present_set_)
      vkDestroyDescriptorSetLayout(device_, present_set_, nullptr);
    if (present_pass_)
      vkDestroyRenderPass(device_, present_pass_, nullptr);
    for (auto p : pipelines_)
      vkDestroyPipeline(device_, p, nullptr);
    for (auto s : modules_)
      vkDestroyShaderModule(device_, s, nullptr);
    for (const auto &[key, sampler] : sampler_cache_)
      vkDestroySampler(device_, sampler, nullptr);
    if (descriptors_)
      vkDestroyDescriptorPool(device_, descriptors_, nullptr);
    if (layout_)
      vkDestroyPipelineLayout(device_, layout_, nullptr);
    for (auto set : sets_)
      if (set)
        vkDestroyDescriptorSetLayout(device_, set, nullptr);
    if (framebuffer4_)
      vkDestroyFramebuffer(device_, framebuffer4_, nullptr);
    if (render_pass4_)
      vkDestroyRenderPass(device_, render_pass4_, nullptr);
    if (framebuffer_)
      vkDestroyFramebuffer(device_, framebuffer_, nullptr);
    if (render_pass_)
      vkDestroyRenderPass(device_, render_pass_, nullptr);
    if (opaque_scene_view_)
      vkDestroyImageView(device_, opaque_scene_view_, nullptr);
    if (scene_result_.handle)
      DestroyImage(scene_result_);
    for (auto i : images_) {
      vkDestroyImageView(device_, i.view, nullptr);
      vkDestroyImage(device_, i.handle, nullptr);
      vkFreeMemory(device_, i.memory, nullptr);
    }
    for (auto b : buffers_) {
      vkDestroyBuffer(device_, b.handle, nullptr);
      vkFreeMemory(device_, b.memory, nullptr);
    }
    vkDestroyCommandPool(device_, pool_, nullptr);
    if (owns_device_) {
      vkDestroyDevice(device_, nullptr);
      vkDestroyInstance(instance_, nullptr);
    }
  }
  std::vector<uint8_t> Render(const Frame &frame,
                              const std::filesystem::path &shaders,
                              Output target = {},
                              RenderTimings *timings = nullptr) {
    timings_ = timings;
    record_start_ = timings_ ? std::chrono::steady_clock::now()
                             : std::chrono::steady_clock::time_point{};
    ResetFrame();
    const auto &display_gamma = frame.gamma;
    const uint32_t width = frame.width, height = frame.height;
    const uint32_t draws = frame.draws.size(),
                   texture_count = frame.textures.size();
    if (!width || !height || width > 4096 || height > 4096)
      throw std::runtime_error("Invalid native frame dimensions");
    vkResetCommandPool(device_, pool_, 0);
    VkCommandBufferBeginInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    Check(vkBeginCommandBuffer(command_, &cb));
    if (!scene_result_.handle) {
      scene_result_ = CreateImage(width, height, VK_FORMAT_R8G8B8A8_UNORM,
                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                      VK_IMAGE_USAGE_SAMPLED_BIT,
                                  VK_IMAGE_ASPECT_COLOR_BIT);
      VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      view.image = scene_result_.handle;
      view.viewType = VK_IMAGE_VIEW_TYPE_2D;
      view.format = VK_FORMAT_R8G8B8A8_UNORM;
      view.components.a = VK_COMPONENT_SWIZZLE_ONE;
      view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      Check(vkCreateImageView(device_, &view, nullptr, &opaque_scene_view_));
    }
    for (uint32_t i = 0; i < texture_count; ++i) {
      const auto &source = *frame.textures[i];
      if (!source.levels ||
          source.levels >
              std::bit_width(std::max(source.width, source.height)) ||
          (source.kind != TextureKind::Image && source.levels != 1))
        throw std::runtime_error("Invalid texture mip count");
      texture_samplers_.push_back(Sampler(source.sampler));
      const auto &pixels = source.pixels;
      if (source.kind != TextureKind::Image) {
        if (!frame.world || source.width != width || source.height != height ||
            (source.kind != TextureKind::Scene &&
             source.kind != TextureKind::OpaqueScene))
          throw std::runtime_error("Unsupported native scene texture");
        auto image = scene_result_;
        if (source.kind == TextureKind::OpaqueScene)
          image.view = opaque_scene_view_;
        textures_.push_back(image);
        continue;
      }
      if (auto found = texture_cache_.find(source.id);
          found != texture_cache_.end()) {
        found->second.last_frame = frame_number_;
        textures_.push_back(found->second.image);
        continue;
      }
      auto image = CreateImage(
          source.width, source.height, VK_FORMAT_R8G8B8A8_UNORM,
          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
          VK_IMAGE_ASPECT_COLOR_BIT, VK_SAMPLE_COUNT_1_BIT, source.levels);
      {
        auto staging = CreateBuffer(
            pixels.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, pixels.data());
        buffers_.push_back(staging);
        Barrier(image.handle, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_WRITE_BIT);
        std::vector<VkBufferImageCopy> copies(source.levels);
        VkDeviceSize offset = 0;
        for (uint32_t level = 0; level < source.levels; ++level) {
          auto &copy = copies[level];
          copy.bufferOffset = offset;
          copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
          copy.imageExtent = {std::max(source.width >> level, 1u),
                              std::max(source.height >> level, 1u), 1};
          offset += VkDeviceSize(copy.imageExtent.width) *
                    copy.imageExtent.height * 4;
        }
        if (offset != pixels.size())
          throw std::runtime_error("Texture mip payload size mismatch");
        vkCmdCopyBufferToImage(command_, staging.handle, image.handle,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               copies.size(), copies.data());
        Barrier(image.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
      }
      texture_cache_[source.id] = {image, frame_number_};
      textures_.push_back(image);
    }
    if (textures_.empty()) {
      auto dummy = CreateImage(1, 1, VK_FORMAT_R8G8B8A8_UNORM,
                               VK_IMAGE_USAGE_SAMPLED_BIT |
                                   VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT);
      Barrier(dummy.handle, VK_IMAGE_LAYOUT_UNDEFINED,
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0,
              VK_ACCESS_SHADER_READ_BIT);
      images_.push_back(dummy);
      textures_.push_back(dummy);
      texture_samplers_.push_back(Sampler(0));
    }
    if (!framebuffer_) {
      color_ = CreateImage(width, height, VK_FORMAT_R8G8B8A8_UNORM,
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                               VK_IMAGE_USAGE_SAMPLED_BIT |
                               VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT);
      depth_ = CreateImage(width, height, VK_FORMAT_D32_SFLOAT,
                           VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                               VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                           VK_IMAGE_ASPECT_DEPTH_BIT);
      CreateRenderPass(color_, depth_, false);
      color4_ = CreateImage(width, height, VK_FORMAT_R8G8B8A8_UNORM,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT, VK_SAMPLE_COUNT_4_BIT);
      depth4_ = CreateImage(width, height, VK_FORMAT_D32_SFLOAT,
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                            VK_IMAGE_ASPECT_DEPTH_BIT, VK_SAMPLE_COUNT_4_BIT);
      CreateRenderPass(color4_, depth4_, true);
      target_images_ = {color_, depth_, color4_, depth4_};
    }
    if (color_.width != width || color_.height != height)
      throw std::runtime_error("Changing native output size is unsupported");
    const auto color = color_;
    CreateDescriptors(std::max(draws, 1u));
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical_, &properties);
    const VkDeviceSize uniform_alignment =
        properties.limits.minUniformBufferOffsetAlignment;
    // One upload allocation avoids the device's allocation-count limit when
    // changed tile versions need separate draws.
    uint32_t quad_vertices = 0;
    for (const auto &draw : frame.draws) {
      if (draw.primitive == Primitive::Quads) {
        if (draw.vertex_count % 4)
          throw std::runtime_error("Incomplete native quad");
        quad_vertices = std::max(quad_vertices, draw.vertex_count);
      }
    }
    const size_t quad_index_count = size_t(quad_vertices / 4) * 6;
    const VkDeviceSize required =
        std::max(VkDeviceSize(16),
                 frame.UploadSize() + quad_index_count * sizeof(uint32_t) +
                     VkDeviceSize(draws) * (3 * uniform_alignment + 16));
    if (upload_.size < required) {
      if (upload_.handle) {
        vkUnmapMemory(device_, upload_.memory);
        vkDestroyBuffer(device_, upload_.handle, nullptr);
        vkFreeMemory(device_, upload_.memory, nullptr);
      }
      upload_ = CreateBuffer(std::bit_ceil(required),
                             VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
      Check(vkMapMemory(device_, upload_.memory, 0, upload_.size, 0,
                        &upload_mapping_));
    }
    const auto upload_buffer = upload_;
    void *upload_mapping = upload_mapping_;
    VkDeviceSize upload_offset = 0;
    auto upload = [&](const void *data, size_t size,
                      VkDeviceSize alignment) -> BufferRange {
      upload_offset = ((upload_offset + alignment - 1) / alignment) * alignment;
      if (upload_offset > upload_buffer.size ||
          size > upload_buffer.size - upload_offset)
        throw std::runtime_error("Frame upload exceeds its allocation");
      std::memcpy(static_cast<uint8_t *>(upload_mapping) + upload_offset, data,
                  size);
      BufferRange range{upload_buffer.handle, upload_offset, size};
      upload_offset += size;
      return range;
    };
    BufferRange quad_indices{};
    if (quad_index_count) {
      const uint32_t first_quad = quad_indices_.size() / 6;
      for (uint32_t quad = first_quad; quad < quad_vertices / 4; ++quad) {
        const uint32_t base = quad * 4;
        const uint32_t indices[] = {base,     base + 1, base + 3,
                                    base + 1, base + 2, base + 3};
        quad_indices_.insert(quad_indices_.end(), std::begin(indices),
                             std::end(indices));
      }
      quad_indices =
          upload(quad_indices_.data(), quad_index_count * sizeof(uint32_t), 4);
    }
    struct UniformUpload {
      std::vector<uint8_t> bytes;
      BufferRange range{};
    };
    std::array<UniformUpload, 3> uniforms;
    auto upload_uniform = [&](uint32_t index, const void *data, size_t size) {
      auto &cached = uniforms[index];
      if (cached.bytes.size() != size ||
          std::memcmp(cached.bytes.data(), data, size) != 0) {
        const auto *bytes = static_cast<const uint8_t *>(data);
        cached.bytes.assign(bytes, bytes + size);
        cached.range = upload(data, size, uniform_alignment);
      }
      return cached.range;
    };
    VkClearValue clears[2]{};
    auto set_clear = [&](uint32_t target) {
      const auto &clear = target ? frame.world_clear : frame.ui_clear;
      std::memcpy(clears[0].color.float32, clear.color.data(),
                  sizeof(float) * 4);
      clears[1].depthStencil.depth = clear.depth;
    };
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = render_pass_;
    rp.framebuffer = framebuffer_;
    rp.renderArea.extent = {width, height};
    rp.clearValueCount = 2;
    rp.pClearValues = clears;
    int active_target = -1;
    bool seen_ui = false;
    bool scene_resolved = false;
    // Clear-only world passes must still execute before their resolve is
    // sampled.
    if (frame.world) {
      set_clear(1);
      rp.renderPass = render_pass4_;
      rp.framebuffer = framebuffer4_;
      vkCmdBeginRenderPass(command_, &rp, VK_SUBPASS_CONTENTS_INLINE);
      active_target = 1;
      // Xbox resolves clear the reused EDRAM for the next tile. Preserve those
      // initial colors/depths while drawing the scene once at full resolution.
      for (uint32_t tile = 1; tile < 7; ++tile) {
        if (frame.tile_clears[tile - 1] == frame.world_clear)
          continue;
        VkClearAttachment attachments[2]{};
        attachments[0].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        std::memcpy(attachments[0].clearValue.color.float32,
                    frame.tile_clears[tile - 1].color.data(),
                    sizeof(float) * 4);
        attachments[1].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        attachments[1].clearValue.depthStencil.depth =
            frame.tile_clears[tile - 1].depth;
        VkClearRect rectangle{};
        rectangle.rect = {{0, int32_t(kTileEdges[tile])},
                          {width, kTileEdges[tile + 1] - kTileEdges[tile]}};
        rectangle.layerCount = 1;
        vkCmdClearAttachments(command_, 2, attachments, 1, &rectangle);
      }
    }
    VkDescriptorSet uniform_set{};
    std::map<std::array<VkImageView, 2>, VkDescriptorSet> image_sets;
    std::map<std::array<VkSampler, 2>, VkDescriptorSet> sampler_sets;
    auto allocate_set = [&](uint32_t index) {
      VkDescriptorSet set;
      VkDescriptorSetAllocateInfo info{
          VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
      info.descriptorPool = descriptors_;
      info.descriptorSetCount = 1;
      info.pSetLayouts = &sets_[index];
      Check(vkAllocateDescriptorSets(device_, &info, &set));
      return set;
    };
    if (draws) {
      uniform_set = allocate_set(2);
      VkDescriptorBufferInfo infos[3] = {{upload_buffer.handle, 0, 4096},
                                         {upload_buffer.handle, 0, 4096},
                                         {upload_buffer.handle, 0, 512}};
      VkWriteDescriptorSet writes[3]{};
      for (uint32_t i = 0; i < 3; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = uniform_set;
        writes[i].dstBinding = i;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        writes[i].descriptorCount = 1;
        writes[i].pBufferInfo = &infos[i];
      }
      vkUpdateDescriptorSets(device_, 3, writes, 0, nullptr);
    }
    for (uint32_t draw = 0; draw < draws; ++draw) {
      const auto &d = frame.draws[draw];
      const auto &viewport = d.viewport;
      const auto &rect = d.scissor;
      int target = d.world ? 1 : 0;
      if (target && (!frame.world || seen_ui))
        throw std::runtime_error(
            "Only one world pass followed by UI is supported");
      if (!target)
        seen_ui = true;
      if (target != active_target) {
        if (active_target != -1)
          vkCmdEndRenderPass(command_);
        if (active_target == 1) {
          scene_resolved = true;
        }
        if (target && scene_resolved)
          throw std::runtime_error(
              "Multiple scene passes are not supported yet");
        set_clear(target);
        rp.renderPass = target ? render_pass4_ : render_pass_;
        rp.framebuffer = target ? framebuffer4_ : framebuffer_;
        vkCmdBeginRenderPass(command_, &rp, VK_SUBPASS_CONTENTS_INLINE);
        active_target = target;
      }
      uint32_t vertex_count = d.vertex_count;
      const auto &vs_constants = d.vs_constants;
      const auto &ps_constants = d.ps_constants;
      const auto &shared = d.shared;
      const auto &vertices = d.vertices;
      auto vertex = upload(vertices.data(), vertices.size(), 16);
      BufferRange constants[] = {
          upload_uniform(0, vs_constants.data(), vs_constants.size()),
          upload_uniform(1, ps_constants.data(), ps_constants.size()),
          upload_uniform(2, shared.data(), shared.size())};
      const VkPrimitiveTopology topology =
          d.primitive == Primitive::TriangleStrip
              ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP
              : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      auto pipeline = CreatePipeline(shaders, d, topology);
      VkDescriptorSet sets[3]{};
      sets[2] = uniform_set;
      std::array<VkImageView, 2> image_key{};
      std::array<VkSampler, 2> sampler_key{};
      std::array<VkDescriptorImageInfo, 2> images{}, samplers{};
      for (size_t i = 0; i < 2; ++i) {
        uint32_t index = d.textures[i];
        if (index >= textures_.size())
          throw std::runtime_error("Invalid texture index");
        image_key[i] = images[i].imageView = textures_[index].view;
        images[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        sampler_key[i] = samplers[i].sampler = texture_samplers_[index];
      }
      auto bind_images = [&](auto &cache, const auto &key, uint32_t set_index,
                             VkDescriptorType type, const auto &infos) {
        auto [entry, inserted] = cache.try_emplace(key);
        if (inserted) {
          entry->second = allocate_set(set_index);
          VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
          write.dstSet = entry->second;
          write.descriptorType = type;
          write.descriptorCount = 2;
          write.pImageInfo = infos.data();
          vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
        }
        return entry->second;
      };
      sets[0] = bind_images(image_sets, image_key, 0,
                            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, images);
      sets[1] = bind_images(sampler_sets, sampler_key, 1,
                            VK_DESCRIPTOR_TYPE_SAMPLER, samplers);
      vkCmdBindPipeline(command_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
      VkViewport vp{viewport[1] - viewport[0],
                    viewport[3] - viewport[2],
                    2 * viewport[0],
                    2 * viewport[2],
                    viewport[5],
                    viewport[4] + viewport[5]};
      vkCmdSetViewport(command_, 0, 1, &vp);
      uint32_t left = std::min(rect.left, width),
               top = std::min(rect.top, height),
               right = std::min(rect.right, width),
               bottom = std::min(rect.bottom, height);
      VkRect2D scissor{
          {int32_t(left), int32_t(top)},
          {right > left ? right - left : 0, bottom > top ? bottom - top : 0}};
      vkCmdSetScissor(command_, 0, 1, &scissor);
      const uint32_t dynamic_offsets[] = {uint32_t(constants[0].offset),
                                          uint32_t(constants[1].offset),
                                          uint32_t(constants[2].offset)};
      vkCmdBindDescriptorSets(command_, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              layout_, 0, 3, sets, 3, dynamic_offsets);
      vkCmdBindVertexBuffers(command_, 0, 1, &vertex.handle, &vertex.offset);
      if (d.primitive == Primitive::Quads) {
        vkCmdBindIndexBuffer(command_, quad_indices.handle, quad_indices.offset,
                             VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(command_, (vertex_count / 4) * 6, 1, 0, 0, 0);
      } else {
        vkCmdDraw(command_, vertex_count, 1, 0, 0);
      }
    }
    if (active_target != -1)
      vkCmdEndRenderPass(command_);
    if (!seen_ui) {
      set_clear(0);
      rp.renderPass = render_pass_;
      rp.framebuffer = framebuffer_;
      vkCmdBeginRenderPass(command_, &rp, VK_SUBPASS_CONTENTS_INLINE);
      vkCmdEndRenderPass(command_);
    }
    if (target.image) {
      Present(color, frame.gamma, target);
      Submit();
      return {};
    }
    auto result = CreateBuffer(size_t(width) * height * 4,
                               VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    buffers_.push_back(result);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(command_, color.handle,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, result.handle,
                           1, &copy);
    Submit();
    void *data;
    Check(vkMapMemory(device_, result.memory, 0, result.size, 0, &data));
    // The guest's display lookup runs after the final UI pass, as on the SDK.
    auto *pixels = static_cast<uint8_t *>(data);
    for (size_t i = 0; i < result.size; i += 4) {
      for (size_t channel = 0; channel < 3; ++channel)
        pixels[i + channel] = display_gamma[pixels[i + channel] * 3 + channel];
      pixels[i + 3] = 255;
    }
    std::vector<uint8_t> output(pixels, pixels + result.size);
    vkUnmapMemory(device_, result.memory);
    return output;
  }

private:
  void Present(Image source, const std::array<uint8_t, 768> &gamma,
               Output output) {
    if (!present_pass_) {
      VkDescriptorSetLayoutBinding bindings[2]{};
      bindings[0] = {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                     VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
      bindings[1] = {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                     VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
      VkDescriptorSetLayoutCreateInfo sl{
          VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
      sl.bindingCount = 2;
      sl.pBindings = bindings;
      Check(vkCreateDescriptorSetLayout(device_, &sl, nullptr, &present_set_));
      VkPipelineLayoutCreateInfo pl{
          VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
      pl.setLayoutCount = 1;
      pl.pSetLayouts = &present_set_;
      Check(vkCreatePipelineLayout(device_, &pl, nullptr, &present_layout_));
      VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1},
                                      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}};
      VkDescriptorPoolCreateInfo dp{
          VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
      dp.maxSets = 1;
      dp.poolSizeCount = 2;
      dp.pPoolSizes = sizes;
      Check(vkCreateDescriptorPool(device_, &dp, nullptr, &present_pool_));
      VkAttachmentDescription attachment{};
      attachment.format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
      attachment.samples = VK_SAMPLE_COUNT_1_BIT;
      attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      attachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
      VkSubpassDescription sub{};
      sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
      sub.colorAttachmentCount = 1;
      sub.pColorAttachments = &ref;
      VkSubpassDependency dependency{
          0,
          VK_SUBPASS_EXTERNAL,
          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
          VK_ACCESS_SHADER_READ_BIT,
          0};
      VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
      rp.attachmentCount = 1;
      rp.pAttachments = &attachment;
      rp.subpassCount = 1;
      rp.pSubpasses = &sub;
      rp.dependencyCount = 1;
      rp.pDependencies = &dependency;
      Check(vkCreateRenderPass(device_, &rp, nullptr, &present_pass_));
      VkPipelineShaderStageCreateInfo stages[2]{};
      for (auto &stage : stages) {
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.pName = "main";
      }
      stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
      stages[0].module = Shader("present.vs.spv");
      stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
      stages[1].module = Shader("present.ps.spv");
      VkPipelineVertexInputStateCreateInfo vertex{
          VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
      VkPipelineInputAssemblyStateCreateInfo assembly{
          VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
      assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      VkPipelineViewportStateCreateInfo viewport{
          VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
      viewport.viewportCount = viewport.scissorCount = 1;
      VkPipelineRasterizationStateCreateInfo raster{
          VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
      raster.polygonMode = VK_POLYGON_MODE_FILL;
      raster.lineWidth = 1;
      VkPipelineMultisampleStateCreateInfo samples{
          VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
      samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
      VkPipelineColorBlendAttachmentState blend{};
      blend.colorWriteMask = 15;
      VkPipelineColorBlendStateCreateInfo blending{
          VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
      blending.attachmentCount = 1;
      blending.pAttachments = &blend;
      VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                 VK_DYNAMIC_STATE_SCISSOR};
      VkPipelineDynamicStateCreateInfo dynamic{
          VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
      dynamic.dynamicStateCount = 2;
      dynamic.pDynamicStates = states;
      VkGraphicsPipelineCreateInfo pipeline{
          VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
      pipeline.stageCount = 2;
      pipeline.pStages = stages;
      pipeline.pVertexInputState = &vertex;
      pipeline.pInputAssemblyState = &assembly;
      pipeline.pViewportState = &viewport;
      pipeline.pRasterizationState = &raster;
      pipeline.pMultisampleState = &samples;
      pipeline.pColorBlendState = &blending;
      pipeline.pDynamicState = &dynamic;
      pipeline.layout = present_layout_;
      pipeline.renderPass = present_pass_;
      Check(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipeline,
                                      nullptr, &present_pipeline_));
    }
    Check(vkResetDescriptorPool(device_, present_pool_, 0));
    std::array<std::array<uint32_t, 4>, 256> lut{};
    for (size_t i = 0; i < 256; ++i)
      for (size_t channel = 0; channel < 3; ++channel)
        lut[i][channel] = gamma[i * 3 + channel];
    auto constants = CreateBuffer(
        sizeof(lut), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, lut.data());
    buffers_.push_back(constants);
    VkDescriptorSet set;
    VkDescriptorSetAllocateInfo alloc{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = present_pool_;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &present_set_;
    Check(vkAllocateDescriptorSets(device_, &alloc, &set));
    VkDescriptorImageInfo image{};
    image.imageView = source.view;
    image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorBufferInfo buffer{constants.handle, 0, sizeof(lut)};
    VkWriteDescriptorSet writes[2]{};
    for (auto &w : writes) {
      w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      w.dstSet = set;
      w.descriptorCount = 1;
    }
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[0].pImageInfo = &image;
    writes[1].dstBinding = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[1].pBufferInfo = &buffer;
    vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);
    Barrier(source.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    Barrier(output.image,
            output.initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                               : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            output.initialized ? VK_ACCESS_SHADER_READ_BIT : 0,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fb.renderPass = present_pass_;
    fb.attachmentCount = 1;
    fb.pAttachments = &output.view;
    fb.width = source.width;
    fb.height = source.height;
    fb.layers = 1;
    Check(vkCreateFramebuffer(device_, &fb, nullptr, &present_framebuffer_));
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass = present_pass_;
    begin.framebuffer = present_framebuffer_;
    begin.renderArea.extent = {source.width, source.height};
    vkCmdBeginRenderPass(command_, &begin, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport{0, 0, float(source.width), float(source.height), 0, 1};
    VkRect2D scissor{{0, 0}, {source.width, source.height}};
    vkCmdSetViewport(command_, 0, 1, &viewport);
    vkCmdSetScissor(command_, 0, 1, &scissor);
    vkCmdBindPipeline(command_, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      present_pipeline_);
    vkCmdBindDescriptorSets(command_, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            present_layout_, 0, 1, &set, 0, nullptr);
    vkCmdDraw(command_, 3, 1, 0, 0);
    vkCmdEndRenderPass(command_);
  }

  void Submit() {
    Check(vkEndCommandBuffer(command_));
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_;
    auto submitted = timings_ ? std::chrono::steady_clock::now()
                              : std::chrono::steady_clock::time_point{};
    if (timings_)
      timings_->record_ms =
          std::chrono::duration<double, std::milli>(submitted - record_start_)
              .count();
    Check(vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE));
    Check(vkQueueWaitIdle(queue_));
    if (timings_)
      timings_->queue_ms = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - submitted)
                               .count();
  }
  void DestroyImage(Image image) {
    vkDestroyImageView(device_, image.view, nullptr);
    vkDestroyImage(device_, image.handle, nullptr);
    vkFreeMemory(device_, image.memory, nullptr);
  }
  void ResetFrame() {
    ++frame_number_;
    if (present_framebuffer_)
      vkDestroyFramebuffer(device_, present_framebuffer_, nullptr);
    present_framebuffer_ = VK_NULL_HANDLE;
    for (auto image : images_)
      DestroyImage(image);
    images_.clear();
    for (auto buffer : buffers_) {
      vkDestroyBuffer(device_, buffer.handle, nullptr);
      vkFreeMemory(device_, buffer.memory, nullptr);
    }
    buffers_.clear();
    textures_.clear();
    texture_samplers_.clear();
    for (auto it = texture_cache_.begin(); it != texture_cache_.end();) {
      if (frame_number_ - it->second.last_frame > 120) {
        DestroyImage(it->second.image);
        it = texture_cache_.erase(it);
      } else
        ++it;
    }
  }

  uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < memory_.memoryTypeCount; ++i)
      if ((bits & (1u << i)) &&
          (memory_.memoryTypes[i].propertyFlags & flags) == flags)
        return i;
    throw std::runtime_error("No suitable Vulkan memory type");
  }
  Buffer CreateBuffer(size_t size, VkBufferUsageFlags usage,
                      const void *data = nullptr) {
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    const size_t data_size = size;
    size = std::max(size, size_t(16));
    ci.size = size;
    ci.usage = usage;
    Buffer b{};
    b.size = size;
    Check(vkCreateBuffer(device_, &ci, nullptr, &b.handle));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device_, b.handle, &req);
    VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ma.allocationSize = req.size;
    ma.memoryTypeIndex = MemoryType(req.memoryTypeBits,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    Check(vkAllocateMemory(device_, &ma, nullptr, &b.memory));
    Check(vkBindBufferMemory(device_, b.handle, b.memory, 0));
    if (data) {
      void *dest;
      Check(vkMapMemory(device_, b.memory, 0, size, 0, &dest));
      std::memcpy(dest, data, data_size);
      vkUnmapMemory(device_, b.memory);
    }
    return b;
  }
  Image CreateImage(uint32_t w, uint32_t h, VkFormat format,
                    VkImageUsageFlags usage, VkImageAspectFlags aspect,
                    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT,
                    uint32_t levels = 1) {
    Image image{};
    image.width = w;
    image.height = h;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {w, h, 1};
    ci.mipLevels = levels;
    ci.arrayLayers = 1;
    ci.samples = samples;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    Check(vkCreateImage(device_, &ci, nullptr, &image.handle));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device_, image.handle, &req);
    VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ma.allocationSize = req.size;
    VkMemoryPropertyFlags properties = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    if (usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) {
      for (uint32_t i = 0; i < memory_.memoryTypeCount; ++i)
        if ((req.memoryTypeBits & (1u << i)) &&
            (memory_.memoryTypes[i].propertyFlags &
             VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT)) {
          properties |= VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT;
          break;
        }
    }
    ma.memoryTypeIndex = MemoryType(req.memoryTypeBits, properties);
    Check(vkAllocateMemory(device_, &ma, nullptr, &image.memory));
    Check(vkBindImageMemory(device_, image.handle, image.memory, 0));
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = image.handle;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {aspect, 0, levels, 0, 1};
    Check(vkCreateImageView(device_, &vi, nullptr, &image.view));
    return image;
  }
  void Barrier(VkImage image, VkImageLayout from, VkImageLayout to,
               VkAccessFlags src, VkAccessFlags dest) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src;
    b.dstAccessMask = dest;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS,
                          0, 1};
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
  }
  void CreateRenderPass(Image color, Image depth, bool multisampled) {
    VkRenderPass pass{};
    VkFramebuffer framebuffer{};
    VkAttachmentDescription attachments[3]{};
    attachments[0].format = VK_FORMAT_R8G8B8A8_UNORM;
    attachments[1].format = VK_FORMAT_D32_SFLOAT;
    for (auto &a : attachments) {
      a.samples = multisampled ? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_1_BIT;
      a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    }
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].finalLayout = multisampled
                                     ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                     : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    // Resolve in the pass so tile GPUs can discard multisampled storage.
    if (multisampled) {
      attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      attachments[2].format = VK_FORMAT_R8G8B8A8_UNORM;
      attachments[2].samples = VK_SAMPLE_COUNT_1_BIT;
      attachments[2].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      attachments[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      attachments[2].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    attachments[1].finalLayout =
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference c{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        d{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &c;
    sub.pDepthStencilAttachment = &d;
    VkAttachmentReference resolve{2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    if (multisampled)
      sub.pResolveAttachments = &resolve;
    VkSubpassDependency deps[2]{};
    deps[0] = {VK_SUBPASS_EXTERNAL,
               0,
               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                   VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
               0,
               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
               0};
    deps[1] = {0,
               VK_SUBPASS_EXTERNAL,
               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT,
               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
               VK_ACCESS_TRANSFER_READ_BIT,
               0};
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    if (multisampled) {
      deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
      deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    }
    rp.attachmentCount = multisampled ? 3 : 2;
    rp.pAttachments = attachments;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    rp.dependencyCount = 2;
    rp.pDependencies = deps;
    Check(vkCreateRenderPass(device_, &rp, nullptr, &pass));
    VkImageView views[] = {color.view, depth.view, scene_result_.view};
    VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fb.renderPass = pass;
    fb.attachmentCount = multisampled ? 3 : 2;
    fb.pAttachments = views;
    fb.width = color.width;
    fb.height = color.height;
    fb.layers = 1;
    Check(vkCreateFramebuffer(device_, &fb, nullptr, &framebuffer));
    if (multisampled) {
      render_pass4_ = pass;
      framebuffer4_ = framebuffer;
    } else {
      render_pass_ = pass;
      framebuffer_ = framebuffer;
    }
  }
  void CreateDescriptors(uint32_t draws) {
    if (!layout_) {
      for (uint32_t s = 0; s < 3; ++s) {
        VkDescriptorSetLayoutBinding b[3]{};
        uint32_t n = s == 2 ? 3 : 1;
        for (uint32_t i = 0; i < n; ++i) {
          b[i].binding = i;
          b[i].descriptorCount = s == 2 ? 1 : 2;
          b[i].descriptorType = s == 2
                                    ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
                                : s == 0 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                         : VK_DESCRIPTOR_TYPE_SAMPLER;
          b[i].stageFlags =
              VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo ci{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = n;
        ci.pBindings = b;
        Check(vkCreateDescriptorSetLayout(device_, &ci, nullptr, &sets_[s]));
      }
      VkPipelineLayoutCreateInfo pl{
          VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
      pl.setLayoutCount = 3;
      pl.pSetLayouts = sets_;
      Check(vkCreatePipelineLayout(device_, &pl, nullptr, &layout_));
    }
    if (descriptors_ && draws <= descriptor_capacity_) {
      Check(vkResetDescriptorPool(device_, descriptors_, 0));
      return;
    }
    if (descriptors_)
      vkDestroyDescriptorPool(device_, descriptors_, nullptr);
    descriptor_capacity_ = draws;
    VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2 * draws},
        {VK_DESCRIPTOR_TYPE_SAMPLER, 2 * draws},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 3}};
    VkDescriptorPoolCreateInfo dp{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = 3 * draws;
    dp.poolSizeCount = 3;
    dp.pPoolSizes = sizes;
    Check(vkCreateDescriptorPool(device_, &dp, nullptr, &descriptors_));
  }
  VkSampler Sampler(uint32_t key) {
    if (auto cached = sampler_cache_.find(key); cached != sampler_cache_.end())
      return cached->second;
    if (key & ~0xFFFFFFu)
      throw std::runtime_error("Invalid sampler parameters");
    auto address_mode = [](uint32_t mode) {
      switch (mode) {
      case 0:
        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
      case 1:
        return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
      case 2:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      case 6:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
      default:
        throw std::runtime_error("Unsupported sampler address mode");
      }
    };
    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.magFilter = key & 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    info.minFilter = key & 2 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    info.mipmapMode = key & 512 ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.maxLod = (key >> 10) & 15;
    const int32_t bias = int32_t(key << 8) >> 22;
    info.mipLodBias = float(bias) / 32;
    info.addressModeU = address_mode((key >> 2) & 7);
    info.addressModeV = address_mode((key >> 5) & 7);
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.borderColor = key & 256 ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                                 : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    VkSampler sampler;
    Check(vkCreateSampler(device_, &info, nullptr, &sampler));
    sampler_cache_[key] = sampler;
    return sampler;
  }
  VkShaderModule Shader(const std::filesystem::path &path) {
    if (auto cached = shader_cache_.find(path); cached != shader_cache_.end())
      return cached->second;
    std::vector<uint32_t> code;
    if (path.parent_path().empty()) {
      auto embedded = EmbeddedShader(path.filename().string());
      code.assign(embedded.begin(), embedded.end());
    } else {
      std::ifstream f(path, std::ios::binary | std::ios::ate);
      if (!f)
        throw std::runtime_error("Missing SPIR-V " + path.string());
      const auto length = f.tellg();
      if (length < 20 || length % 4 != 0)
        throw std::runtime_error("Invalid SPIR-V length " + path.string());
      code.resize(size_t(length) / 4);
      f.seekg(0);
      if (!f.read(reinterpret_cast<char *>(code.data()), length) ||
          code[0] != 0x07230203)
        throw std::runtime_error("Invalid SPIR-V " + path.string());
    }
    const auto size = code.size() * 4;
    VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    sm.codeSize = size;
    sm.pCode = code.data();
    VkShaderModule module;
    Check(vkCreateShaderModule(device_, &sm, nullptr, &module));
    modules_.push_back(module);
    shader_cache_.emplace(path, module);
    return module;
  }
  VkPipeline CreatePipeline(const std::filesystem::path &shaders,
                            const Draw &draw, VkPrimitiveTopology topology) {
    const auto &hashes = draw.programs;
    const auto &locations = draw.locations;
    const PipelineKey key{hashes,
                          locations,
                          draw.attribute_count,
                          topology,
                          {draw.blend_control, draw.depth_control,
                           draw.color_mask, draw.raster_control},
                          draw.world};
    if (auto cached = pipeline_cache_.find(key);
        cached != pipeline_cache_.end())
      return cached->second;
    char name[40];
    std::snprintf(name, sizeof(name), "%016llX.vs.spv",
                  (unsigned long long)hashes[0]);
    auto vs = Shader(shaders / name);
    std::snprintf(name, sizeof(name), "%016llX.ps.spv",
                  (unsigned long long)hashes[1]);
    auto ps = Shader(shaders / name);
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = stages[1].sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[0].module = vs;
    stages[1].module = ps;
    stages[0].pName = stages[1].pName = "main";
    VkVertexInputBindingDescription binding{0, draw.attribute_count * 16,
                                            VK_VERTEX_INPUT_RATE_VERTEX};
    std::array<VkVertexInputAttributeDescription, 8> attributes{};
    for (uint32_t i = 0; i < draw.attribute_count; ++i)
      attributes[i] = {locations[i], 0, VK_FORMAT_R32G32B32A32_SFLOAT, i * 16};
    VkPipelineVertexInputStateCreateInfo vertex{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex.vertexBindingDescriptionCount = 1;
    vertex.pVertexBindingDescriptions = &binding;
    vertex.vertexAttributeDescriptionCount = draw.attribute_count;
    vertex.pVertexAttributeDescriptions = attributes.data();
    VkPipelineInputAssemblyStateCreateInfo assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = topology;
    VkPipelineViewportStateCreateInfo viewport{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.lineWidth = 1;
    raster.cullMode = draw.raster_control & 3;
    raster.frontFace = (draw.raster_control & 4)
                           ? VK_FRONT_FACE_CLOCKWISE
                           : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    VkPipelineMultisampleStateCreateInfo samples{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    samples.rasterizationSamples =
        draw.world ? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = (draw.depth_control >> 1) & 1;
    depth.depthWriteEnable = (draw.depth_control >> 2) & 1;
    depth.depthCompareOp = VkCompareOp((draw.depth_control >> 4) & 7);
    auto factor = [](uint32_t n) {
      switch (n) {
      case 0:
        return VK_BLEND_FACTOR_ZERO;
      case 1:
        return VK_BLEND_FACTOR_ONE;
      case 4:
        return VK_BLEND_FACTOR_SRC_COLOR;
      case 5:
        return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
      case 6:
        return VK_BLEND_FACTOR_SRC_ALPHA;
      case 7:
        return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      case 8:
        return VK_BLEND_FACTOR_DST_COLOR;
      case 9:
        return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
      case 10:
        return VK_BLEND_FACTOR_DST_ALPHA;
      case 11:
        return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
      default:
        throw std::runtime_error("Unsupported blend factor");
      }
    };
    auto op = [](uint32_t n) {
      const VkBlendOp ops[] = {VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT,
                               VK_BLEND_OP_MIN, VK_BLEND_OP_MAX,
                               VK_BLEND_OP_REVERSE_SUBTRACT};
      if (n > 4)
        throw std::runtime_error("Unsupported blend operation");
      return ops[n];
    };
    uint32_t b = draw.blend_control;
    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = true;
    blend.srcColorBlendFactor = factor(b & 31);
    blend.dstColorBlendFactor = factor((b >> 8) & 31);
    blend.colorBlendOp = op((b >> 5) & 7);
    blend.srcAlphaBlendFactor = factor((b >> 16) & 31);
    blend.dstAlphaBlendFactor = factor((b >> 24) & 31);
    blend.alphaBlendOp = op((b >> 21) & 7);
    blend.colorWriteMask = draw.color_mask & 15;
    VkPipelineColorBlendStateCreateInfo blending{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blending.attachmentCount = 1;
    blending.pAttachments = &blend;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT,
                            VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo pc{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pc.stageCount = 2;
    pc.pStages = stages;
    pc.pVertexInputState = &vertex;
    pc.pInputAssemblyState = &assembly;
    pc.pViewportState = &viewport;
    pc.pRasterizationState = &raster;
    pc.pMultisampleState = &samples;
    pc.pDepthStencilState = &depth;
    pc.pColorBlendState = &blending;
    pc.pDynamicState = &dynamic;
    pc.layout = layout_;
    pc.renderPass = draw.world ? render_pass4_ : render_pass_;
    VkPipeline pipeline;
    Check(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pc, nullptr,
                                    &pipeline));
    pipelines_.push_back(pipeline);
    pipeline_cache_.emplace(key, pipeline);
    return pipeline;
  }
  struct CachedTexture {
    Image image;
    uint64_t last_frame;
  };
  std::map<uint64_t, CachedTexture> texture_cache_;
  uint64_t frame_number_ = 0;
  uint32_t descriptor_capacity_ = 0;
  bool owns_device_ = true;
  VkFramebuffer present_framebuffer_{};
  VkRenderPass present_pass_{};
  VkDescriptorSetLayout present_set_{};
  VkPipelineLayout present_layout_{};
  VkPipeline present_pipeline_{};
  VkDescriptorPool present_pool_{};
  RenderTimings *timings_ = nullptr;
  std::chrono::steady_clock::time_point record_start_;
  std::vector<uint32_t> quad_indices_;
  Buffer upload_{};
  void *upload_mapping_ = nullptr;
  Image color_{}, depth_{}, color4_{}, depth4_{};
  std::vector<Image> target_images_;
  Image scene_result_{};
  VkImageView opaque_scene_view_{};
  VkInstance instance_{};
  VkPhysicalDevice physical_{};
  VkDevice device_{};
  VkQueue queue_{};
  uint32_t family_ = 0;
  VkPhysicalDeviceMemoryProperties memory_{};
  VkCommandPool pool_{};
  VkCommandBuffer command_{};
  VkRenderPass render_pass_{}, render_pass4_{};
  VkFramebuffer framebuffer_{}, framebuffer4_{};
  VkDescriptorSetLayout sets_[3]{};
  VkPipelineLayout layout_{};
  VkDescriptorPool descriptors_{};
  std::map<uint32_t, VkSampler> sampler_cache_;
  std::vector<VkSampler> texture_samplers_;
  std::vector<Buffer> buffers_;
  std::vector<Image> images_, textures_;
  std::vector<VkPipeline> pipelines_;
  std::vector<VkShaderModule> modules_;
  using PipelineKey =
      std::tuple<std::array<uint64_t, 2>, std::array<uint32_t, 8>, uint32_t,
                 VkPrimitiveTopology, std::array<uint32_t, 4>, bool>;
  std::map<PipelineKey, VkPipeline> pipeline_cache_;
  std::map<std::filesystem::path, VkShaderModule> shader_cache_;
};
Renderer::Renderer() : impl_(std::make_unique<Impl>()) {}
Renderer::Renderer(VkInstance instance, VkPhysicalDevice physical,
                   VkDevice device, uint32_t family,
                   PFN_vkGetInstanceProcAddr get_instance_proc_addr)
    : impl_(std::make_unique<Impl>(instance, physical, device, family,
                                   get_instance_proc_addr)) {}
Renderer::~Renderer() = default;
std::vector<uint8_t> Renderer::Render(const Frame &frame,
                                      const std::filesystem::path &shaders,
                                      Output target, RenderTimings *timings) {
  return impl_->Render(frame, shaders, target, timings);
}
} // namespace daytona::native
