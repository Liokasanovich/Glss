#include "glss/vulkan_compute.h"

#include <cstring>
#include <iostream>

#include "glss/vulkan_context.h"

namespace glss {
namespace {

// Cast a runtime-resolved vkVoidFunction into a typed entry point. This is the
// standard Vulkan loading idiom; VK_NO_PROTOTYPES guarantees no linked symbols.
template <typename Fn>
Fn LoadDeviceFn(VulkanContext& ctx, const char* name) {
    return reinterpret_cast<Fn>(ctx.LoadDeviceProc(name));
}

// Push constant block shared with shaders/upscale.comp (6 * 4 = 24 bytes).
struct UpscalePushConstants {
    uint32_t in_w;
    uint32_t in_h;
    uint32_t out_w;
    uint32_t out_h;
    float sharpness;
    uint32_t mode;
};

constexpr uint32_t kLocalSizeX = 8;
constexpr uint32_t kLocalSizeY = 8;

} // namespace

VulkanCompute::VulkanCompute(VulkanContext& context) : context_(&context) {}

VulkanCompute::~VulkanCompute() { Shutdown(); }

bool VulkanCompute::ResolveEntryPoints() {
    create_shader_module_ = LoadDeviceFn<PFN_vkCreateShaderModule>(*context_, "vkCreateShaderModule");
    destroy_shader_module_ = LoadDeviceFn<PFN_vkDestroyShaderModule>(*context_, "vkDestroyShaderModule");
    create_desc_layout_ = LoadDeviceFn<PFN_vkCreateDescriptorSetLayout>(*context_, "vkCreateDescriptorSetLayout");
    destroy_desc_layout_ = LoadDeviceFn<PFN_vkDestroyDescriptorSetLayout>(*context_, "vkDestroyDescriptorSetLayout");
    create_pipeline_layout_ = LoadDeviceFn<PFN_vkCreatePipelineLayout>(*context_, "vkCreatePipelineLayout");
    destroy_pipeline_layout_ = LoadDeviceFn<PFN_vkDestroyPipelineLayout>(*context_, "vkDestroyPipelineLayout");
    create_compute_pipelines_ = LoadDeviceFn<PFN_vkCreateComputePipelines>(*context_, "vkCreateComputePipelines");
    destroy_pipeline_ = LoadDeviceFn<PFN_vkDestroyPipeline>(*context_, "vkDestroyPipeline");
    create_command_pool_ = LoadDeviceFn<PFN_vkCreateCommandPool>(*context_, "vkCreateCommandPool");
    destroy_command_pool_ = LoadDeviceFn<PFN_vkDestroyCommandPool>(*context_, "vkDestroyCommandPool");
    allocate_command_buffers_ = LoadDeviceFn<PFN_vkAllocateCommandBuffers>(*context_, "vkAllocateCommandBuffers");
    begin_command_buffer_ = LoadDeviceFn<PFN_vkBeginCommandBuffer>(*context_, "vkBeginCommandBuffer");
    end_command_buffer_ = LoadDeviceFn<PFN_vkEndCommandBuffer>(*context_, "vkEndCommandBuffer");
    reset_command_pool_ = LoadDeviceFn<PFN_vkResetCommandPool>(*context_, "vkResetCommandPool");
    cmd_bind_pipeline_ = LoadDeviceFn<PFN_vkCmdBindPipeline>(*context_, "vkCmdBindPipeline");
    cmd_bind_descriptor_sets_ = LoadDeviceFn<PFN_vkCmdBindDescriptorSets>(*context_, "vkCmdBindDescriptorSets");
    cmd_push_constants_ = LoadDeviceFn<PFN_vkCmdPushConstants>(*context_, "vkCmdPushConstants");
    cmd_dispatch_ = LoadDeviceFn<PFN_vkCmdDispatch>(*context_, "vkCmdDispatch");
    create_descriptor_pool_ = LoadDeviceFn<PFN_vkCreateDescriptorPool>(*context_, "vkCreateDescriptorPool");
    destroy_descriptor_pool_ = LoadDeviceFn<PFN_vkDestroyDescriptorPool>(*context_, "vkDestroyDescriptorPool");
    allocate_descriptor_sets_ = LoadDeviceFn<PFN_vkAllocateDescriptorSets>(*context_, "vkAllocateDescriptorSets");
    update_descriptor_sets_ = LoadDeviceFn<PFN_vkUpdateDescriptorSets>(*context_, "vkUpdateDescriptorSets");
    create_buffer_ = LoadDeviceFn<PFN_vkCreateBuffer>(*context_, "vkCreateBuffer");
    destroy_buffer_ = LoadDeviceFn<PFN_vkDestroyBuffer>(*context_, "vkDestroyBuffer");
    get_buffer_memory_requirements_ =
        LoadDeviceFn<PFN_vkGetBufferMemoryRequirements>(*context_, "vkGetBufferMemoryRequirements");
    allocate_memory_ = LoadDeviceFn<PFN_vkAllocateMemory>(*context_, "vkAllocateMemory");
    free_memory_ = LoadDeviceFn<PFN_vkFreeMemory>(*context_, "vkFreeMemory");
    bind_buffer_memory_ = LoadDeviceFn<PFN_vkBindBufferMemory>(*context_, "vkBindBufferMemory");
    map_memory_ = LoadDeviceFn<PFN_vkMapMemory>(*context_, "vkMapMemory");
    unmap_memory_ = LoadDeviceFn<PFN_vkUnmapMemory>(*context_, "vkUnmapMemory");
    queue_submit_ = LoadDeviceFn<PFN_vkQueueSubmit>(*context_, "vkQueueSubmit");
    queue_wait_idle_ = LoadDeviceFn<PFN_vkQueueWaitIdle>(*context_, "vkQueueWaitIdle");

    const bool ok = create_shader_module_ != nullptr && destroy_shader_module_ != nullptr &&
                    create_desc_layout_ != nullptr && destroy_desc_layout_ != nullptr &&
                    create_pipeline_layout_ != nullptr && destroy_pipeline_layout_ != nullptr &&
                    create_compute_pipelines_ != nullptr && destroy_pipeline_ != nullptr &&
                    create_command_pool_ != nullptr && destroy_command_pool_ != nullptr &&
                    allocate_command_buffers_ != nullptr && begin_command_buffer_ != nullptr &&
                    end_command_buffer_ != nullptr && reset_command_pool_ != nullptr &&
                    cmd_bind_pipeline_ != nullptr && cmd_bind_descriptor_sets_ != nullptr &&
                    cmd_push_constants_ != nullptr && cmd_dispatch_ != nullptr &&
                    create_descriptor_pool_ != nullptr && destroy_descriptor_pool_ != nullptr &&
                    allocate_descriptor_sets_ != nullptr && update_descriptor_sets_ != nullptr &&
                    create_buffer_ != nullptr && destroy_buffer_ != nullptr &&
                    get_buffer_memory_requirements_ != nullptr && allocate_memory_ != nullptr &&
                    free_memory_ != nullptr && bind_buffer_memory_ != nullptr && map_memory_ != nullptr &&
                    unmap_memory_ != nullptr && queue_submit_ != nullptr && queue_wait_idle_ != nullptr;
    if (!ok) {
        std::cerr << "[GLSS VulkanCompute] Failed to resolve one or more device entry points." << std::endl;
    }
    return ok;
}

bool VulkanCompute::CreatePipelineObjects(const uint32_t* spirv, size_t spirv_words) {
    VkDevice device = context_->GetDevice();

    VkDescriptorSetLayoutBinding bindings[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = 2;
    layout_info.pBindings = bindings;
    if (create_desc_layout_(device, &layout_info, nullptr, &desc_layout_) != VK_SUCCESS) {
        std::cerr << "[GLSS VulkanCompute] vkCreateDescriptorSetLayout failed." << std::endl;
        return false;
    }

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(UpscalePushConstants);

    VkPipelineLayoutCreateInfo pipeline_layout_info{};
    pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_info.setLayoutCount = 1;
    pipeline_layout_info.pSetLayouts = &desc_layout_;
    pipeline_layout_info.pushConstantRangeCount = 1;
    pipeline_layout_info.pPushConstantRanges = &push_range;
    if (create_pipeline_layout_(device, &pipeline_layout_info, nullptr, &pipeline_layout_) != VK_SUCCESS) {
        std::cerr << "[GLSS VulkanCompute] vkCreatePipelineLayout failed." << std::endl;
        return false;
    }

    VkShaderModuleCreateInfo module_info{};
    module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module_info.codeSize = spirv_words * sizeof(uint32_t);
    module_info.pCode = spirv;
    if (create_shader_module_(device, &module_info, nullptr, &shader_module_) != VK_SUCCESS) {
        std::cerr << "[GLSS VulkanCompute] vkCreateShaderModule failed." << std::endl;
        return false;
    }

    VkComputePipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline_info.stage.module = shader_module_;
    pipeline_info.stage.pName = "main";
    pipeline_info.layout = pipeline_layout_;
    if (create_compute_pipelines_(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline_) != VK_SUCCESS) {
        std::cerr << "[GLSS VulkanCompute] vkCreateComputePipelines failed." << std::endl;
        return false;
    }

    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = context_->GetComputeQueueFamily();
    if (create_command_pool_(device, &pool_info, nullptr, &command_pool_) != VK_SUCCESS) {
        std::cerr << "[GLSS VulkanCompute] vkCreateCommandPool failed." << std::endl;
        return false;
    }

    VkCommandBufferAllocateInfo cb_info{};
    cb_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cb_info.commandPool = command_pool_;
    cb_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb_info.commandBufferCount = 1;
    if (allocate_command_buffers_(device, &cb_info, &command_buffer_) != VK_SUCCESS) {
        std::cerr << "[GLSS VulkanCompute] vkAllocateCommandBuffers failed." << std::endl;
        return false;
    }

    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_size.descriptorCount = 2;
    VkDescriptorPoolCreateInfo desc_pool_info{};
    desc_pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    desc_pool_info.maxSets = 1;
    desc_pool_info.poolSizeCount = 1;
    desc_pool_info.pPoolSizes = &pool_size;
    if (create_descriptor_pool_(device, &desc_pool_info, nullptr, &desc_pool_) != VK_SUCCESS) {
        std::cerr << "[GLSS VulkanCompute] vkCreateDescriptorPool failed." << std::endl;
        return false;
    }

    VkDescriptorSetAllocateInfo set_info{};
    set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    set_info.descriptorPool = desc_pool_;
    set_info.descriptorSetCount = 1;
    set_info.pSetLayouts = &desc_layout_;
    if (allocate_descriptor_sets_(device, &set_info, &desc_set_) != VK_SUCCESS) {
        std::cerr << "[GLSS VulkanCompute] vkAllocateDescriptorSets failed." << std::endl;
        return false;
    }

    return true;
}

uint32_t VulkanCompute::FindMemoryType(uint32_t type_bits) const {
    constexpr VkMemoryPropertyFlags kRequired =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < memory_properties_.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) == 0) {
            continue;
        }
        if ((memory_properties_.memoryTypes[i].propertyFlags & kRequired) == kRequired) {
            return i;
        }
    }
    return UINT32_MAX;
}

