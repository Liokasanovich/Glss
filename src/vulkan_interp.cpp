#include "glss/vulkan_interp.h"

#include <cmath>
#include <cstring>
#include <iostream>

#include "glss/vulkan_context.h"

namespace glss {
namespace {

template <typename Fn>
Fn LoadDeviceFn(VulkanContext& ctx, const char* name) {
    return reinterpret_cast<Fn>(ctx.LoadDeviceProc(name));
}

struct InterpPushConstants {
    uint32_t width;
    uint32_t height;
    float t;
    uint32_t mode;
    int32_t search_radius;
};

constexpr uint32_t kLocalSizeX = 8;
constexpr uint32_t kLocalSizeY = 8;

} // namespace

VulkanInterpCompute::VulkanInterpCompute(VulkanContext& context) : context_(&context) {}

VulkanInterpCompute::~VulkanInterpCompute() { Shutdown(); }

bool VulkanInterpCompute::ResolveEntryPoints() {
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
        std::cerr << "[GLSS VulkanInterp] Failed to resolve one or more device entry points." << std::endl;
    }
    return ok;
}

bool VulkanInterpCompute::CreatePipelineObjects(const uint32_t* spirv, size_t spirv_words) {
    VkDevice device = context_->GetDevice();

    VkDescriptorSetLayoutBinding bindings[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = 3;
    layout_info.pBindings = bindings;
    if (create_desc_layout_(device, &layout_info, nullptr, &desc_layout_) != VK_SUCCESS) {
        return false;
    }

    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcr.offset = 0;
    pcr.size = sizeof(InterpPushConstants);

    VkPipelineLayoutCreateInfo pipe_layout_info{};
    pipe_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipe_layout_info.setLayoutCount = 1;
    pipe_layout_info.pSetLayouts = &desc_layout_;
    pipe_layout_info.pushConstantRangeCount = 1;
    pipe_layout_info.pPushConstantRanges = &pcr;
    if (create_pipeline_layout_(device, &pipe_layout_info, nullptr, &pipeline_layout_) != VK_SUCCESS) {
        return false;
    }

    VkShaderModuleCreateInfo module_info{};
    module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module_info.codeSize = spirv_words * sizeof(uint32_t);
    module_info.pCode = spirv;
    if (create_shader_module_(device, &module_info, nullptr, &shader_module_) != VK_SUCCESS) {
        return false;
    }

    VkComputePipelineCreateInfo pipe_info{};
    pipe_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipe_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipe_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipe_info.stage.module = shader_module_;
    pipe_info.stage.pName = "main";
    pipe_info.layout = pipeline_layout_;

    if (create_compute_pipelines_(device, VK_NULL_HANDLE, 1, &pipe_info, nullptr, &pipeline_) != VK_SUCCESS) {
        return false;
    }

    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = context_->GetComputeQueueFamily();
    if (create_command_pool_(device, &pool_info, nullptr, &command_pool_) != VK_SUCCESS) {
        return false;
    }

    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = command_pool_;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = 1;
    if (allocate_command_buffers_(device, &alloc_info, &command_buffer_) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_size.descriptorCount = 3;

    VkDescriptorPoolCreateInfo desc_pool_info{};
    desc_pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    desc_pool_info.maxSets = 1;
    desc_pool_info.poolSizeCount = 1;
    desc_pool_info.pPoolSizes = &pool_size;
    if (create_descriptor_pool_(device, &desc_pool_info, nullptr, &desc_pool_) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorSetAllocateInfo ds_alloc{};
    ds_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ds_alloc.descriptorPool = desc_pool_;
    ds_alloc.descriptorSetCount = 1;
    ds_alloc.pSetLayouts = &desc_layout_;
    if (allocate_descriptor_sets_(device, &ds_alloc, &desc_set_) != VK_SUCCESS) {
        return false;
    }

    return true;
}

uint32_t VulkanInterpCompute::FindMemoryType(uint32_t type_bits) const {
    const VkMemoryPropertyFlags required =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < memory_properties_.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) &&
            (memory_properties_.memoryTypes[i].propertyFlags & required) == required) {
            return i;
        }
    }
    return UINT32_MAX;
}

