#include "glss/vulkan_context.h"

#include <iostream>
#include <utility>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace glss {
namespace {

// ---------------------------------------------------------------------------
// Native dynamic loader abstraction
// ---------------------------------------------------------------------------
#ifdef _WIN32
using NativeLoader = HMODULE;
#else
using NativeLoader = void*;
#endif

NativeLoader OpenVulkanLoader() {
#ifdef _WIN32
    return LoadLibraryA("vulkan-1.dll");
#else
    NativeLoader handle = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        handle = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    }
    return handle;
#endif
}

void CloseVulkanLoader(NativeLoader loader) {
    if (loader == nullptr) {
        return;
    }
#ifdef _WIN32
    FreeLibrary(loader);
#else
    dlclose(loader);
#endif
}

void* LoadLoaderSymbol(NativeLoader loader, const char* name) {
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(loader, name));
#else
    return dlsym(loader, name);
#endif
}

// ---------------------------------------------------------------------------
// Runtime-resolved Vulkan entry points
// ---------------------------------------------------------------------------
struct VulkanApi {
    PFN_vkGetInstanceProcAddr get_instance_proc_addr = nullptr;
    PFN_vkGetDeviceProcAddr get_device_proc_addr = nullptr;
    PFN_vkCreateInstance create_instance = nullptr;
    PFN_vkEnumerateInstanceVersion enumerate_instance_version = nullptr;

    // Instance-level
    PFN_vkEnumeratePhysicalDevices enumerate_physical_devices = nullptr;
    PFN_vkGetPhysicalDeviceProperties get_physical_device_properties = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties get_physical_device_memory_properties = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_physical_device_queue_family_properties = nullptr;
    PFN_vkCreateDevice create_device = nullptr;
    PFN_vkDestroyInstance destroy_instance = nullptr;

    // Device-level
    PFN_vkGetDeviceQueue get_device_queue = nullptr;
    PFN_vkDestroyDevice destroy_device = nullptr;

    template <typename Fn>
    Fn Resolve(VkInstance instance, const char* name) const {
        return reinterpret_cast<Fn>(get_instance_proc_addr(instance, name));
    }

    template <typename Fn>
    Fn ResolveDevice(VkDevice device, const char* name) const {
        return reinterpret_cast<Fn>(get_device_proc_addr(device, name));
    }

    bool LoadGlobal() {
        create_instance = Resolve<PFN_vkCreateInstance>(VK_NULL_HANDLE, "vkCreateInstance");
        enumerate_instance_version =
            Resolve<PFN_vkEnumerateInstanceVersion>(VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
        return create_instance != nullptr;
    }

    bool LoadInstance(VkInstance instance) {
        get_device_proc_addr = Resolve<PFN_vkGetDeviceProcAddr>(instance, "vkGetDeviceProcAddr");
        enumerate_physical_devices = Resolve<PFN_vkEnumeratePhysicalDevices>(instance, "vkEnumeratePhysicalDevices");
        get_physical_device_properties =
            Resolve<PFN_vkGetPhysicalDeviceProperties>(instance, "vkGetPhysicalDeviceProperties");
        get_physical_device_memory_properties =
            Resolve<PFN_vkGetPhysicalDeviceMemoryProperties>(instance, "vkGetPhysicalDeviceMemoryProperties");
        get_physical_device_queue_family_properties = Resolve<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
            instance, "vkGetPhysicalDeviceQueueFamilyProperties");
        create_device = Resolve<PFN_vkCreateDevice>(instance, "vkCreateDevice");
        destroy_instance = Resolve<PFN_vkDestroyInstance>(instance, "vkDestroyInstance");

        return get_device_proc_addr != nullptr && enumerate_physical_devices != nullptr &&
               get_physical_device_properties != nullptr && get_physical_device_memory_properties != nullptr &&
               get_physical_device_queue_family_properties != nullptr && create_device != nullptr &&
               destroy_instance != nullptr;
    }

    void LoadDevice(VkDevice device) {
        get_device_queue = ResolveDevice<PFN_vkGetDeviceQueue>(device, "vkGetDeviceQueue");
        destroy_device = ResolveDevice<PFN_vkDestroyDevice>(device, "vkDestroyDevice");
    }
};

uint32_t SelectApiVersion(const VulkanApi& api) {
    uint32_t loader_version = VK_API_VERSION_1_0;
    if (api.enumerate_instance_version != nullptr) {
        uint32_t queried = 0;
        if (api.enumerate_instance_version(&queried) == VK_SUCCESS && queried != 0) {
            loader_version = queried;
        }
    }
    return loader_version < VK_API_VERSION_1_2 ? loader_version : VK_API_VERSION_1_2;
}

