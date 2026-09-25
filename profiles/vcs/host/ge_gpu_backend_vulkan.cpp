#include "ge_gpu_backend.hpp"
#include "ge_gpu_vulkan_shaders.hpp"
#include "vcs_config.hpp"

#include <vulkan/vulkan.h>
#include <android/log.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace vcs {
namespace {

constexpr std::uint32_t kWidth = 480u;
constexpr std::uint32_t kHeight = 272u;
// VCS renders the world (and the HUD over it) into a 512x320 surface at
// 0x88000, then its 64 composition quads sample all of it back into the
// 480x272 display. Measured on device: world scissor 512,320, composition UVs
// up to 512,320. At 480x272 the world was cropped and the composition clamped
// past the edge - the streaks down the bottom and right of the screen.
constexpr std::uint32_t kWorldWidth = 512u;
constexpr std::uint32_t kWorldHeight = 320u;
constexpr VkDeviceSize kVertexCapacity = 32u * 1024u * 1024u;

struct Buffer {
    VkBuffer handle{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    void *mapped{};
};

struct DrawBatch {
    GeGpuDrawDescriptor draw{};
    std::uint32_t first{};
    std::uint32_t count{};
    std::uint64_t texture_key{};
    bool textured{};
    bool framebuffer_feedback{};
};

struct Texture {
    std::uint64_t signature{};
    std::uint64_t checked_frame{UINT64_MAX};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint64_t bytes{};
    VkImage image{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    VkImageView view{VK_NULL_HANDLE};
    VkDescriptorSet descriptor{VK_NULL_HANDLE};
    Buffer staging{};
    bool pending{};
    // Its upload has been recorded into a submitted frame; the staging buffer
    // can go once that frame's fence has signalled.
    bool recorded{};
};

struct VulkanPreview {
    GeGpuBackendReport report{};
    bool enabled{};
    bool authoritative{};
    bool software_menu_active{};
    // A 3D (non-through, non-clear) draw has been seen since the last vblank.
    // Menus draw none; they also double-buffer between two surfaces every
    // vblank, one of which is the gameplay world target, so the address alone
    // cannot tell a menu frame from a gameplay one.
    bool frame_has_scene{};
    // A frame submitted but not yet collected. The CPU no longer waits for the
    // GPU right after submitting: it goes on emulating the next vblank while
    // the GPU draws, and collects the result at the start of the next frame.
    // The last frame drew into the displayed surface through the world target
    // (interface drawn into 0x88000, then composited). Menus that double-buffer
    // draw straight into a surface instead, and must not be treated as world.
    bool composited_last_frame{};
    bool frame_in_flight{};
    bool fence_waited{};
    std::uint64_t in_flight_vblank{};
    unsigned consecutive_2d_frames{};
    VkInstance instance{VK_NULL_HANDLE};
    VkPhysicalDevice physical{VK_NULL_HANDLE};
    VkDevice device{VK_NULL_HANDLE};
    VkQueue queue{VK_NULL_HANDLE};
    std::uint32_t queue_family{};
    VkCommandPool command_pool{VK_NULL_HANDLE};
    VkCommandBuffer command{VK_NULL_HANDLE};
    VkFence fence{VK_NULL_HANDLE};
    VkImage color{VK_NULL_HANDLE};
    VkDeviceMemory color_memory{VK_NULL_HANDLE};
    VkImageView color_view{VK_NULL_HANDLE};
    VkImage world_color{VK_NULL_HANDLE};
    VkDeviceMemory world_color_memory{VK_NULL_HANDLE};
    VkImageView world_color_view{VK_NULL_HANDLE};
    VkFramebuffer world_framebuffer{VK_NULL_HANDLE};
    VkDescriptorSet world_descriptor{VK_NULL_HANDLE};
    std::uint32_t world_framebuffer_address{};
    VkImage depth{VK_NULL_HANDLE};
    VkDeviceMemory depth_memory{VK_NULL_HANDLE};
    VkImageView depth_view{VK_NULL_HANDLE};
    VkRenderPass render_pass{VK_NULL_HANDLE};
    VkFramebuffer framebuffer{VK_NULL_HANDLE};
    VkShaderModule vertex_shader{VK_NULL_HANDLE};
    VkShaderModule fragment_shader{VK_NULL_HANDLE};
    VkShaderModule textured_shader{VK_NULL_HANDLE};
    VkDescriptorSetLayout descriptor_layout{VK_NULL_HANDLE};
    VkDescriptorPool descriptor_pool{VK_NULL_HANDLE};
    VkSampler sampler{VK_NULL_HANDLE};
    // Per-texture sampling state, indexed by texture_sampler_index(): the PSP
    // chooses filtering and wrap/clamp per texture, and a single NEAREST+REPEAT
    // sampler for everything is what made the ground render as hard blocks.
    std::array<VkSampler, 16> texture_samplers{};
    // Descriptor sets available for textures (the pool also holds the world
    // target's), and so the hard ceiling on cached textures. See create_pipeline.
    std::uint32_t texture_capacity{};
    // Physical sizes of the two targets. Coordinates stay in PSP space (480x272
    // display, 512x320 world) and are mapped onto these, so raising the internal
    // resolution renders more pixels rather than stretching the PSP image.
    std::uint32_t display_width{kWidth};
    std::uint32_t display_height{kHeight};
    std::uint32_t world_width{kWorldWidth};
    std::uint32_t world_height{kWorldHeight};
    VkSampler world_sampler{VK_NULL_HANDLE};
    VkPipelineLayout pipeline_layout{VK_NULL_HANDLE};
    // Five PSP blend variants, each with 34 depth/texture combinations.
    std::array<VkPipeline, 170> pipelines{};
    Buffer vertices_gpu{};
    Buffer readback{};
    std::vector<GeGpuVertex> vertices;
    std::vector<DrawBatch> batches;
    std::unordered_map<std::uint64_t, Texture> textures;
    std::uint64_t texture_bytes{};
    std::vector<std::byte> last_texture_rgba;
    std::vector<std::byte> frame_rgba;
    std::uint32_t display_framebuffer{};
};

VulkanPreview &state() {
    static VulkanPreview preview;
    return preview;
}

void log_error(const char *operation, VkResult result) {
    __android_log_print(ANDROID_LOG_ERROR, "VCSVulkan", "%s failed: %d",
                        operation, static_cast<int>(result));
}

std::uint32_t blend_variant(const GeGpuDrawDescriptor &draw) noexcept {
    if (!draw.blend_enabled || draw.clear_mode) return 0u;
    const std::uint32_t eq = draw.blend_equation & 7u;
    const std::uint32_t src = draw.blend_source_factor & 0xFu;
    const std::uint32_t dst = draw.blend_dest_factor & 0xFu;
    if (eq == 0u && src == 2u && dst == 3u) return 1u;
    if (eq == 0u && src == 10u && dst == 10u) {
        const std::uint32_t fs = draw.blend_fix_source & 0x00FFFFFFu;
        const std::uint32_t fd = draw.blend_fix_dest & 0x00FFFFFFu;
        if (fs == 0x00FFFFFFu && fd == 0u) return 0u;
        if (fs == 0x00FFFFFFu && fd == 0x00FFFFFFu) return 2u;
        bool complements = true;
        for (std::uint32_t shift = 0u; shift < 24u; shift += 8u)
            complements &= (((fs >> shift) & 0xFFu) + ((fd >> shift) & 0xFFu)) == 0xFFu;
        if (complements) return 3u;
    }
    if (eq == 0u && src == 2u && dst == 10u &&
        (draw.blend_fix_dest & 0x00FFFFFFu) == 0x00FFFFFFu) return 4u;
    return 0u;
}

std::uint32_t memory_type(VkPhysicalDevice physical, std::uint32_t bits,
                          VkMemoryPropertyFlags required) {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    for (std::uint32_t index = 0u; index < properties.memoryTypeCount; ++index) {
        if ((bits & (1u << index)) != 0u &&
            (properties.memoryTypes[index].propertyFlags & required) == required)
            return index;
    }
    return UINT32_MAX;
}

void destroy_buffer(VulkanPreview &s, Buffer &buffer) {
    if (buffer.mapped != nullptr) vkUnmapMemory(s.device, buffer.memory);
    if (buffer.handle != VK_NULL_HANDLE) vkDestroyBuffer(s.device, buffer.handle, nullptr);
    if (buffer.memory != VK_NULL_HANDLE) vkFreeMemory(s.device, buffer.memory, nullptr);
    buffer = {};
}

void destroy_texture(VulkanPreview &s, Texture &texture) {
    destroy_buffer(s, texture.staging);
    if (texture.descriptor != VK_NULL_HANDLE && s.descriptor_pool != VK_NULL_HANDLE)
        vkFreeDescriptorSets(s.device, s.descriptor_pool, 1u, &texture.descriptor);
    if (texture.view != VK_NULL_HANDLE) vkDestroyImageView(s.device, texture.view, nullptr);
    if (texture.image != VK_NULL_HANDLE) vkDestroyImage(s.device, texture.image, nullptr);
    if (texture.memory != VK_NULL_HANDLE) vkFreeMemory(s.device, texture.memory, nullptr);
    texture = {};
}

std::uint64_t hash_mix(std::uint64_t hash, std::uint64_t value) noexcept {
    hash ^= value + 0x9E3779B97F4A7C15ull + (hash << 6u) + (hash >> 2u);
    return hash;
}

std::uint64_t texture_key(const GeGpuDrawDescriptor &draw) noexcept {
    if (draw.texture_cache_key_hint != 0u) return draw.texture_cache_key_hint;
    std::uint64_t key = 0xCBF29CE484222325ull;
    const std::uint32_t levels = draw.texture_mipmap_enabled
        ? std::min<std::uint32_t>(8u, draw.texture_max_level + 1u) : 1u;
    key = hash_mix(key, levels);
    for (std::uint32_t level = 0u; level < levels; ++level) {
        key = hash_mix(key, draw.texture_level_addresses[level] != 0u
            ? draw.texture_level_addresses[level] : draw.texture_address);
        key = hash_mix(key, draw.texture_level_buffer_widths[level] != 0u
            ? draw.texture_level_buffer_widths[level] : draw.texture_buffer_width);
        key = hash_mix(key, draw.texture_level_widths[level] != 0u
            ? draw.texture_level_widths[level] : draw.texture_width);
        key = hash_mix(key, draw.texture_level_heights[level] != 0u
            ? draw.texture_level_heights[level] : draw.texture_height);
    }
    for (std::uint64_t value : {
             static_cast<std::uint64_t>(draw.texture_format),
             static_cast<std::uint64_t>(draw.clut_address),
             static_cast<std::uint64_t>(draw.clut_format),
             static_cast<std::uint64_t>(draw.clut_shift),
             static_cast<std::uint64_t>(draw.clut_mask),
             static_cast<std::uint64_t>(draw.clut_start),
             static_cast<std::uint64_t>(draw.clut_checksum),
             static_cast<std::uint64_t>(draw.texture_swizzled),
             static_cast<std::uint64_t>(draw.texture_min_linear),
             static_cast<std::uint64_t>(draw.texture_mag_linear),
             static_cast<std::uint64_t>(draw.texture_mipmap_enabled),
             static_cast<std::uint64_t>(draw.texture_mipmap_linear),
             static_cast<std::uint64_t>(draw.texture_max_level),
             static_cast<std::uint64_t>(draw.texture_level_mode),
             static_cast<std::uint64_t>(static_cast<std::uint32_t>(draw.texture_level_offset16)),
             static_cast<std::uint64_t>(draw.texture_selected_level),
             static_cast<std::uint64_t>(draw.texture_clamp_u),
             static_cast<std::uint64_t>(draw.texture_clamp_v)})
        key = hash_mix(key, value);
    return key;
}

bool create_buffer(VulkanPreview &s, VkDeviceSize bytes, VkBufferUsageFlags usage,
                   Buffer &buffer) {
    VkBufferCreateInfo description{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    description.size = bytes;
    description.usage = usage;
    description.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(s.device, &description, nullptr, &buffer.handle) != VK_SUCCESS) return false;
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(s.device, buffer.handle, &requirements);
    const std::uint32_t type = memory_type(s.physical, requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX) return false;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = type;
    if (vkAllocateMemory(s.device, &allocation, nullptr, &buffer.memory) != VK_SUCCESS) return false;
    if (vkBindBufferMemory(s.device, buffer.handle, buffer.memory, 0u) != VK_SUCCESS) return false;
    return vkMapMemory(s.device, buffer.memory, 0u, bytes, 0u, &buffer.mapped) == VK_SUCCESS;
}

void destroy_backend(VulkanPreview &s) {
    if (s.device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(s.device);
        destroy_buffer(s, s.vertices_gpu);
        destroy_buffer(s, s.readback);
        for (auto &[key, texture] : s.textures) destroy_texture(s, texture);
        for (VkPipeline pipeline : s.pipelines)
            if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(s.device, pipeline, nullptr);
        if (s.pipeline_layout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(s.device, s.pipeline_layout, nullptr);
        if (s.vertex_shader != VK_NULL_HANDLE)
            vkDestroyShaderModule(s.device, s.vertex_shader, nullptr);
        if (s.fragment_shader != VK_NULL_HANDLE)
            vkDestroyShaderModule(s.device, s.fragment_shader, nullptr);
        if (s.textured_shader != VK_NULL_HANDLE)
            vkDestroyShaderModule(s.device, s.textured_shader, nullptr);
    if (s.world_sampler != VK_NULL_HANDLE)
        vkDestroySampler(s.device, s.world_sampler, nullptr);
    if (s.sampler != VK_NULL_HANDLE) vkDestroySampler(s.device, s.sampler, nullptr);
    for (VkSampler sampler : s.texture_samplers)
        if (sampler != VK_NULL_HANDLE) vkDestroySampler(s.device, sampler, nullptr);
        if (s.descriptor_pool != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(s.device, s.descriptor_pool, nullptr);
        if (s.descriptor_layout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(s.device, s.descriptor_layout, nullptr);
        if (s.framebuffer != VK_NULL_HANDLE)
            vkDestroyFramebuffer(s.device, s.framebuffer, nullptr);
        if (s.world_framebuffer != VK_NULL_HANDLE)
            vkDestroyFramebuffer(s.device, s.world_framebuffer, nullptr);
        if (s.render_pass != VK_NULL_HANDLE)
            vkDestroyRenderPass(s.device, s.render_pass, nullptr);
        if (s.color_view != VK_NULL_HANDLE)
            vkDestroyImageView(s.device, s.color_view, nullptr);
        if (s.world_color_view != VK_NULL_HANDLE)
            vkDestroyImageView(s.device, s.world_color_view, nullptr);
        if (s.depth_view != VK_NULL_HANDLE)
            vkDestroyImageView(s.device, s.depth_view, nullptr);
        if (s.color != VK_NULL_HANDLE) vkDestroyImage(s.device, s.color, nullptr);
        if (s.world_color != VK_NULL_HANDLE) vkDestroyImage(s.device, s.world_color, nullptr);
        if (s.depth != VK_NULL_HANDLE) vkDestroyImage(s.device, s.depth, nullptr);
        if (s.color_memory != VK_NULL_HANDLE)
            vkFreeMemory(s.device, s.color_memory, nullptr);
        if (s.world_color_memory != VK_NULL_HANDLE)
            vkFreeMemory(s.device, s.world_color_memory, nullptr);
        if (s.depth_memory != VK_NULL_HANDLE)
            vkFreeMemory(s.device, s.depth_memory, nullptr);
        if (s.fence != VK_NULL_HANDLE) vkDestroyFence(s.device, s.fence, nullptr);
        if (s.command_pool != VK_NULL_HANDLE)
            vkDestroyCommandPool(s.device, s.command_pool, nullptr);
        vkDestroyDevice(s.device, nullptr);
    }
    if (s.instance != VK_NULL_HANDLE) vkDestroyInstance(s.instance, nullptr);
    s = {};
}

bool create_color_image(VulkanPreview &s, VkImage &handle,
                        VkDeviceMemory &memory, VkImageView &image_view,
                        VkImageUsageFlags extra_usage,
                        std::uint32_t width = kWidth, std::uint32_t height = kHeight) {
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = VK_FORMAT_R8G8B8A8_UNORM;
    image.extent = {width, height, 1u};
    image.mipLevels = 1u;
    image.arrayLayers = 1u;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                  extra_usage;
    image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(s.device, &image, nullptr, &handle) != VK_SUCCESS) return false;
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(s.device, handle, &requirements);
    const std::uint32_t type = memory_type(s.physical, requirements.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) return false;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = type;
    if (vkAllocateMemory(s.device, &allocation, nullptr, &memory) != VK_SUCCESS) return false;
    if (vkBindImageMemory(s.device, handle, memory, 0u) != VK_SUCCESS) return false;

    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = handle;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = image.format;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = 1u;
    view.subresourceRange.layerCount = 1u;
    return vkCreateImageView(s.device, &view, nullptr, &image_view) == VK_SUCCESS;
}

bool create_depth_image(VulkanPreview &s) {
    VkFormatProperties format{};
    vkGetPhysicalDeviceFormatProperties(s.physical, VK_FORMAT_D16_UNORM, &format);
    if ((format.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0u)
        return false;
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = VK_FORMAT_D16_UNORM;
    // Shared by both passes, so sized for the larger world one; an attachment
    // may be bigger than the framebuffer that uses it.
    image.extent = {std::max(s.world_width, s.display_width),
                    std::max(s.world_height, s.display_height), 1u};
    image.mipLevels = 1u;
    image.arrayLayers = 1u;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(s.device, &image, nullptr, &s.depth) != VK_SUCCESS) return false;
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(s.device, s.depth, &requirements);
    const std::uint32_t type = memory_type(s.physical, requirements.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) return false;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = type;
    if (vkAllocateMemory(s.device, &allocation, nullptr, &s.depth_memory) != VK_SUCCESS ||
        vkBindImageMemory(s.device, s.depth, s.depth_memory, 0u) != VK_SUCCESS) return false;
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = s.depth;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = VK_FORMAT_D16_UNORM;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    view.subresourceRange.levelCount = 1u;
    view.subresourceRange.layerCount = 1u;
    return vkCreateImageView(s.device, &view, nullptr, &s.depth_view) == VK_SUCCESS;
}

bool create_pipeline(VulkanPreview &s) {
    VkAttachmentDescription attachment{};
    attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentDescription depth_attachment{};
    depth_attachment.format = VK_FORMAT_D16_UNORM;
    depth_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth_attachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    const std::array<VkAttachmentDescription, 2> attachments{attachment, depth_attachment};
    VkAttachmentReference color_reference{0u, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depth_reference{1u, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1u;
    subpass.pColorAttachments = &color_reference;
    subpass.pDepthStencilAttachment = &depth_reference;
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0u;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstStageMask = dependency.srcStageMask;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    pass.attachmentCount = static_cast<std::uint32_t>(attachments.size());
    pass.pAttachments = attachments.data();
    pass.subpassCount = 1u;
    pass.pSubpasses = &subpass;
    pass.dependencyCount = 1u;
    pass.pDependencies = &dependency;
    if (vkCreateRenderPass(s.device, &pass, nullptr, &s.render_pass) != VK_SUCCESS) return false;

    VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    framebuffer.renderPass = s.render_pass;
    const std::array<VkImageView, 2> views{s.color_view, s.depth_view};
    framebuffer.attachmentCount = static_cast<std::uint32_t>(views.size());
    framebuffer.pAttachments = views.data();
    framebuffer.width = s.display_width;
    framebuffer.height = s.display_height;
    framebuffer.layers = 1u;
    if (vkCreateFramebuffer(s.device, &framebuffer, nullptr, &s.framebuffer) != VK_SUCCESS) return false;
    const std::array<VkImageView, 2> world_views{s.world_color_view, s.depth_view};
    framebuffer.pAttachments = world_views.data();
    framebuffer.width = s.world_width;
    framebuffer.height = s.world_height;
    if (vkCreateFramebuffer(s.device, &framebuffer, nullptr,
                            &s.world_framebuffer) != VK_SUCCESS) return false;

    const auto make_shader = [&](const std::uint32_t *words, std::size_t bytes,
                                 VkShaderModule &output) {
        VkShaderModuleCreateInfo description{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        description.codeSize = bytes;
        description.pCode = words;
        return vkCreateShaderModule(s.device, &description, nullptr, &output) == VK_SUCCESS;
    };
    if (!make_shader(vulkan_shaders::vertex, sizeof(vulkan_shaders::vertex), s.vertex_shader) ||
        !make_shader(vulkan_shaders::fragment, sizeof(vulkan_shaders::fragment), s.fragment_shader) ||
        !make_shader(vulkan_shaders::textured_fragment,
                     sizeof(vulkan_shaders::textured_fragment), s.textured_shader))
        return false;

    VkDescriptorSetLayoutBinding texture_binding{};
    texture_binding.binding = 0u;
    texture_binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    texture_binding.descriptorCount = 1u;
    texture_binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo descriptor_layout{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    descriptor_layout.bindingCount = 1u;
    descriptor_layout.pBindings = &texture_binding;
    if (vkCreateDescriptorSetLayout(s.device, &descriptor_layout, nullptr,
                                    &s.descriptor_layout) != VK_SUCCESS) return false;
    // One descriptor set per cached texture. This was a fixed 1024, below the
    // texture cache's own limit, so once a city scene held that many textures
    // every new one failed to allocate its set and its draws - the HUD text
    // among them - lost their texture. Sized from the cache limit now, which
    // is itself capped by how many separate allocations the driver allows
    // (each texture owns its image memory).
    VkPhysicalDeviceProperties device_properties{};
    vkGetPhysicalDeviceProperties(s.physical, &device_properties);
    const std::uint32_t allocation_budget =
        device_properties.limits.maxMemoryAllocationCount > 256u
            ? device_properties.limits.maxMemoryAllocationCount - 256u : 256u;
    s.texture_capacity = std::max<std::uint32_t>(256u, std::min<std::uint32_t>(
        vcs_configuration().rendering.texture_cache_entries, std::min(allocation_budget, 8192u)));
    const std::uint32_t pool_sets = s.texture_capacity + 16u;
    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, pool_sets};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool.maxSets = pool_sets;
    pool.poolSizeCount = 1u;
    pool.pPoolSizes = &pool_size;
    if (vkCreateDescriptorPool(s.device, &pool, nullptr, &s.descriptor_pool) != VK_SUCCESS)
        return false;
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = VK_FILTER_NEAREST;
    sampler.minFilter = VK_FILTER_NEAREST;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.maxLod = 0.0f;
    if (vkCreateSampler(s.device, &sampler, nullptr, &s.sampler) != VK_SUCCESS) return false;
    // The composition quads can address outside the 480x272 visible part of
    // the PSP framebuffer. Repeating the GPU image duplicated the scene at
    // the sides and bottom of the screen.
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(s.device, &sampler, nullptr, &s.world_sampler) != VK_SUCCESS)
        return false;
    VkDescriptorSetAllocateInfo world_set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    world_set.descriptorPool = s.descriptor_pool;
    world_set.descriptorSetCount = 1u;
    world_set.pSetLayouts = &s.descriptor_layout;
    if (vkAllocateDescriptorSets(s.device, &world_set, &s.world_descriptor) != VK_SUCCESS)
        return false;
    VkDescriptorImageInfo world_image_info{s.world_sampler, s.world_color_view,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet world_write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    world_write.dstSet = s.world_descriptor;
    world_write.descriptorCount = 1u;
    world_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    world_write.pImageInfo = &world_image_info;
    vkUpdateDescriptorSets(s.device, 1u, &world_write, 0u, nullptr);

    std::array<VkPushConstantRange, 2> push{};
    push[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push[0].size = 4u * sizeof(float);
    push[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    push[1].offset = 4u * sizeof(float);
    push[1].size = 16u * sizeof(float);
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1u;
    layout.pSetLayouts = &s.descriptor_layout;
    layout.pushConstantRangeCount = static_cast<std::uint32_t>(push.size());
    layout.pPushConstantRanges = push.data();
    if (vkCreatePipelineLayout(s.device, &layout, nullptr, &s.pipeline_layout) != VK_SUCCESS)
        return false;

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    for (std::size_t index = 0u; index < stages.size(); ++index) {
        stages[index].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[index].stage = index == 0u ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[index].module = index == 0u ? s.vertex_shader : s.fragment_shader;
        stages[index].pName = "main";
    }
    VkVertexInputBindingDescription binding{0u, sizeof(GeGpuVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    std::array<VkVertexInputAttributeDescription, 5> attributes{};
    attributes[0] = {0u, 0u, VK_FORMAT_R32G32B32A32_SFLOAT,
                     static_cast<std::uint32_t>(offsetof(GeGpuVertex, x))};
    attributes[1] = {1u, 0u, VK_FORMAT_R8G8B8A8_UNORM,
                     static_cast<std::uint32_t>(offsetof(GeGpuVertex, rgba))};
    attributes[2] = {2u, 0u, VK_FORMAT_R32G32_SFLOAT,
                     static_cast<std::uint32_t>(offsetof(GeGpuVertex, u))};
    attributes[3] = {3u, 0u, VK_FORMAT_R32_SFLOAT,
                     static_cast<std::uint32_t>(offsetof(GeGpuVertex, q))};
    attributes[4] = {4u, 0u, VK_FORMAT_R32_SFLOAT,
                     static_cast<std::uint32_t>(offsetof(GeGpuVertex, fog_factor))};
    VkPipelineVertexInputStateCreateInfo vertex_input{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex_input.vertexBindingDescriptionCount = 1u;
    vertex_input.pVertexBindingDescriptions = &binding;
    vertex_input.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
    vertex_input.pVertexAttributeDescriptions = attributes.data();
    VkPipelineInputAssemblyStateCreateInfo assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport{0.0f, 0.0f, static_cast<float>(kWidth),
                        static_cast<float>(kHeight), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {kWidth, kHeight}};
    VkPipelineViewportStateCreateInfo viewport_state{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport_state.viewportCount = 1u;
    viewport_state.pViewports = &viewport;
    viewport_state.scissorCount = 1u;
    viewport_state.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo raster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.blendEnable = VK_TRUE;
    blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
    blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blending{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blending.attachmentCount = 1u;
    blending.pAttachments = &blend_attachment;
    const std::array<VkDynamicState, 3> dynamic_states{
        VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_VIEWPORT};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size());
    dynamic.pDynamicStates = dynamic_states.data();
    VkGraphicsPipelineCreateInfo description{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    description.stageCount = static_cast<std::uint32_t>(stages.size());
    description.pStages = stages.data();
    description.pVertexInputState = &vertex_input;
    description.pInputAssemblyState = &assembly;
    description.pViewportState = &viewport_state;
    description.pRasterizationState = &raster;
    description.pMultisampleState = &multisample;
    description.pColorBlendState = &blending;
    description.pDynamicState = &dynamic;
    description.layout = s.pipeline_layout;
    description.renderPass = s.render_pass;
    VkPipelineDepthStencilStateCreateInfo depth_state{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    description.pDepthStencilState = &depth_state;
    constexpr std::array<VkCompareOp, 8> compare{
        VK_COMPARE_OP_NEVER, VK_COMPARE_OP_ALWAYS, VK_COMPARE_OP_EQUAL,
        VK_COMPARE_OP_NOT_EQUAL, VK_COMPARE_OP_LESS, VK_COMPARE_OP_LESS_OR_EQUAL,
        VK_COMPARE_OP_GREATER, VK_COMPARE_OP_GREATER_OR_EQUAL};
    for (std::uint32_t index = 0u; index < s.pipelines.size(); ++index) {
        const std::uint32_t base = index % 34u;
        const std::uint32_t blend = index / 34u;
        const bool depth_enabled = base >= 2u;
        const std::uint32_t state = depth_enabled ? base - 2u : 0u;
        const bool textured = (base & 1u) != 0u;
        blend_attachment.blendEnable = blend != 0u ? VK_TRUE : VK_FALSE;
        blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        switch (blend) {
        case 1u:
            blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            break;
        case 2u:
            blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            break;
        case 3u:
            blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_COLOR;
            blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
            break;
        case 4u:
            blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            break;
        default: break;
        }
        depth_state.depthTestEnable = depth_enabled ? VK_TRUE : VK_FALSE;
        depth_state.depthWriteEnable = depth_enabled && ((state >> 1u) & 1u) != 0u
            ? VK_TRUE : VK_FALSE;
        depth_state.depthCompareOp = depth_enabled ? compare[state >> 2u] : VK_COMPARE_OP_ALWAYS;
        stages[1].module = textured ? s.textured_shader : s.fragment_shader;
        if (vkCreateGraphicsPipelines(s.device, VK_NULL_HANDLE, 1u, &description,
                                      nullptr, &s.pipelines[index]) != VK_SUCCESS) return false;
        if (depth_enabled) ++s.report.depth_pipeline_variants_created;
    }
    return true;
}

bool create_backend(VulkanPreview &s, std::string &error) {
    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "VCS Android";
    application.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo instance{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance.pApplicationInfo = &application;
    if (vkCreateInstance(&instance, nullptr, &s.instance) != VK_SUCCESS) {
        error = "vkCreateInstance failed";
        return false;
    }
    std::uint32_t count{};
    if (vkEnumeratePhysicalDevices(s.instance, &count, nullptr) != VK_SUCCESS || count == 0u) {
        error = "No Vulkan GPU found";
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    if (vkEnumeratePhysicalDevices(s.instance, &count, devices.data()) != VK_SUCCESS) {
        error = "vkEnumeratePhysicalDevices failed";
        return false;
    }
    s.report.physical_device_count = count;
    for (VkPhysicalDevice physical : devices) {
        std::uint32_t families{};
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, nullptr);
        std::vector<VkQueueFamilyProperties> queues(families);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, queues.data());
        for (std::uint32_t index = 0u; index < families; ++index) {
            if ((queues[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0u) continue;
            s.physical = physical;
            s.queue_family = index;
            break;
        }
        if (s.physical != VK_NULL_HANDLE) break;
    }
    if (s.physical == VK_NULL_HANDLE) {
        error = "No Vulkan graphics queue found";
        return false;
    }
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = s.queue_family;
    queue.queueCount = 1u;
    queue.pQueuePriorities = &priority;
    VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device.queueCreateInfoCount = 1u;
    device.pQueueCreateInfos = &queue;
    if (vkCreateDevice(s.physical, &device, nullptr, &s.device) != VK_SUCCESS) {
        error = "vkCreateDevice failed";
        return false;
    }
    vkGetDeviceQueue(s.device, s.queue_family, 0u, &s.queue);
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = s.queue_family;
    if (vkCreateCommandPool(s.device, &pool, nullptr, &s.command_pool) != VK_SUCCESS) {
        error = "vkCreateCommandPool failed";
        return false;
    }
    VkCommandBufferAllocateInfo commands{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    commands.commandPool = s.command_pool;
    commands.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commands.commandBufferCount = 1u;
    if (vkAllocateCommandBuffers(s.device, &commands, &s.command) != VK_SUCCESS) {
        error = "vkAllocateCommandBuffers failed";
        return false;
    }
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(s.device, &fence, nullptr, &s.fence) != VK_SUCCESS ||
        !create_color_image(s, s.color, s.color_memory, s.color_view, 0u,
                            s.display_width, s.display_height) ||
        !create_color_image(s, s.world_color, s.world_color_memory,
                            s.world_color_view, VK_IMAGE_USAGE_SAMPLED_BIT,
                            s.world_width, s.world_height) ||
        !create_depth_image(s) || !create_pipeline(s) ||
        !create_buffer(s, kVertexCapacity, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, s.vertices_gpu) ||
        !create_buffer(s, static_cast<VkDeviceSize>(s.display_width) * s.display_height * 4u,
                       VK_BUFFER_USAGE_TRANSFER_DST_BIT, s.readback)) {
        error = "Vulkan preview resource creation failed";
        return false;
    }
    s.frame_rgba.resize(static_cast<std::size_t>(s.display_width) * s.display_height * 4u);
    return true;
}

std::uint32_t texture_sampler_index(const GeGpuDrawDescriptor &draw) noexcept {
    return (draw.texture_min_linear ? 1u : 0u) | (draw.texture_mag_linear ? 2u : 0u) |
           (draw.texture_clamp_u ? 4u : 0u) | (draw.texture_clamp_v ? 8u : 0u);
}

// The sampler for this draw's PSP filter/wrap state, created on first use.
// Falls back to the shared nearest sampler if creation fails.
VkSampler texture_sampler_for(VulkanPreview &s, const GeGpuDrawDescriptor &draw) {
    const std::uint32_t index = texture_sampler_index(draw);
    VkSampler &slot = s.texture_samplers[index];
    if (slot != VK_NULL_HANDLE) return slot;
    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.minFilter = draw.texture_min_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    info.magFilter = draw.texture_mag_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = draw.texture_clamp_u ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
                                             : VK_SAMPLER_ADDRESS_MODE_REPEAT;
    info.addressModeV = draw.texture_clamp_v ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
                                             : VK_SAMPLER_ADDRESS_MODE_REPEAT;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.maxLod = 0.0f;  // only the base level is uploaded
    if (vkCreateSampler(s.device, &info, nullptr, &slot) != VK_SUCCESS) {
        slot = VK_NULL_HANDLE;
        return s.sampler;
    }
    return slot;
}

bool create_texture(VulkanPreview &s, std::uint32_t width, std::uint32_t height,
                    std::span<const std::byte> rgba, Texture &texture,
                    VkSampler sampler = VK_NULL_HANDLE) {
    if (width == 0u || height == 0u || width > 2048u || height > 2048u) return false;
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * height * 4u;
    if (rgba.size() < bytes) return false;
    texture.width = width;
    texture.height = height;
    texture.bytes = bytes;
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = VK_FORMAT_R8G8B8A8_UNORM;
    image.extent = {width, height, 1u};
    image.mipLevels = 1u;
    image.arrayLayers = 1u;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(s.device, &image, nullptr, &texture.image) != VK_SUCCESS) return false;
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(s.device, texture.image, &requirements);
    const std::uint32_t type = memory_type(s.physical, requirements.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) return false;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = type;
    if (vkAllocateMemory(s.device, &allocation, nullptr, &texture.memory) != VK_SUCCESS ||
        vkBindImageMemory(s.device, texture.image, texture.memory, 0u) != VK_SUCCESS)
        return false;
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = texture.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = image.format;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = 1u;
    view.subresourceRange.layerCount = 1u;
    if (vkCreateImageView(s.device, &view, nullptr, &texture.view) != VK_SUCCESS ||
        !create_buffer(s, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, texture.staging))
        return false;
    std::memcpy(texture.staging.mapped, rgba.data(), static_cast<std::size_t>(bytes));
    VkDescriptorSetAllocateInfo descriptor{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    descriptor.descriptorPool = s.descriptor_pool;
    descriptor.descriptorSetCount = 1u;
    descriptor.pSetLayouts = &s.descriptor_layout;
    if (vkAllocateDescriptorSets(s.device, &descriptor, &texture.descriptor) != VK_SUCCESS)
        return false;
    VkDescriptorImageInfo image_info{sampler != VK_NULL_HANDLE ? sampler : s.sampler, texture.view,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = texture.descriptor;
    write.descriptorCount = 1u;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image_info;
    vkUpdateDescriptorSets(s.device, 1u, &write, 0u, nullptr);
    texture.pending = true;
    return true;
}

void record_texture_uploads(VulkanPreview &s) {
    for (auto &[key, texture] : s.textures) {
        if (!texture.pending || texture.recorded) continue;
        texture.recorded = true;
        VkImageMemoryBarrier before{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        before.srcAccessMask = 0u;
        before.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        before.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        before.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        before.image = texture.image;
        before.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        before.subresourceRange.levelCount = 1u;
        before.subresourceRange.layerCount = 1u;
        vkCmdPipelineBarrier(s.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &before);
        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1u;
        copy.imageExtent = {texture.width, texture.height, 1u};
        vkCmdCopyBufferToImage(s.command, texture.staging.handle, texture.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &copy);
        VkImageMemoryBarrier after = before;
        after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        after.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        after.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        after.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(s.command, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &after);
    }
}

} // namespace

bool initialize_ge_gpu_backend(std::string &error) {
    VulkanPreview &s = state();
    destroy_backend(s);
    s.report.requested = GeGpuBackendKind::Software;
    s.report.active = GeGpuBackendKind::Software;
    // This bring-up path draws real GE triangle lists into a Vulkan image.
    // Texture, depth and framebuffer feedback are still handled by the software
    // renderer. Opt in to capture/inspect the image without changing gameplay.
    const char *preview = std::getenv("PSPRECOMP_ANDROID_VULKAN_PREVIEW");
    const bool preview_enabled =
        (preview != nullptr && *preview != '\0' && *preview != '0') ||
        vcs_configuration().rendering.backend == RenderingBackend::VulkanPreview ||
        vcs_configuration().rendering.backend == RenderingBackend::Vulkan;
    if (!preview_enabled) {
        s.report.message = "Software GE active; Vulkan preview disabled";
        error.clear();
        return true;
    }
    s.report.requested = GeGpuBackendKind::Vulkan;
    {
        // The configured internal resolution is the display target; the world
        // surface gets the same scale factor over its own 512x320.
        const InternalResolutionDimensions dims =
            resolve_internal_resolution(vcs_configuration().rendering);
        s.display_height = std::clamp<std::uint32_t>(dims.height, kHeight, 4096u);
        s.display_width = std::clamp<std::uint32_t>(dims.width, kWidth, 4096u);
        // Widescreen: the game widens its own frustum to the panel's aspect
        // and the picture is shown full-screen, so the target takes the
        // panel's shape too - otherwise its pixels are stretched sideways.
        const VcsConfiguration &config = vcs_configuration();
        const DisplaySurfaceDimensions panel = resolve_display_surface_dimensions(config.display);
        if (widescreen_stretch_factor(config, panel.width, panel.height) > 1.0001f &&
            panel.width != 0u && panel.height != 0u) {
            s.display_width = std::clamp<std::uint32_t>(static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(s.display_height) * panel.width + panel.height / 2u) /
                panel.height), kWidth, 4096u);
        }
        s.world_width = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(kWorldWidth) * s.display_width + kWidth / 2u) / kWidth);
        s.world_height = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(kWorldHeight) * s.display_height + kHeight / 2u) / kHeight);
    }
    if (!create_backend(s, error)) {
        __android_log_print(ANDROID_LOG_WARN, "VCSVulkan", "Falling back to software: %s",
                            error.c_str());
        destroy_backend(s);
        s.report.requested = GeGpuBackendKind::Vulkan;
        s.report.active = GeGpuBackendKind::Software;
        s.report.message = "Vulkan initialization failed; using software GE";
        error.clear();
        return true;
    }
    s.enabled = true;
    s.authoritative = vcs_configuration().rendering.backend == RenderingBackend::Vulkan;
    s.report.requested = GeGpuBackendKind::Vulkan;
    s.report.active = GeGpuBackendKind::Vulkan;
    s.report.loader_opened = true;
    s.report.instance_created = true;
    s.report.device_created = true;
    s.report.graphics_queue_family = s.queue_family;
    s.report.command_pool_created = true;
    s.report.offscreen_image_created = true;
    s.report.offscreen_image_memory_bound = true;
    s.report.offscreen_image_view_created = true;
    s.report.depth_image_created = true;
    s.report.depth_image_memory_bound = true;
    s.report.depth_image_view_created = true;
    s.report.depth_attachment_active = true;
    s.report.transfer_buffer_created = true;
    s.report.transfer_memory_mapped = true;
    s.report.render_pass_created = true;
    s.report.framebuffer_created = true;
    s.report.shader_modules_created = true;
    s.report.graphics_pipeline_created = true;
    s.report.texture_descriptor_layout_created = true;
    s.report.texture_descriptor_pool_created = true;
    s.report.textured_shader_modules_created = true;
    s.report.textured_pipeline_created = true;
    s.report.alpha_test_shader_active = true;
    s.report.observed_texture_function_shader_active = true;
    s.report.offscreen_width = s.display_width;
    s.report.offscreen_height = s.display_height;
    s.report.message = s.authoritative
        ? "Vulkan GE active: displayed framebuffer is rasterized on GPU"
        : "Vulkan GE preview: color, RGBA textures, alpha and D16 depth";
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(s.physical, &properties);
    __android_log_print(ANDROID_LOG_INFO, "VCSVulkan",
        "%s initialized on %s: %zu pipelines, display %ux%u, world %ux%u",
        s.authoritative ? "Renderer" : "Preview", properties.deviceName,
        s.pipelines.size(), s.display_width, s.display_height, s.world_width, s.world_height);
    error.clear();
    return true;
}

void shutdown_ge_gpu_backend() noexcept { destroy_backend(state()); }
bool ge_gpu_backend_active() noexcept { return state().enabled; }
bool ge_gpu_backend_transfer_ready() noexcept { return state().enabled; }
bool ge_gpu_backend_graphics_ready() noexcept { return state().enabled; }
void ge_gpu_backend_record_draw(const GeGpuDrawDescriptor &draw) noexcept {
    auto &s = state();
    if (!s.enabled) return;
    ++s.report.draw_calls;
    s.report.vertices += draw.vertex_count;
    if (s.authoritative && !draw.through && !draw.clear_mode &&
        (draw.framebuffer_address & 0x001FFFF0u) != s.display_framebuffer) {
        s.world_framebuffer_address = draw.framebuffer_address & 0x001FFFF0u;
        s.frame_has_scene = true;
    }
}
void ge_gpu_backend_observe_camera(const std::array<float, 12> &,
                                   const std::array<float, 16> &,
                                   const std::array<float, 6> &,
                                   const std::array<float, 3> &,
                                   const GeGpuDrawDescriptor &,
                                   std::uint32_t) noexcept {}
bool ge_gpu_backend_stage_vertices(const GeGpuDrawDescriptor &,
                                   std::span<const GeGpuVertex>) noexcept { return false; }
bool ge_gpu_backend_texture_needed(const GeGpuDrawDescriptor &draw) noexcept {
    auto &s = state();
    if (ge_gpu_backend_is_framebuffer_feedback_texture(draw)) return false;
    if (!s.enabled || !draw.texture_enabled || draw.texture_width == 0u ||
        draw.texture_height == 0u || draw.texture_format > 10u) return false;
    ++s.report.texture_decode_requests;
    const auto found = s.textures.find(texture_key(draw));
    if (found == s.textures.end() || found->second.image == VK_NULL_HANDLE) return true;
    found->second.checked_frame = s.report.game_frames;
    if (draw.texture_content_signature != 0u &&
        found->second.signature != draw.texture_content_signature) return true;
    ++s.report.texture_cache_hits;
    return false;
}
void ge_gpu_backend_prepare_texture_keys(GeGpuDrawDescriptor &draw) noexcept {
    if (!draw.texture_enabled) {
        draw.texture_cache_key_hint = 0u;
        draw.texture_image_key_hint = 0u;
        return;
    }
    draw.texture_cache_key_hint = 0u;
    draw.texture_image_key_hint = 0u;
    draw.texture_cache_key_hint = texture_key(draw);
    draw.texture_image_key_hint = draw.texture_cache_key_hint;
}
bool ge_gpu_backend_texture_signature_needed(const GeGpuDrawDescriptor &draw) noexcept {
    auto &s = state();
    if (ge_gpu_backend_is_framebuffer_feedback_texture(draw)) return false;
    if (!s.enabled || !draw.texture_enabled || draw.texture_width == 0u ||
        draw.texture_height == 0u) return false;
    const auto found = s.textures.find(texture_key(draw));
    return found == s.textures.end() || found->second.checked_frame != s.report.game_frames;
}
bool ge_gpu_backend_is_framebuffer_feedback_texture(const GeGpuDrawDescriptor &draw) noexcept {
    const auto &s = state();
    return s.enabled && s.authoritative && !s.software_menu_active &&
        s.world_framebuffer_address != 0u && draw.texture_enabled &&
        (draw.texture_address & 0x001FFFF0u) == s.world_framebuffer_address &&
        (draw.framebuffer_address & 0x001FFFF0u) == s.display_framebuffer;
}
GeGpuWidescreenHud ge_gpu_backend_widescreen_hud(const GeGpuDrawDescriptor &draw) noexcept {
    // The widened frustum fixes the 3D view; the 2D interface has no
    // projection, so without this it is stretched across the wider screen
    // along with everything else. Same correction as the DX12 backend: pull
    // through-mode draws back toward the centre of their own target.
    GeGpuWidescreenHud hud{};
    const auto &s = state();
    if (!s.enabled || !s.authoritative) return hud;
    hud.gameplay_world = s.frame_has_scene && s.world_framebuffer_address != 0u &&
        (draw.framebuffer_address & 0x001FFFF0u) == s.world_framebuffer_address;
    const VcsConfiguration &config = vcs_configuration();
    if (!config.initialized || !config.widescreen.enabled) return hud;
    const DisplaySurfaceDimensions panel = resolve_display_surface_dimensions(config.display);
    const float shrink = widescreen_stretch_factor(config, panel.width, panel.height);
    if (!std::isfinite(shrink) || shrink <= 0.0f || std::abs(shrink - 1.0f) < 1.0e-5f) return hud;
    // The HUD is drawn into the 512-wide world surface, which the composition
    // maps onto the 480-wide display.
    const std::uint32_t target = draw.framebuffer_address & 0x001FFFF0u;
    const bool world = (s.frame_has_scene || s.composited_last_frame) &&
        s.world_framebuffer_address != 0u && target == s.world_framebuffer_address;
    const std::uint32_t logical_width = world ? kWorldWidth : kWidth;
    hud.shrink = shrink;
    hud.display_scale_x = static_cast<float>(kWidth) / static_cast<float>(logical_width);
    hud.source_center = static_cast<float>(logical_width) * 0.5f;
    return hud;
}
void ge_gpu_backend_note_through_extent(const GeGpuDrawDescriptor &, float, float) noexcept {}
bool ge_gpu_backend_adopt_shared_texture(const GeGpuDrawDescriptor &) noexcept { return false; }
bool ge_gpu_backend_texture_available(const GeGpuDrawDescriptor &draw) noexcept {
    auto &s = state();
    if (ge_gpu_backend_is_framebuffer_feedback_texture(draw)) return true;
    if (!s.enabled || !draw.texture_enabled) return false;
    const auto found = s.textures.find(texture_key(draw));
    return found != s.textures.end() && found->second.image != VK_NULL_HANDLE;
}
bool ge_gpu_backend_upload_decoded_texture(const GeGpuDrawDescriptor &draw,
                                           std::uint32_t width, std::uint32_t height,
                                           std::span<const std::byte> rgba) noexcept {
    try {
        return ge_gpu_backend_upload_decoded_texture_chain_packed(
            draw, width, height, 1u, {rgba.begin(), rgba.end()});
    } catch (...) { return false; }
}
bool ge_gpu_backend_upload_decoded_texture_chain(const GeGpuDrawDescriptor &draw,
                                                 std::span<const GeGpuDecodedMipLevel> levels) noexcept {
    if (levels.empty()) return false;
    return ge_gpu_backend_upload_decoded_texture(draw, levels.front().width,
                                                 levels.front().height, levels.front().rgba8);
}
void wait_in_flight(VulkanPreview &s);  // defined with the frame submission below

bool ge_gpu_backend_upload_decoded_texture_chain_packed(const GeGpuDrawDescriptor &draw,
    std::uint32_t width, std::uint32_t height, std::uint32_t,
    std::vector<std::byte> rgba) noexcept {
    auto &s = state();
    if (!s.enabled || !draw.texture_enabled || width == 0u || height == 0u) return false;
    const std::uint64_t key = texture_key(draw);
    auto found = s.textures.find(key);
    // Same limits the DX12 backend reads (Rendering.TextureCacheEntries/MB).
    // A fixed 512 entries was fine for a quiet street, but a drive through
    // the city draws more distinct textures than that in ONE frame: nothing
    // could be evicted, uploads were refused, and those draws fell back to
    // flat colour - the radar and HUD blinking in as solid blocks.
    const std::uint64_t kTextureBudget = static_cast<std::uint64_t>(
        vcs_configuration().rendering.texture_cache_mb) * 1024ull * 1024ull;
    const std::size_t kTextureEntries = std::min<std::size_t>(
        vcs_configuration().rendering.texture_cache_entries, s.texture_capacity);
    const std::uint64_t base_bytes = static_cast<std::uint64_t>(width) * height * 4u;
    if (base_bytes > kTextureBudget) return false;
    const std::uint64_t replaced_bytes = found == s.textures.end() ? 0u : found->second.bytes;
    while ((found == s.textures.end() && s.textures.size() >= kTextureEntries) ||
           s.texture_bytes - replaced_bytes + base_bytes > kTextureBudget) {
        auto victim = s.textures.end();
        for (auto it = s.textures.begin(); it != s.textures.end(); ++it) {
            if (it->first == key) continue;
            if (it->second.checked_frame >= s.report.game_frames) continue;
            if (victim == s.textures.end() ||
                it->second.checked_frame < victim->second.checked_frame) victim = it;
        }
        if (victim == s.textures.end()) {
            static unsigned logged_refusals = 0u;
            if (logged_refusals < 20u) {
                ++logged_refusals;
                __android_log_print(ANDROID_LOG_WARN, "VCSVulkan",
                    "texture upload refused: cache full of this frame's textures (%zu entries, %llu bytes)",
                    s.textures.size(), static_cast<unsigned long long>(s.texture_bytes));
            }
            return false;
        }
        s.texture_bytes -= victim->second.bytes;
        wait_in_flight(s);  // the frame in flight may still sample it
        destroy_texture(s, victim->second);
        s.textures.erase(victim);
        ++s.report.evicted_textures;
    }
    Texture texture{};
    try {
        if (!create_texture(s, width, height, rgba, texture, texture_sampler_for(s, draw))) {
            static unsigned logged_failures = 0u;
            if (logged_failures < 20u) {
                ++logged_failures;
                __android_log_print(ANDROID_LOG_WARN, "VCSVulkan",
                    "texture create failed %ux%u fmt=%u cached=%zu/%u bytes=%llu",
                    width, height, draw.texture_format, s.textures.size(), s.texture_capacity,
                    static_cast<unsigned long long>(s.texture_bytes));
            }
            destroy_texture(s, texture);
            return false;
        }
        texture.signature = draw.texture_content_signature;
        texture.checked_frame = s.report.game_frames;
        s.last_texture_rgba.assign(rgba.begin(),
                                   rgba.begin() + static_cast<std::size_t>(base_bytes));
        if (found == s.textures.end()) {
            s.textures.emplace(key, texture);
            s.texture_bytes += base_bytes;
            ++s.report.unique_texture_keys;
            ++s.report.unique_texture_image_keys;
        } else {
            s.texture_bytes -= found->second.bytes;
            wait_in_flight(s);
            destroy_texture(s, found->second);
            found->second = texture;
            s.texture_bytes += base_bytes;
        }
        ++s.report.decoded_texture_uploads;
        s.report.decoded_texture_bytes += base_bytes;
        ++s.report.texture_images_created;
        ++s.report.texture_image_uploads;
        s.report.texture_image_upload_bytes += base_bytes;
        return true;
    } catch (...) {
        destroy_texture(s, texture);
        return false;
    }
}
bool ge_gpu_backend_copy_last_texture_rgba(std::span<std::byte> destination) noexcept {
    const auto &rgba = state().last_texture_rgba;
    if (rgba.empty() || destination.size() < rgba.size()) return false;
    std::memcpy(destination.data(), rgba.data(), rgba.size());
    return true;
}

// Measured extents of the world surface and of the composition that samples it,
// reset every time they are logged. The world target is still a fixed 480x272,
// and these numbers decide what size it has to be instead.
struct ExtentDiagnostics {
    float world_max_x{}, world_max_y{};
    std::int32_t world_scissor_x1{}, world_scissor_y1{};
    std::uint32_t world_stride{};
    std::uint32_t feedback_texture_width{}, feedback_texture_height{};
    float feedback_max_u{}, feedback_max_v{};
    float feedback_max_x{}, feedback_max_y{};
};
ExtentDiagnostics g_extent_diag;

void ge_gpu_backend_accumulate_color_triangles(const GeGpuDrawDescriptor &draw,
                                                std::span<const GeGpuVertex> triangles) noexcept {
    VulkanPreview &s = state();
    if (!s.enabled || triangles.empty()) return;
    {
        ExtentDiagnostics &d = g_extent_diag;
        const std::uint32_t target = draw.framebuffer_address & 0x001FFFF0u;
        if (target == s.world_framebuffer_address && !draw.through && !draw.clear_mode) {
            for (const GeGpuVertex &v : triangles) {
                d.world_max_x = std::max(d.world_max_x, v.x);
                d.world_max_y = std::max(d.world_max_y, v.y);
            }
            d.world_scissor_x1 = std::max(d.world_scissor_x1, draw.scissor_x1);
            d.world_scissor_y1 = std::max(d.world_scissor_y1, draw.scissor_y1);
            d.world_stride = std::max(d.world_stride, draw.framebuffer_stride);
        }
        if (draw.texture_enabled && ge_gpu_backend_is_framebuffer_feedback_texture(draw)) {
            d.feedback_texture_width = std::max(d.feedback_texture_width, draw.texture_width);
            d.feedback_texture_height = std::max(d.feedback_texture_height, draw.texture_height);
            for (const GeGpuVertex &v : triangles) {
                d.feedback_max_u = std::max(d.feedback_max_u, v.u);
                d.feedback_max_v = std::max(d.feedback_max_v, v.v);
                d.feedback_max_x = std::max(d.feedback_max_x, v.x);
                d.feedback_max_y = std::max(d.feedback_max_y, v.y);
            }
        }
    }
    const std::size_t max_vertices = static_cast<std::size_t>(kVertexCapacity / sizeof(GeGpuVertex));
    if (triangles.size() > max_vertices ||
        s.vertices.size() > max_vertices - triangles.size()) return;
    try {
        const auto first = static_cast<std::uint32_t>(s.vertices.size());
        const std::uint64_t key = draw.texture_enabled ? texture_key(draw) : 0u;
        const bool feedback = ge_gpu_backend_is_framebuffer_feedback_texture(draw);
        const bool textured = feedback || (draw.texture_enabled && s.textures.contains(key));
        s.vertices.insert(s.vertices.end(), triangles.begin(), triangles.end());
        if (textured && draw.texture_width != 0u && draw.texture_height != 0u) {
            for (std::size_t index = first; index < s.vertices.size(); ++index) {
                s.vertices[index].u /= static_cast<float>(feedback ? kWorldWidth : draw.texture_width);
                s.vertices[index].v /= static_cast<float>(feedback ? kWorldHeight : draw.texture_height);
            }
        }
        s.batches.push_back({draw, first, static_cast<std::uint32_t>(triangles.size()),
                             key, textured, feedback});
        ++s.report.game_draw_calls;
        s.report.game_vertices += triangles.size();
        s.report.game_triangles += triangles.size() / 3u;
    } catch (...) {
        s.vertices.clear();
        s.batches.clear();
    }
}
void ge_gpu_backend_accumulate_hardware_triangles(const GeGpuDrawDescriptor &,
    const GeGpuHardwareTransform &, std::span<const GeGpuVertex>,
    std::span<const std::uint32_t>) noexcept {}
bool ge_gpu_backend_accumulate_hardware_packed_0115(const GeGpuDrawDescriptor &,
    const GeGpuHardwareTransform &, std::span<const std::byte>, std::uint32_t,
    std::span<const std::uint32_t>) noexcept { return false; }
void ge_gpu_backend_set_native_window(void *) noexcept {}
void ge_gpu_backend_set_display_framebuffer(std::uint32_t address) noexcept {
    auto &s = state();
    const std::uint32_t masked = address & 0x001FFFF0u;
    if (masked != s.display_framebuffer) {
        static unsigned logged_switches = 0u;
        if (logged_switches < 60u) {
            ++logged_switches;
            __android_log_print(ANDROID_LOG_INFO, "VCSVulkan", "display framebuffer %05x -> %05x (world %05x)",
                                s.display_framebuffer, masked, s.world_framebuffer_address);
        }
    }
    s.display_framebuffer = masked;
}

// Frame-time breakdown, logged every 120 vblanks with the extent diagnostics.
struct FrameTiming {
    std::uint64_t finish_ns{}, wait_ns{}, copy_ns{}, frames{};
};
FrameTiming g_frame_timing;
std::uint64_t steady_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Blocks until the frame in flight (if any) has finished on the GPU. Needed
// before anything it may still be reading is destroyed or overwritten.
void wait_in_flight(VulkanPreview &s) {
    if (!s.frame_in_flight || s.fence_waited) return;
    const std::uint64_t wait_start = steady_ns();
    const VkResult result = vkWaitForFences(s.device, 1u, &s.fence, VK_TRUE, 2'000'000'000ull);
    g_frame_timing.wait_ns += steady_ns() - wait_start;
    if (result != VK_SUCCESS) log_error("vkWaitForFences", result);
    s.fence_waited = true;
}

// Collects the frame in flight: frees the staging buffers its uploads used and
// copies its readback out. Returns true when a finished frame is now available.
bool collect_in_flight(VulkanPreview &s) {
    if (!s.frame_in_flight) return false;
    wait_in_flight(s);
    for (auto &[key, texture] : s.textures) {
        if (!texture.pending || !texture.recorded) continue;
        destroy_buffer(s, texture.staging);
        texture.pending = false;
        texture.recorded = false;
    }
    const std::uint64_t copy_start = steady_ns();
    std::memcpy(s.frame_rgba.data(), s.readback.mapped, s.frame_rgba.size());
    g_frame_timing.copy_ns += steady_ns() - copy_start;
    ++s.report.game_frames;
    s.report.game_frame_readback_bytes = s.frame_rgba.size();
    s.report.game_frame_vblank = s.in_flight_vblank;
    s.frame_in_flight = false;
    return true;
}

bool submit_color_frame(std::uint64_t vblank) noexcept;

bool ge_gpu_backend_finish_color_frame(std::uint64_t vblank) noexcept {
    VulkanPreview &s = state();
    // The previous frame has had a whole vblank of emulation to finish on the
    // GPU; collect it now, then submit this one without waiting for it.
    const bool frame_ready = s.enabled && collect_in_flight(s);
    (void)submit_color_frame(vblank);
    return frame_ready;
}

bool submit_color_frame(std::uint64_t vblank) noexcept {
    const std::uint64_t finish_start = steady_ns();
    struct FinishTimer {
        std::uint64_t start;
        ~FinishTimer() { g_frame_timing.finish_ns += steady_ns() - start; ++g_frame_timing.frames; }
    } finish_timer{finish_start};
    VulkanPreview &s = state();
    s.report.software_fallback_frame = s.software_menu_active;
    if (!s.enabled || s.batches.empty()) {
        s.frame_has_scene = false;
        return false;
    }
    const bool has_scene_draws = std::any_of(s.batches.begin(), s.batches.end(),
        [](const DrawBatch &batch) {
            return !batch.draw.through && !batch.draw.clear_mode;
        });
    if (s.authoritative && vblank % 120u == 0u) {
        struct TargetCount { std::uint32_t address{}; unsigned world{}; unsigned through{}; };
        std::array<TargetCount, 4> targets{};
        unsigned world_textured = 0u;
        unsigned world_missing_texture = 0u;
        for (const DrawBatch &batch : s.batches) {
            const auto address = batch.draw.framebuffer_address & 0x001FFFF0u;
            if (address == s.world_framebuffer_address && !batch.draw.through &&
                batch.draw.texture_enabled) {
                if (batch.textured) ++world_textured;
                else ++world_missing_texture;
            }
            auto it = std::find_if(targets.begin(), targets.end(),
                [address](const TargetCount &target) {
                    return target.address == address;
                });
            if (it == targets.end()) {
                it = std::find_if(targets.begin(), targets.end(),
                    [](const TargetCount &target) { return target.address == 0u; });
                if (it == targets.end()) continue;
                it->address = address;
            }
            if (batch.draw.through) ++it->through;
            else ++it->world;
        }
        std::array<std::uint32_t, 4> display_textures{};
        unsigned display_texture_count = 0u;
        for (const DrawBatch &batch : s.batches) {
            if ((batch.draw.framebuffer_address & 0x001FFFF0u) != s.display_framebuffer ||
                !batch.draw.texture_enabled) continue;
            const std::uint32_t texture = batch.draw.texture_address & 0x001FFFF0u;
            if (std::find(display_textures.begin(), display_textures.end(), texture) !=
                display_textures.end()) continue;
            if (display_texture_count < display_textures.size())
                display_textures[display_texture_count++] = texture;
        }
        __android_log_print(ANDROID_LOG_INFO, "VCSVulkan",
            "vblank=%llu display=%05x targets=%05x:%u/%u,%05x:%u/%u,%05x:%u/%u,%05x:%u/%u textures=%05x,%05x,%05x,%05x world_textured=%u world_missing=%u uploads=%llu hits=%llu menu=%d",
            static_cast<unsigned long long>(vblank), s.display_framebuffer,
            targets[0].address, targets[0].world, targets[0].through,
            targets[1].address, targets[1].world, targets[1].through,
            targets[2].address, targets[2].world, targets[2].through,
            targets[3].address, targets[3].world, targets[3].through,
            display_textures[0], display_textures[1], display_textures[2],
            display_textures[3],
            world_textured, world_missing_texture,
            static_cast<unsigned long long>(s.report.decoded_texture_uploads),
            static_cast<unsigned long long>(s.report.texture_cache_hits),
            s.software_menu_active ? 1 : 0);
        const ExtentDiagnostics &d = g_extent_diag;
        __android_log_print(ANDROID_LOG_INFO, "VCSVulkan",
            "extent world=%05x max_xy=%.1f,%.1f scissor=%d,%d stride=%u | feedback tex=%ux%u max_uv=%.1f,%.1f max_xy=%.1f,%.1f",
            s.world_framebuffer_address, d.world_max_x, d.world_max_y,
            d.world_scissor_x1, d.world_scissor_y1, d.world_stride,
            d.feedback_texture_width, d.feedback_texture_height,
            d.feedback_max_u, d.feedback_max_v, d.feedback_max_x, d.feedback_max_y);
        g_extent_diag = {};
        const FrameTiming &t = g_frame_timing;
        if (t.frames != 0u)
            __android_log_print(ANDROID_LOG_INFO, "VCSVulkan",
                "timing per GPU frame: finish=%.2fms (gpu wait=%.2fms, readback copy=%.2fms) over %llu frames",
                t.finish_ns / 1e6 / t.frames, t.wait_ns / 1e6 / t.frames, t.copy_ns / 1e6 / t.frames,
                static_cast<unsigned long long>(t.frames));
        g_frame_timing = {};
    }
    std::vector<GeGpuVertex> selected;
    std::vector<DrawBatch> world_batches;
    std::vector<DrawBatch> display_batches;
    try {
        const auto select_target = [&](std::uint32_t address,
                                       std::vector<DrawBatch> &output) {
            for (const DrawBatch &batch : s.batches) {
                if ((batch.draw.framebuffer_address & 0x001FFFF0u) != address)
                    continue;
                const auto first = static_cast<std::uint32_t>(selected.size());
                selected.insert(selected.end(), s.vertices.begin() + batch.first,
                                s.vertices.begin() + batch.first + batch.count);
                output.push_back({batch.draw, first, batch.count,
                                  batch.texture_key, batch.textured,
                                  batch.framebuffer_feedback});
            }
        };
        const bool display_has_draws = std::any_of(s.batches.begin(), s.batches.end(),
            [&](const DrawBatch &batch) {
                return (batch.draw.framebuffer_address & 0x001FFFF0u) == s.display_framebuffer;
            });
        if (has_scene_draws || display_has_draws) {
            // Gameplay, and the 2D screens built the same way - save prompts,
            // "continue game", mission titles: drawn into the world surface and
            // composited into the display. Both passes, or the interface drawn
            // into the world surface is lost and the screen stays black while
            // the game waits for an answer to a prompt nobody can see.
            if (s.authoritative && s.world_framebuffer_address != 0u &&
                s.world_framebuffer_address != s.display_framebuffer)
                select_target(s.world_framebuffer_address, world_batches);
            select_target(s.display_framebuffer, display_batches);
        } else {
            // Nothing drawn into the displayed surface: a menu double-buffering
            // between two surfaces every vblank, so the one being displayed is
            // not the one just drawn. Present whichever received the draws.
            std::uint32_t best = s.display_framebuffer;
            std::size_t best_count = 0u;
            for (const DrawBatch &candidate : s.batches) {
                const std::uint32_t address = candidate.draw.framebuffer_address & 0x001FFFF0u;
                std::size_t count = 0u;
                for (const DrawBatch &other : s.batches)
                    if ((other.draw.framebuffer_address & 0x001FFFF0u) == address) ++count;
                if (count > best_count) { best = address; best_count = count; }
            }
            select_target(best, display_batches);
        }
    } catch (...) {
        s.vertices.clear();
        s.batches.clear();
        return false;
    }
    s.vertices.clear();
    s.batches.clear();
    s.frame_has_scene = false;
    s.composited_last_frame = !world_batches.empty();
    if (selected.empty() || display_batches.empty() ||
        selected.size() * sizeof(GeGpuVertex) > kVertexCapacity) return false;
    // Menus are rendered here too. They used to be handed back to the software
    // rasterizer after two 2D-only frames, but the CPU raster of the surface
    // the menu draws into is skipped while the GPU owns it - so the picture
    // switched between a GPU frame and a half-painted guest framebuffer
    // (garbled menu text, unstable pause menu), and at 480x272 stretched to
    // the panel. Drawing them on the GPU keeps one source, in HD, with the
    // same widescreen HUD correction as gameplay.
    (void)has_scene_draws;
    s.consecutive_2d_frames = 0u;
    s.software_menu_active = false;
    s.report.software_fallback_frame = false;
    std::memcpy(s.vertices_gpu.mapped, selected.data(), selected.size() * sizeof(GeGpuVertex));
    VkResult result = vkResetCommandPool(s.device, s.command_pool, 0u);
    if (result != VK_SUCCESS) { log_error("vkResetCommandPool", result); return false; }
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = vkBeginCommandBuffer(s.command, &begin);
    if (result != VK_SUCCESS) { log_error("vkBeginCommandBuffer", result); return false; }
    record_texture_uploads(s);
    std::array<VkClearValue, 2> clear{};
    // VCS uses reversed PSP depth (GE_GREATER/GE_GEQUAL). Match the software
    // framebuffer and the established DX12 path: far depth clears to zero.
    clear[1].depthStencil.depth = 0.0f;
    VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass.renderPass = s.render_pass;
    pass.framebuffer = s.framebuffer;
    pass.renderArea = {{0, 0}, {s.display_width, s.display_height}};
    pass.clearValueCount = static_cast<std::uint32_t>(clear.size());
    pass.pClearValues = clear.data();
    const VkBuffer vertex_buffer = s.vertices_gpu.handle;
    const VkDeviceSize offset = 0u;
    vkCmdBindVertexBuffers(s.command, 0u, 1u, &vertex_buffer, &offset);
    // Each pass maps PSP screen coordinates onto its own surface: the world at
    // 512x320, the display at 480x272. Viewport, vertex scale and scissor limits
    // all follow the pass.
    const auto record_batches = [&](const std::vector<DrawBatch> &batches,
                                    std::uint32_t width, std::uint32_t height,
                                    std::uint32_t physical_width,
                                    std::uint32_t physical_height) {
    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(physical_width),
                              static_cast<float>(physical_height), 0.0f, 1.0f};
    vkCmdSetViewport(s.command, 0u, 1u, &viewport);
    const std::array<float, 4> scale{
        2.0f / static_cast<float>(width), 2.0f / static_cast<float>(height), -1.0f, -1.0f};
    vkCmdPushConstants(s.command, s.pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT,
                       0u, sizeof(scale), scale.data());
    for (const DrawBatch &batch : batches) {
        const std::array<std::uint32_t, 4> alpha{
            batch.draw.alpha_test_enabled ? 1u : 0u,
            batch.draw.alpha_function, batch.draw.alpha_reference,
            batch.draw.alpha_mask};
        vkCmdPushConstants(s.command, s.pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                           48u, sizeof(alpha), alpha.data());
        const std::uint32_t fog_color = batch.draw.fog_color;
        const std::array<float, 4> fog{
            static_cast<float>(fog_color & 0xFFu) / 255.0f,
            static_cast<float>((fog_color >> 8u) & 0xFFu) / 255.0f,
            static_cast<float>((fog_color >> 16u) & 0xFFu) / 255.0f,
            batch.draw.fog_enabled ? 1.0f : 0.0f};
        vkCmdPushConstants(s.command, s.pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                           64u, sizeof(fog), fog.data());
        if (batch.draw.fog_enabled) ++s.report.fogged_game_draw_calls;
        const auto texture = s.textures.find(batch.texture_key);
        const bool textured = batch.framebuffer_feedback ||
            (batch.textured && texture != s.textures.end() &&
             texture->second.descriptor != VK_NULL_HANDLE);
        // A textured draw whose image is not resident (refused or evicted since
        // it was queued) is skipped rather than drawn untextured: missing for a
        // frame is far less visible than a block of its flat vertex colour.
        if (batch.draw.texture_enabled && !textured) {
            ++s.report.missing_texture_draw_calls;
            static unsigned logged_missing = 0u;
            if (logged_missing < 40u) {
                ++logged_missing;
                __android_log_print(ANDROID_LOG_INFO, "VCSVulkan",
                    "skip untextured target=%05x tex=%08x fmt=%u %ux%u bw=%u clut=%08x through=%d queued_textured=%d cached=%d",
                    batch.draw.framebuffer_address & 0x001FFFF0u, batch.draw.texture_address,
                    batch.draw.texture_format, batch.draw.texture_width, batch.draw.texture_height,
                    batch.draw.texture_buffer_width, batch.draw.clut_address,
                    batch.draw.through ? 1 : 0, batch.textured ? 1 : 0,
                    texture != s.textures.end() ? 1 : 0);
            }
            continue;
        }
        const std::uint32_t pipeline_index = batch.draw.depth_test_enabled
            ? 2u + (batch.draw.depth_function & 7u) * 4u +
                (batch.draw.depth_write_enabled ? 2u : 0u) + (textured ? 1u : 0u)
            : (textured ? 1u : 0u);
        const std::uint32_t blend = blend_variant(batch.draw);
        vkCmdBindPipeline(s.command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          s.pipelines[pipeline_index + blend * 34u]);
        if (blend == 3u) {
            const std::uint32_t fix = batch.draw.blend_fix_source;
            const std::array<float, 4> constants{
                static_cast<float>(fix & 0xFFu) / 255.0f,
                static_cast<float>((fix >> 8u) & 0xFFu) / 255.0f,
                static_cast<float>((fix >> 16u) & 0xFFu) / 255.0f, 1.0f};
            vkCmdSetBlendConstants(s.command, constants.data());
        }
        if (batch.draw.depth_test_enabled) ++s.report.depth_tested_game_draw_calls;
        if (batch.draw.depth_write_enabled) ++s.report.depth_writing_game_draw_calls;
        if (textured) {
            const VkDescriptorSet descriptor = batch.framebuffer_feedback
                ? s.world_descriptor : texture->second.descriptor;
            vkCmdBindDescriptorSets(s.command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                s.pipeline_layout, 0u, 1u, &descriptor, 0u, nullptr);
            const std::array<std::uint32_t, 4> control{
                batch.draw.texture_function, batch.draw.texture_use_alpha ? 1u : 0u,
                batch.draw.texture_double_color ? 1u : 0u, 0u};
            const std::uint32_t env = batch.draw.texture_env;
            const std::array<float, 4> environment{
                static_cast<float>(env & 0xFFu) / 255.0f,
                static_cast<float>((env >> 8u) & 0xFFu) / 255.0f,
                static_cast<float>((env >> 16u) & 0xFFu) / 255.0f, 1.0f};
            vkCmdPushConstants(s.command, s.pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                               16u, sizeof(control), control.data());
            vkCmdPushConstants(s.command, s.pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                               32u, sizeof(environment), environment.data());
        }
        // Scissor arrives in PSP pixels; scale it onto the physical target.
        const auto sx = [&](std::int32_t value) {
            return static_cast<std::int32_t>(std::clamp<std::int64_t>(
                static_cast<std::int64_t>(value) * physical_width / width, 0, physical_width));
        };
        const auto sy = [&](std::int32_t value) {
            return static_cast<std::int32_t>(std::clamp<std::int64_t>(
                static_cast<std::int64_t>(value) * physical_height / height, 0, physical_height));
        };
        const std::int32_t x0 = sx(batch.draw.scissor_x0);
        const std::int32_t y0 = sy(batch.draw.scissor_y0);
        const std::int32_t x1 = sx(batch.draw.scissor_x1 + 1);
        const std::int32_t y1 = sy(batch.draw.scissor_y1 + 1);
        if (x1 <= x0 || y1 <= y0) continue;
        VkRect2D scissor{{x0, y0},
                         {static_cast<std::uint32_t>(x1 - x0),
                          static_cast<std::uint32_t>(y1 - y0)}};
        vkCmdSetScissor(s.command, 0u, 1u, &scissor);
        vkCmdDraw(s.command, batch.count, 1u, batch.first, 0u);
    }
    };
    if (!world_batches.empty()) {
        pass.framebuffer = s.world_framebuffer;
        pass.renderArea = {{0, 0}, {s.world_width, s.world_height}};
        vkCmdBeginRenderPass(s.command, &pass, VK_SUBPASS_CONTENTS_INLINE);
        record_batches(world_batches, kWorldWidth, kWorldHeight, s.world_width, s.world_height);
        vkCmdEndRenderPass(s.command);
        VkImageMemoryBarrier world_barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        world_barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        world_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        world_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        world_barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        world_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        world_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        world_barrier.image = s.world_color;
        world_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        world_barrier.subresourceRange.levelCount = 1u;
        world_barrier.subresourceRange.layerCount = 1u;
        vkCmdPipelineBarrier(s.command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr,
            1u, &world_barrier);
    }
    pass.framebuffer = s.framebuffer;
    pass.renderArea = {{0, 0}, {s.display_width, s.display_height}};
    vkCmdBeginRenderPass(s.command, &pass, VK_SUBPASS_CONTENTS_INLINE);
    record_batches(display_batches, kWidth, kHeight, s.display_width, s.display_height);
    vkCmdEndRenderPass(s.command);
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = s.color;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1u;
    barrier.subresourceRange.layerCount = 1u;
    vkCmdPipelineBarrier(s.command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &barrier);
    VkBufferImageCopy copy{};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1u;
    copy.imageExtent = {s.display_width, s.display_height, 1u};
    vkCmdCopyImageToBuffer(s.command, s.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           s.readback.handle, 1u, &copy);
    result = vkEndCommandBuffer(s.command);
    if (result != VK_SUCCESS) { log_error("vkEndCommandBuffer", result); return false; }
    result = vkResetFences(s.device, 1u, &s.fence);
    if (result != VK_SUCCESS) { log_error("vkResetFences", result); return false; }
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1u;
    submit.pCommandBuffers = &s.command;
    result = vkQueueSubmit(s.queue, 1u, &submit, s.fence);
    if (result != VK_SUCCESS) { log_error("vkQueueSubmit", result); return false; }
    s.frame_in_flight = true;
    s.fence_waited = false;
    s.in_flight_vblank = vblank;
    // VulkanPreview is an explicit visual-inspection mode. The software GE is
    // still updating PSP memory, so a failed or missing GPU frame can fall
    // back to that reference image without losing game state.
    return true;
}

bool ge_gpu_backend_copy_game_frame_rgba(std::span<std::byte> destination) noexcept {
    const auto &frame = state().frame_rgba;
    if (frame.empty() || destination.size() < frame.size()) return false;
    std::memcpy(destination.data(), frame.data(), frame.size());
    return true;
}
bool ge_gpu_backend_presents_directly() noexcept { return false; }
std::uint32_t ge_gpu_backend_owned_framebuffer() noexcept {
    const auto &s = state();
    return s.enabled && s.authoritative && !s.software_menu_active
        ? s.world_framebuffer_address : 0u;
}
std::uint32_t ge_gpu_backend_display_framebuffer() noexcept { return state().display_framebuffer; }
std::span<const std::byte> ge_gpu_backend_game_frame_rgba() noexcept {
    const auto &frame = state().frame_rgba;
    return {frame.data(), frame.size()};
}
bool ge_gpu_backend_copy_offscreen_rgba(std::span<std::byte>) noexcept { return false; }
void ge_gpu_backend_mark_window_presented() noexcept {
    state().report.gpu_frame_presented_to_window = true;
}
GeGpuBackendReport ge_gpu_backend_report() { return state().report; }
const char *ge_gpu_backend_name(GeGpuBackendKind kind) noexcept {
    switch (kind) {
    case GeGpuBackendKind::Software: return "software";
    case GeGpuBackendKind::DirectX12: return "directx12";
    case GeGpuBackendKind::Vulkan: return "vulkan";
    }
    return "unknown";
}

} // namespace vcs