bool VulkanCompute::CreateHostBuffer(VkDeviceSize size, HostBuffer& out) {
    VkDevice device = context_->GetDevice();

    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size;
    buffer_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (create_buffer_(device, &buffer_info, nullptr, &out.buffer) != VK_SUCCESS) {
        std::cerr << "[GLSS VulkanCompute] vkCreateBuffer failed." << std::endl;
        return false;
    }

    VkMemoryRequirements req{};
    get_buffer_memory_requirements_(device, out.buffer, &req);
    const uint32_t type_index = FindMemoryType(req.memoryTypeBits);
    if (type_index == UINT32_MAX) {
        std::cerr << "[GLSS VulkanCompute] No host-visible/coherent memory type for buffer." << std::endl;
        destroy_buffer_(device, out.buffer, nullptr);
        out.buffer = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = req.size;
    alloc_info.memoryTypeIndex = type_index;
    if (allocate_memory_(device, &alloc_info, nullptr, &out.memory) != VK_SUCCESS) {
        std::cerr << "[GLSS VulkanCompute] vkAllocateMemory failed." << std::endl;
        destroy_buffer_(device, out.buffer, nullptr);
        out.buffer = VK_NULL_HANDLE;
        return false;
    }

    if (bind_buffer_memory_(device, out.buffer, out.memory, 0) != VK_SUCCESS) {
        std::cerr << "[GLSS VulkanCompute] vkBindBufferMemory failed." << std::endl;
        free_memory_(device, out.memory, nullptr);
        destroy_buffer_(device, out.buffer, nullptr);
        out.memory = VK_NULL_HANDLE;
        out.buffer = VK_NULL_HANDLE;
        return false;
    }

    if (map_memory_(device, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped) != VK_SUCCESS) {
        std::cerr << "[GLSS VulkanCompute] vkMapMemory failed." << std::endl;
        free_memory_(device, out.memory, nullptr);
        destroy_buffer_(device, out.buffer, nullptr);
        out.memory = VK_NULL_HANDLE;
        out.buffer = VK_NULL_HANDLE;
        return false;
    }

    out.capacity = size;
    return true;
}

void VulkanCompute::DestroyHostBuffer(HostBuffer& buffer) {
    VkDevice device = context_->GetDevice();
    if (device == VK_NULL_HANDLE) {
        buffer = HostBuffer{};
        return;
    }
    if (buffer.mapped != nullptr && unmap_memory_ != nullptr) {
        unmap_memory_(device, buffer.memory);
        buffer.mapped = nullptr;
    }
    if (buffer.buffer != VK_NULL_HANDLE && destroy_buffer_ != nullptr) {
        destroy_buffer_(device, buffer.buffer, nullptr);
        buffer.buffer = VK_NULL_HANDLE;
    }
    if (buffer.memory != VK_NULL_HANDLE && free_memory_ != nullptr) {
        free_memory_(device, buffer.memory, nullptr);
        buffer.memory = VK_NULL_HANDLE;
    }
    buffer.capacity = 0;
}

bool VulkanCompute::EnsureBuffer(HostBuffer& buffer, VkDeviceSize needed) {
    if (buffer.buffer != VK_NULL_HANDLE && buffer.capacity >= needed) {
        return true;
    }
    DestroyHostBuffer(buffer);
    return CreateHostBuffer(needed, buffer);
}

bool VulkanCompute::WriteDescriptors() {
    VkDescriptorBufferInfo in_info{};
    in_info.buffer = in_buffer_.buffer;
    in_info.offset = 0;
    in_info.range = VK_WHOLE_SIZE;

    VkDescriptorBufferInfo out_info{};
    out_info.buffer = out_buffer_.buffer;
    out_info.offset = 0;
    out_info.range = VK_WHOLE_SIZE;

    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = desc_set_;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo = &in_info;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = desc_set_;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &out_info;

    update_descriptor_sets_(context_->GetDevice(), 2, writes, 0, nullptr);
    return true;
}

bool VulkanCompute::Initialize(const uint32_t* spirv, size_t spirv_words) {
    if (initialized_) {
        return true;
    }
    if (context_ == nullptr || !context_->IsAvailable()) {
        return false;
    }
    // Minimum valid SPIR-V module is 5 header words.
    if (spirv == nullptr || spirv_words < 5) {
        return false;
    }
    if (!ResolveEntryPoints()) {
        return false;
    }

    const auto get_memory_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        context_->LoadInstanceProc("vkGetPhysicalDeviceMemoryProperties"));
    if (get_memory_properties == nullptr) {
        std::cerr << "[GLSS VulkanCompute] Failed to resolve vkGetPhysicalDeviceMemoryProperties."
                  << std::endl;
        return false;
    }
    get_memory_properties(context_->GetPhysicalDevice(), &memory_properties_);

    if (!CreatePipelineObjects(spirv, spirv_words)) {
        Shutdown();
        return false;
    }

    initialized_ = true;
    return true;
}