bool VulkanInterpCompute::CreateHostBuffer(VkDeviceSize size, HostBuffer& out) {
    VkDevice device = context_->GetDevice();

    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (create_buffer_(device, &bi, nullptr, &out.buffer) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements reqs{};
    get_buffer_memory_requirements_(device, out.buffer, &reqs);

    const uint32_t mem_type = FindMemoryType(reqs.memoryTypeBits);
    if (mem_type == UINT32_MAX) {
        destroy_buffer_(device, out.buffer, nullptr);
        out.buffer = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = reqs.size;
    ai.memoryTypeIndex = mem_type;
    if (allocate_memory_(device, &ai, nullptr, &out.memory) != VK_SUCCESS) {
        destroy_buffer_(device, out.buffer, nullptr);
        out.buffer = VK_NULL_HANDLE;
        return false;
    }

    if (bind_buffer_memory_(device, out.buffer, out.memory, 0) != VK_SUCCESS) {
        DestroyHostBuffer(out);
        return false;
    }

    if (map_memory_(device, out.memory, 0, size, 0, &out.mapped) != VK_SUCCESS) {
        DestroyHostBuffer(out);
        return false;
    }

    out.capacity = size;
    return true;
}

void VulkanInterpCompute::DestroyHostBuffer(HostBuffer& buffer) {
    if (!context_ || context_->GetDevice() == VK_NULL_HANDLE) {
        buffer = HostBuffer{};
        return;
    }
    VkDevice device = context_->GetDevice();
    if (buffer.mapped && unmap_memory_) {
        unmap_memory_(device, buffer.memory);
        buffer.mapped = nullptr;
    }
    if (buffer.memory && free_memory_) {
        free_memory_(device, buffer.memory, nullptr);
        buffer.memory = VK_NULL_HANDLE;
    }
    if (buffer.buffer && destroy_buffer_) {
        destroy_buffer_(device, buffer.buffer, nullptr);
        buffer.buffer = VK_NULL_HANDLE;
    }
    buffer.capacity = 0;
}

bool VulkanInterpCompute::EnsureBuffer(HostBuffer& buffer, VkDeviceSize needed) {
    if (buffer.buffer != VK_NULL_HANDLE && buffer.capacity >= needed) {
        return true;
    }
    DestroyHostBuffer(buffer);
    VkDeviceSize alloc_size = needed;
    constexpr VkDeviceSize kMinAlloc = 4 * 1024 * 1024;
    if (alloc_size < kMinAlloc) {
        alloc_size = kMinAlloc;
    }
    return CreateHostBuffer(alloc_size, buffer);
}

bool VulkanInterpCompute::WriteDescriptors() {
    VkDescriptorBufferInfo dbi_prev{};
    dbi_prev.buffer = prev_buffer_.buffer;
    dbi_prev.offset = 0;
    dbi_prev.range = VK_WHOLE_SIZE;

    VkDescriptorBufferInfo dbi_curr{};
    dbi_curr.buffer = curr_buffer_.buffer;
    dbi_curr.offset = 0;
    dbi_curr.range = VK_WHOLE_SIZE;

    VkDescriptorBufferInfo dbi_out{};
    dbi_out.buffer = out_buffer_.buffer;
    dbi_out.offset = 0;
    dbi_out.range = VK_WHOLE_SIZE;

    VkWriteDescriptorSet writes[3]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = desc_set_;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo = &dbi_prev;

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = desc_set_;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &dbi_curr;

    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = desc_set_;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[2].pBufferInfo = &dbi_out;

    update_descriptor_sets_(context_->GetDevice(), 3, writes, 0, nullptr);
    return true;
}

bool VulkanInterpCompute::Initialize(const uint32_t* spirv, size_t spirv_words) {
    if (initialized_) {
        return true;
    }
    if (!context_ || !context_->IsAvailable()) {
        return false;
    }
    if (spirv == nullptr || spirv_words < 5) {
        return false;
    }
    if (!ResolveEntryPoints()) {
        return false;
    }

    const auto get_memory_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        context_->LoadInstanceProc("vkGetPhysicalDeviceMemoryProperties"));
    if (!get_memory_properties) {
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

bool VulkanInterpCompute::Dispatch(const FrameBuffer& prev, const FrameBuffer& curr, float t,
                                   FrameBuffer& out, uint32_t mode, int32_t search_radius) {
    if (!initialized_) {
        return false;
    }
    if (prev.width == 0 || prev.height == 0 || prev.data.size() < prev.ByteSize() ||
        curr.width != prev.width || curr.height != prev.height || curr.data.size() < curr.ByteSize()) {
        return false;
    }

    const VkDeviceSize in_bytes = prev.ByteSize();
    out.width = prev.width;
    out.height = prev.height;
    out.timestamp_ns = static_cast<uint64_t>(
        std::llround(static_cast<double>(prev.timestamp_ns) * (1.0 - t) +
                     static_cast<double>(curr.timestamp_ns) * t));
    const VkDeviceSize out_bytes = out.ByteSize();
    if (out.data.size() < out_bytes) {
        out.data.resize(out_bytes);
    }

    const bool buffers_recreated =
        (prev_buffer_.capacity < in_bytes) || (curr_buffer_.capacity < in_bytes) || (out_buffer_.capacity < out_bytes);

    if (!EnsureBuffer(prev_buffer_, in_bytes) || !EnsureBuffer(curr_buffer_, in_bytes) ||
        !EnsureBuffer(out_buffer_, out_bytes)) {
        return false;
    }

    if (buffers_recreated) {
        WriteDescriptors();
    }

    std::memcpy(prev_buffer_.mapped, prev.data.data(), in_bytes);
    std::memcpy(curr_buffer_.mapped, curr.data.data(), in_bytes);

    reset_command_pool_(context_->GetDevice(), command_pool_, 0);

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    begin_command_buffer_(command_buffer_, &begin_info);

    cmd_bind_pipeline_(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    cmd_bind_descriptor_sets_(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout_, 0,
                              1, &desc_set_, 0, nullptr);

    InterpPushConstants pc{};
    pc.width = prev.width;
    pc.height = prev.height;
    pc.t = t;
    pc.mode = mode;
    pc.search_radius = search_radius;
    cmd_push_constants_(command_buffer_, pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                        sizeof(pc), &pc);

    const uint32_t group_x = (prev.width + kLocalSizeX - 1) / kLocalSizeX;
    const uint32_t group_y = (prev.height + kLocalSizeY - 1) / kLocalSizeY;
    cmd_dispatch_(command_buffer_, group_x, group_y, 1);

    end_command_buffer_(command_buffer_);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_buffer_;

    if (queue_submit_(context_->GetQueue(), 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS) {
        return false;
    }
    if (queue_wait_idle_(context_->GetQueue()) != VK_SUCCESS) {
        return false;
    }

    std::memcpy(out.data.data(), out_buffer_.mapped, out_bytes);
    return true;
}

void VulkanInterpCompute::Shutdown() {
    if (!context_ || context_->GetDevice() == VK_NULL_HANDLE) {
        initialized_ = false;
        return;
    }
    VkDevice device = context_->GetDevice();

    DestroyHostBuffer(prev_buffer_);
    DestroyHostBuffer(curr_buffer_);
    DestroyHostBuffer(out_buffer_);

    if (desc_pool_ && destroy_descriptor_pool_) {
        destroy_descriptor_pool_(device, desc_pool_, nullptr);
        desc_pool_ = VK_NULL_HANDLE;
    }
    if (command_pool_ && destroy_command_pool_) {
        destroy_command_pool_(device, command_pool_, nullptr);
        command_pool_ = VK_NULL_HANDLE;
    }
    if (pipeline_ && destroy_pipeline_) {
        destroy_pipeline_(device, pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }
    if (pipeline_layout_ && destroy_pipeline_layout_) {
        destroy_pipeline_layout_(device, pipeline_layout_, nullptr);
        pipeline_layout_ = VK_NULL_HANDLE;
    }
    if (shader_module_ && destroy_shader_module_) {
        destroy_shader_module_(device, shader_module_, nullptr);
        shader_module_ = VK_NULL_HANDLE;
    }
    if (desc_layout_ && destroy_desc_layout_) {
        destroy_desc_layout_(device, desc_layout_, nullptr);
        desc_layout_ = VK_NULL_HANDLE;
    }

    initialized_ = false;
}

} // namespace glss