bool CreateInstance(const VulkanApi& api, VkInstance* out_instance) {
    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "GLSS";
    app_info.applicationVersion = VK_MAKE_API_VERSION(0, 2, 0, 0);
    app_info.pEngineName = "GLSS";
    app_info.engineVersion = VK_MAKE_API_VERSION(0, 2, 0, 0);
    app_info.apiVersion = SelectApiVersion(api);

    VkInstanceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &app_info;

    return api.create_instance(&create_info, nullptr, out_instance) == VK_SUCCESS &&
           *out_instance != VK_NULL_HANDLE;
}

uint64_t DeviceLocalVram(const VkPhysicalDeviceMemoryProperties& mem) {
    uint64_t total = 0;
    for (uint32_t i = 0; i < mem.memoryHeapCount; ++i) {
        if ((mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) {
            total += mem.memoryHeaps[i].size;
        }
    }
    return total;
}

} // namespace

// ---------------------------------------------------------------------------
// VulkanContext
// ---------------------------------------------------------------------------
VulkanContext::VulkanContext() = default;

VulkanContext::~VulkanContext() {
    Shutdown();
}

std::vector<VulkanDeviceInfo> VulkanContext::EnumerateDevices() {
    std::vector<VulkanDeviceInfo> devices;

    NativeLoader loader = OpenVulkanLoader();
    if (loader == nullptr) {
        return devices;
    }

    VulkanApi api{};
    api.get_instance_proc_addr =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(LoadLoaderSymbol(loader, "vkGetInstanceProcAddr"));
    if (api.get_instance_proc_addr == nullptr || !api.LoadGlobal()) {
        CloseVulkanLoader(loader);
        return devices;
    }

    VkInstance instance = VK_NULL_HANDLE;
    if (!CreateInstance(api, &instance) || !api.LoadInstance(instance)) {
        if (instance != VK_NULL_HANDLE && api.destroy_instance != nullptr) {
            api.destroy_instance(instance, nullptr);
        }
        CloseVulkanLoader(loader);
        return devices;
    }

    uint32_t count = 0;
    if (api.enumerate_physical_devices(instance, &count, nullptr) == VK_SUCCESS && count > 0) {
        std::vector<VkPhysicalDevice> physical(count);
        if (api.enumerate_physical_devices(instance, &count, physical.data()) == VK_SUCCESS) {
            devices.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
                VkPhysicalDeviceProperties props{};
                api.get_physical_device_properties(physical[i], &props);

                VkPhysicalDeviceMemoryProperties mem{};
                api.get_physical_device_memory_properties(physical[i], &mem);

                VulkanDeviceInfo info;
                info.index = i;
                info.name = props.deviceName;
                info.api_version = props.apiVersion;
                info.vram_bytes = DeviceLocalVram(mem);
                info.is_discrete = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
                devices.push_back(std::move(info));
            }
        }
    }

    api.destroy_instance(instance, nullptr);
    CloseVulkanLoader(loader);
    return devices;
}

