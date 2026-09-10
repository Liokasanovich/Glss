#pragma once

// We dynamically resolve every Vulkan entry point at runtime (dlopen /
// LoadLibrary), so the loader is never a link-time dependency. Defining
// VK_NO_PROTOTYPES suppresses the inline prototypes from the vendored headers.
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

namespace glss {

struct VulkanDeviceInfo {
    uint32_t index = 0;
    std::string name;
    uint32_t api_version = 0;
    uint64_t vram_bytes = 0;
    bool is_discrete = false;
};

// Minimal, ownership-based Vulkan compute context.
//
// The context owns a VkInstance and a logical VkDevice with a single compute
// queue. All Vulkan functions are loaded at runtime from the system loader, so
// linking against libvulkan is not required on any platform.
class VulkanContext {
public:
    VulkanContext();
    ~VulkanContext();

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;
    VulkanContext(VulkanContext&&) = delete;
    VulkanContext& operator=(VulkanContext&&) = delete;

    // Creates the instance + logical device. Returns false cleanly (no crash)
    // when the loader is missing, no device exists, or no compute queue family
    // is available.
    bool Initialize(int preferred_device_index = 0);

    // Destroys device/instance and unloads the loader. Idempotent.
    void Shutdown();

    // True once a loader and a logical device are both ready.
    bool IsAvailable() const { return initialized_; }

    // Kept for source compatibility with earlier call sites.
    bool IsInitialized() const { return initialized_; }

    // Real enumeration: spins up a temporary instance, walks
    // vkEnumeratePhysicalDevices and fills name/api_version/vram/is_discrete.
    static std::vector<VulkanDeviceInfo> EnumerateDevices();

    VkInstance GetInstance() const { return instance_; }
    VkPhysicalDevice GetPhysicalDevice() const { return physical_device_; }
    VkDevice GetDevice() const { return device_; }
    VkQueue GetQueue() const { return compute_queue_; }
    uint32_t GetComputeQueueFamily() const { return compute_family_idx_; }

    // Runtime entry-point lookup for downstream backends (e.g. VulkanCompute).
    // These wrap the loader's vkGetDeviceProcAddr / vkGetInstanceProcAddr so no
    // Vulkan function ever becomes a link-time dependency.
    PFN_vkVoidFunction LoadDeviceProc(const char* name) const;
    PFN_vkVoidFunction LoadInstanceProc(const char* name) const;

private:
    bool initialized_ = false;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue compute_queue_ = VK_NULL_HANDLE;
    uint32_t compute_family_idx_ = 0;

    // Runtime-resolved teardown entry points + native loader handle.
    PFN_vkGetInstanceProcAddr get_instance_proc_addr_ = nullptr;
    PFN_vkGetDeviceProcAddr get_device_proc_addr_ = nullptr;
    PFN_vkDestroyDevice destroy_device_ = nullptr;
    PFN_vkDestroyInstance destroy_instance_ = nullptr;
    void* loader_handle_ = nullptr;
};

} // namespace glss
