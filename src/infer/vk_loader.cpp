#include "infer/vk_loader.hpp"

#if ASKMDB_VULKAN

#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vulkan/vulkan_core.h>

#include <atomic>
#include <cstdlib>
#include <string>

// ggml-vulkan links against "Vulkan::Vulkan", which CMake points at this file instead of
// libvulkan: the binary has no DT_NEEDED on libvulkan.so.1 and starts on machines without it.
// ggml-vulkan reaches everything through vulkan-hpp's dynamic dispatcher, initialised from
// vkGetInstanceProcAddr, plus a few direct calls; this file defines exactly those symbols and
// forwards them to the real loader, opened on first use. A null vkGetInstanceProcAddr makes
// ggml-vulkan report "Vulkan 1.2 required" and register no device, so the CPU path goes on.

namespace {

std::atomic<bool> g_enabled{true};
std::atomic<bool> g_loaded{false};

// NVIDIA's driver keeps the shaders it compiles in $XDG_CACHE_HOME/nvidia/GLCache (~/.cache/...)
// but does not create that directory: measured on an NVIDIA GPU, the first process in a fresh
// HOME compiles ggml's shaders (~11 s) and saves nothing, so the second one pays again. Creating
// it first makes the first process save them. Only with the NVIDIA driver loaded and no cache path
// of the user's own (__GL_SHADER_DISK_CACHE_PATH).
void ensure_nvidia_shader_cache() {
    if (access("/proc/driver/nvidia/version", F_OK) != 0 || std::getenv("__GL_SHADER_DISK_CACHE_PATH") != nullptr) {
        return;
    }
    std::string base;
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg != nullptr && xdg[0] == '/') {
        base = xdg;
    } else if (const char* home = std::getenv("HOME"); home != nullptr && home[0] == '/') {
        base = std::string(home) + "/.cache";
    } else {
        return;
    }
    // Errors are ignored: without the directory the driver just compiles again next time.
    for (const std::string& dir : {base, base + "/nvidia", base + "/nvidia/GLCache"}) {
        mkdir(dir.c_str(), 0700);
    }
}

void* library() {
    // Function-local static: opened once, thread-safe.
    static void* const handle = []() -> void* {
        if (!g_enabled.load()) {
            return nullptr;
        }
        ensure_nvidia_shader_cache();
        void* h = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
        if (h == nullptr) {
            h = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        }
        g_loaded.store(h != nullptr);
        return h;
    }();
    return handle;
}

// The loader exports every core command as a trampoline that dispatches on its handle, so the
// direct calls can use the exported symbol of the same name.
template <typename Fn>
Fn resolve(const char* name) {
    void* h = library();
    return h == nullptr ? nullptr : reinterpret_cast<Fn>(dlsym(h, name));
}

}  // namespace

extern "C" {

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* pName) {
    static const auto real = resolve<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
    return real == nullptr ? nullptr : real(instance, pName);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* pName) {
    static const auto real = resolve<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
    return real == nullptr ? nullptr : real(device, pName);
}

// Only reachable with a device, that is, once the loader is open.
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(VkPhysicalDevice physicalDevice,
                                                        VkPhysicalDeviceFeatures2* pFeatures) {
    static const auto real = resolve<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2");
    if (real != nullptr) {
        real(physicalDevice, pFeatures);
    }
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyBuffer(VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkBuffer dstBuffer,
                                           uint32_t regionCount, const VkBufferCopy* pRegions) {
    static const auto real = resolve<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer");
    if (real != nullptr) {
        real(commandBuffer, srcBuffer, dstBuffer, regionCount, pRegions);
    }
}

}  // extern "C"

namespace askmdb::infer {

bool vulkan_built() { return true; }
void vulkan_set_enabled(bool enabled) { g_enabled.store(enabled); }
bool vulkan_loaded() { return g_loaded.load(); }

}  // namespace askmdb::infer

#else

// Build without Vulkan: no backend, nothing to load.

namespace askmdb::infer {

bool vulkan_built() { return false; }
void vulkan_set_enabled(bool) {}
bool vulkan_loaded() { return false; }

}  // namespace askmdb::infer

#endif
