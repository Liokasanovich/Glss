#pragma once

// Runtime-loaded Vulkan compute backend. Nothing here is linked against
// libvulkan: every entry point is fetched through VulkanContext, which itself
// resolves them with vkGetInstanceProcAddr / vkGetDeviceProcAddr.
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>

#include "glss/glss.h"

namespace glss {

class VulkanContext;

// Owns a single compute pipeline running the embedded upscale SPIR-V, plus the
// host-visible input/output buffers and the command/descriptor objects needed
// to dispatch it. Designed to be safe to call repeatedly with changing frame
// sizes; buffers grow on demand and never shrink.
class VulkanCompute {
public:
    explicit VulkanCompute(VulkanContext& context);
    ~VulkanCompute();

    VulkanCompute(const VulkanCompute&) = delete;
    VulkanCompute& operator=(const VulkanCompute&) = delete;
    VulkanCompute(VulkanCompute&&) = delete;
    VulkanCompute& operator=(VulkanCompute&&) = delete;

    // Builds all Vulkan objects from the given SPIR-V. Returns false cleanly if
    // the context is unavailable or any Vulkan call fails; the caller should
    // then stay on the CPU path.
    bool Initialize(const uint32_t* spirv, size_t spirv_words);

    // Uploads `in`, dispatches the shader to produce `out.width x out.height`,
    // waits for completion and downloads into `out.data`. `out.width`/`out.height`
    // must already be set by the caller. `mode` selects the upscale kernel inside
    // the shader (0 = bilinear + RCAS, 1 = edge-adaptive FSR + RCAS). Returns
    // false on any failure so the caller can fall back to the CPU implementation.
    bool Dispatch(const FrameBuffer& in, FrameBuffer& out, float sharpness, uint32_t mode);

    // Destroys every owned object. Idempotent.
    void Shutdown();

    bool IsReady() const { return initialized_; }

private:
    struct HostBuffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
        VkDeviceSize capacity = 0;
    };

    bool ResolveEntryPoints();
    bool CreatePipelineObjects(const uint32_t* spirv, size_t spirv_words);
    uint32_t FindMemoryType(uint32_t type_bits) const;
    bool CreateHostBuffer(VkDeviceSize size, HostBuffer& out);
    void DestroyHostBuffer(HostBuffer& buffer);
    bool EnsureBuffer(HostBuffer& buffer, VkDeviceSize needed);
    bool WriteDescriptors();

    VulkanContext* context_ = nullptr;
    bool initialized_ = false;

    VkPhysicalDeviceMemoryProperties memory_properties_{};

    VkDescriptorSetLayout desc_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkShaderModule shader_module_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;
    VkDescriptorPool desc_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet desc_set_ = VK_NULL_HANDLE;

    HostBuffer in_buffer_;
    HostBuffer out_buffer_;

    // Runtime-resolved device entry points.
    PFN_vkCreateShaderModule create_shader_module_ = nullptr;
    PFN_vkDestroyShaderModule destroy_shader_module_ = nullptr;
    PFN_vkCreateDescriptorSetLayout create_desc_layout_ = nullptr;
    PFN_vkDestroyDescriptorSetLayout destroy_desc_layout_ = nullptr;
    PFN_vkCreatePipelineLayout create_pipeline_layout_ = nullptr;
    PFN_vkDestroyPipelineLayout destroy_pipeline_layout_ = nullptr;
    PFN_vkCreateComputePipelines create_compute_pipelines_ = nullptr;
    PFN_vkDestroyPipeline destroy_pipeline_ = nullptr;
    PFN_vkCreateCommandPool create_command_pool_ = nullptr;
    PFN_vkDestroyCommandPool destroy_command_pool_ = nullptr;
    PFN_vkAllocateCommandBuffers allocate_command_buffers_ = nullptr;
    PFN_vkBeginCommandBuffer begin_command_buffer_ = nullptr;
    PFN_vkEndCommandBuffer end_command_buffer_ = nullptr;
    PFN_vkResetCommandPool reset_command_pool_ = nullptr;
    PFN_vkCmdBindPipeline cmd_bind_pipeline_ = nullptr;
    PFN_vkCmdBindDescriptorSets cmd_bind_descriptor_sets_ = nullptr;
    PFN_vkCmdPushConstants cmd_push_constants_ = nullptr;
    PFN_vkCmdDispatch cmd_dispatch_ = nullptr;
    PFN_vkCreateDescriptorPool create_descriptor_pool_ = nullptr;
    PFN_vkDestroyDescriptorPool destroy_descriptor_pool_ = nullptr;
    PFN_vkAllocateDescriptorSets allocate_descriptor_sets_ = nullptr;
    PFN_vkUpdateDescriptorSets update_descriptor_sets_ = nullptr;
    PFN_vkCreateBuffer create_buffer_ = nullptr;
    PFN_vkDestroyBuffer destroy_buffer_ = nullptr;
    PFN_vkGetBufferMemoryRequirements get_buffer_memory_requirements_ = nullptr;
    PFN_vkAllocateMemory allocate_memory_ = nullptr;
    PFN_vkFreeMemory free_memory_ = nullptr;
    PFN_vkBindBufferMemory bind_buffer_memory_ = nullptr;
    PFN_vkMapMemory map_memory_ = nullptr;
    PFN_vkUnmapMemory unmap_memory_ = nullptr;
    PFN_vkQueueSubmit queue_submit_ = nullptr;
    PFN_vkQueueWaitIdle queue_wait_idle_ = nullptr;
};

} // namespace glss