bool VulkanCompute::Dispatch(const FrameBuffer& in, FrameBuffer& out, float sharpness, uint32_t mode) {
    if (!initialized_) {
        return false;
    }
    if (in.data.empty() || in.width == 0 || in.height == 0 || in.data.size() < in.ByteSize()) {
        return false;
    }
    if (out.width == 0 || out.height == 0) {
        return false;
    }

    const VkDeviceSize in_bytes = static_cast<VkDeviceSize>(in.ByteSize());
    const VkDeviceSize out_bytes = static_cast<VkDeviceSize>(out.ByteSize());
    if (in_bytes == 0 || out_bytes == 0) {
        return false;
    }

    if (!EnsureBuffer(in_buffer_, in_bytes) || !EnsureBuffer(out_buffer_, out_bytes)) {
        return false;
    }
    WriteDescriptors();

    std::memcpy(in_buffer_.mapped, in.data.data(), static_cast<size_t>(in_bytes));

    VkDevice device = context_->GetDevice();
    if (reset_command_pool_(device, command_pool_, 0) != VK_SUCCESS) {
        return false;
    }

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (begin_command_buffer_(command_buffer_, &begin_info) != VK_SUCCESS) {
        return false;
    }

    cmd_bind_pipeline_(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    cmd_bind_descriptor_sets_(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout_, 0, 1,
                              &desc_set_, 0, nullptr);

    UpscalePushConstants pc{};
    pc.in_w = in.width;
    pc.in_h = in.height;
    pc.out_w = out.width;
    pc.out_h = out.height;
    pc.sharpness = sharpness;
    pc.mode = mode;
    cmd_push_constants_(command_buffer_, pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);

    const uint32_t groups_x = (out.width + kLocalSizeX - 1) / kLocalSizeX;
    const uint32_t groups_y = (out.height + kLocalSizeY - 1) / kLocalSizeY;
    cmd_dispatch_(command_buffer_, groups_x, groups_y, 1);

    if (end_command_buffer_(command_buffer_) != VK_SUCCESS) {
        return false;
    }

    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &command_buffer_;
    if (queue_submit_(context_->GetQueue(), 1, &submit_info, VK_NULL_HANDLE) != VK_SUCCESS) {
        return false;
    }
    if (queue_wait_idle_(context_->GetQueue()) != VK_SUCCESS) {
        return false;
    }

    out.data.assign(static_cast<size_t>(out_bytes), 0);
    std::memcpy(out.data.data(), out_buffer_.mapped, static_cast<size_t>(out_bytes));
    return true;
}

void VulkanCompute::Shutdown() {
    if (context_ != nullptr) {
        VkDevice device = context_->GetDevice();
        if (device != VK_NULL_HANDLE) {
            DestroyHostBuffer(in_buffer_);
            DestroyHostBuffer(out_buffer_);
            if (desc_pool_ != VK_NULL_HANDLE && destroy_descriptor_pool_ != nullptr) {
                destroy_descriptor_pool_(device, desc_pool_, nullptr);
            }
            if (command_pool_ != VK_NULL_HANDLE && destroy_command_pool_ != nullptr) {
                destroy_command_pool_(device, command_pool_, nullptr);
            }
            if (pipeline_ != VK_NULL_HANDLE && destroy_pipeline_ != nullptr) {
                destroy_pipeline_(device, pipeline_, nullptr);
            }
            if (shader_module_ != VK_NULL_HANDLE && destroy_shader_module_ != nullptr) {
                destroy_shader_module_(device, shader_module_, nullptr);
            }
            if (pipeline_layout_ != VK_NULL_HANDLE && destroy_pipeline_layout_ != nullptr) {
                destroy_pipeline_layout_(device, pipeline_layout_, nullptr);
            }
            if (desc_layout_ != VK_NULL_HANDLE && destroy_desc_layout_ != nullptr) {
                destroy_desc_layout_(device, desc_layout_, nullptr);
            }
        }
    }

    desc_layout_ = VK_NULL_HANDLE;
    pipeline_layout_ = VK_NULL_HANDLE;
    shader_module_ = VK_NULL_HANDLE;
    pipeline_ = VK_NULL_HANDLE;
    command_pool_ = VK_NULL_HANDLE;
    command_buffer_ = VK_NULL_HANDLE;
    desc_pool_ = VK_NULL_HANDLE;
    desc_set_ = VK_NULL_HANDLE;
    in_buffer_ = HostBuffer{};
    out_buffer_ = HostBuffer{};
    initialized_ = false;
}

} // namespace glss