bool VulkanContext::Initialize(int preferred_device_index) {
    if (initialized_) {
        return true;
    }

    // Reset any stale state before (re)initializing.
    Shutdown();

    NativeLoader loader = OpenVulkanLoader();
    if (loader == nullptr) {
        std::cerr << "[GLSS Vulkan] Vulkan loader not found (libvulkan.so.1 / vulkan-1.dll)." << std::endl;
        return false;
    }

    VulkanApi api{};
    api.get_instance_proc_addr =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(LoadLoaderSymbol(loader, "vkGetInstanceProcAddr"));
    if (api.get_instance_proc_addr == nullptr || !api.LoadGlobal()) {
        std::cerr << "[GLSS Vulkan] Failed to resolve vkGetInstanceProcAddr." << std::endl;
        CloseVulkanLoader(loader);
        return false;
    }

    VkInstance instance = VK_NULL_HANDLE;
    if (!CreateInstance(api, &instance)) {
        std::cerr << "[GLSS Vulkan] vkCreateInstance failed." << std::endl;
        CloseVulkanLoader(loader);
        return false;
    }

    if (!api.LoadInstance(instance)) {
        std::cerr << "[GLSS Vulkan] Failed to resolve instance-level entry points." << std::endl;
        api.destroy_instance(instance, nullptr);
        CloseVulkanLoader(loader);
        return false;
    }

    uint32_t device_count = 0;
    if (api.enumerate_physical_devices(instance, &device_count, nullptr) != VK_SUCCESS || device_count == 0) {
        std::cerr << "[GLSS Vulkan] No Vulkan physical device found." << std::endl;
        api.destroy_instance(instance, nullptr);
        CloseVulkanLoader(loader);
        return false;
    }

    std::vector<VkPhysicalDevice> physical(device_count);
    if (api.enumerate_physical_devices(instance, &device_count, physical.data()) != VK_SUCCESS) {
        std::cerr << "[GLSS Vulkan] Failed to enumerate physical devices." << std::endl;
        api.destroy_instance(instance, nullptr);
        CloseVulkanLoader(loader);
        return false;
    }

    // Honour the caller's preference when it is a valid index, otherwise fall
    // back to the first discrete GPU (then device 0).
    int selected = preferred_device_index;
    if (selected < 0 || selected >= static_cast<int>(device_count)) {
        selected = -1;
        for (uint32_t i = 0; i < device_count; ++i) {
            VkPhysicalDeviceProperties props{};
            api.get_physical_device_properties(physical[i], &props);
            if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                selected = static_cast<int>(i);
                break;
            }
        }
        if (selected < 0) {
            selected = 0;
        }
    }

    VkPhysicalDevice physical_device = physical[static_cast<size_t>(selected)];

    uint32_t family_count = 0;
    api.get_physical_device_queue_family_properties(physical_device, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    api.get_physical_device_queue_family_properties(physical_device, &family_count, families.data());

    int compute_family = -1;
    for (uint32_t i = 0; i < family_count; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0) {
            compute_family = static_cast<int>(i);
            break;
        }
    }
    if (compute_family < 0) {
        std::cerr << "[GLSS Vulkan] No queue family with compute support." << std::endl;
        api.destroy_instance(instance, nullptr);
        CloseVulkanLoader(loader);
        return false;
    }

    const float queue_priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = static_cast<uint32_t>(compute_family);
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &queue_priority;

    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;

    VkDevice device = VK_NULL_HANDLE;
    if (api.create_device(physical_device, &device_info, nullptr, &device) != VK_SUCCESS ||
        device == VK_NULL_HANDLE) {
        std::cerr << "[GLSS Vulkan] vkCreateDevice failed." << std::endl;
        api.destroy_instance(instance, nullptr);
        CloseVulkanLoader(loader);
        return false;
    }

    api.LoadDevice(device);
    if (api.get_device_queue == nullptr || api.destroy_device == nullptr) {
        std::cerr << "[GLSS Vulkan] Failed to resolve device-level entry points." << std::endl;
        if (api.destroy_device != nullptr) {
            api.destroy_device(device, nullptr);
        }
        api.destroy_instance(instance, nullptr);
        CloseVulkanLoader(loader);
        return false;
    }

    VkQueue queue = VK_NULL_HANDLE;
    api.get_device_queue(device, static_cast<uint32_t>(compute_family), 0, &queue);
    if (queue == VK_NULL_HANDLE) {
        std::cerr << "[GLSS Vulkan] vkGetDeviceQueue returned a null queue." << std::endl;
        api.destroy_device(device, nullptr);
        api.destroy_instance(instance, nullptr);
        CloseVulkanLoader(loader);
        return false;
    }

    VkPhysicalDeviceProperties props{};
    api.get_physical_device_properties(physical_device, &props);

    instance_ = instance;
    physical_device_ = physical_device;
    device_ = device;
    compute_queue_ = queue;
    compute_family_idx_ = static_cast<uint32_t>(compute_family);
    destroy_device_ = api.destroy_device;
    destroy_instance_ = api.destroy_instance;
    loader_handle_ = loader;
    initialized_ = true;

    std::cout << "[GLSS Vulkan] Selected device " << selected << ": " << props.deviceName << " (compute queue family "
              << compute_family_idx_ << ")" << std::endl;
    return true;
}

void VulkanContext::Shutdown() {
    if (destroy_device_ != nullptr && device_ != VK_NULL_HANDLE) {
        destroy_device_(device_, nullptr);
    }
    if (destroy_instance_ != nullptr && instance_ != VK_NULL_HANDLE) {
        destroy_instance_(instance_, nullptr);
    }

    instance_ = VK_NULL_HANDLE;
    physical_device_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    compute_queue_ = VK_NULL_HANDLE;
    compute_family_idx_ = 0;
    destroy_device_ = nullptr;
    destroy_instance_ = nullptr;

    if (loader_handle_ != nullptr) {
        CloseVulkanLoader(static_cast<NativeLoader>(loader_handle_));
        loader_handle_ = nullptr;
    }

    initialized_ = false;
}

} // namespace glss
