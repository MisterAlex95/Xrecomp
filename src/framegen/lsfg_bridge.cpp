// The LSFG bridge (lsfg_bridge.h): a Vulkan compute device running framegen on plain images, behind a C interface.
//
// framegen is the MIT library of the pinned lsfg-vk fork (GameNative/lsfg-vk-android d33caceb, framegen/); this
// file is ours and only calls its public API (framegen/public/lsfg_3_1.hpp, lsfg_3_1p.hpp). Vulkan comes through
// volk (dlopen), so nothing links libvulkan.
#include <volk.h>

#include "lsfg_bridge.h"
#include "lsfg_cache.h"
#include "lsfg_spv.h"

#include "lsfg_3_1.hpp"
#include "lsfg_3_1p.hpp"
#include "common/exception.hpp"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr uint64_t kWaitNs = 2'000'000'000ull;   // bounded waits: a wedged queue must not freeze the caller
constexpr uint32_t kMaxGen = 3;                  // multiplier 4

// An error with its LSFG_E_* code, thrown inside the bridge and caught at the C boundary.
struct BridgeError : std::runtime_error {
    int code;
    BridgeError(int c, const std::string &m) : std::runtime_error(m), code(c) {}
};

std::string g_err;          // last error message
std::string g_loader_err;   // why the shader loader returned nothing (framegen only says "Shader code is empty")

std::string fmt(const char *f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

int code_for(VkResult r, int otherwise) {
    switch (r) {
    case VK_ERROR_DEVICE_LOST: return LSFG_E_DEVICE_LOST;
    case VK_TIMEOUT: return LSFG_E_TIMEOUT;
    case VK_ERROR_OUT_OF_HOST_MEMORY: return LSFG_E_NOMEM;
    case VK_ERROR_INCOMPATIBLE_DRIVER: return LSFG_E_NODEVICE;
    case VK_ERROR_FEATURE_NOT_PRESENT:
    case VK_ERROR_EXTENSION_NOT_PRESENT: return LSFG_E_FEATURE;
    default: return otherwise;
    }
}

void check(VkResult r, const char *what) {
    if (r != VK_SUCCESS)
        throw BridgeError(code_for(r, LSFG_E_VULKAN), fmt("%s failed (VkResult %d)", what, (int)r));
}

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

struct Image {
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
};

struct Buffer {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    void *map = nullptr;
    bool coherent = false;
    VkDeviceSize size = 0;
};

// What the physical device offers, for the feature check and device creation.
struct Caps {
    VkPhysicalDeviceProperties props{};
    uint32_t api = 0;   // min(device, instance)
    VkPhysicalDeviceFeatures core{};
    bool fp16 = false, timeline = false, memModel = false;
    bool extFp16 = false, extTimeline = false, extMemModel = false, extPortability = false;
    uint32_t queueFamily = UINT32_MAX;
    std::string missing;   // required things that are absent (empty = usable)
    std::string notes;     // optional things that are absent
};

struct Bridge {
    bool open = false;
    bool broken = false;
    int brokenCode = 0;
    LsfgConfig cfg{};
    std::string dir;
    bool perf = false;
    uint32_t gens = 1;
    uint32_t width = 0, height = 0;

    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    uint32_t qfi = 0;
    VkQueue queue = VK_NULL_HANDLE;
    Caps caps;
    VkPhysicalDeviceMemoryProperties mem{};

    Image in[2], out[kMaxGen];
    Buffer up, down[kMaxGen];
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cbUp = VK_NULL_HANDLE, cbDown = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    VkPipelineCache pcache = VK_NULL_HANDLE;
    std::string pcachePath;
    PFN_vkCreateComputePipelines realCreateComputePipelines = nullptr;

    bool fgInit = false;
    int32_t ctx = 0;
    bool haveCtx = false;
    uint64_t fgIdx = 0;   // mirrors framegen's frame counter: in[fgIdx % 2] is the next input
    bool primed = false;

    LsfgBridgeInfo info{};
    uint32_t lastGenUs = 0;
};

Bridge g;

// ── Vulkan setup ────────────────────────────────────────────────────────────

bool has_ext(const std::vector<VkExtensionProperties> &v, const char *name) {
    for (const auto &e : v)
        if (std::strcmp(e.extensionName, name) == 0)
            return true;
    return false;
}

void load_vulkan() {
    if (volkInitialize() != VK_SUCCESS)
        throw BridgeError(LSFG_E_NOLOADER, "no Vulkan loader (libvulkan / MoltenVK) could be loaded");
}

VkInstance create_instance(uint32_t *apiOut) {
    uint32_t loaderApi = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion)
        vkEnumerateInstanceVersion(&loaderApi);
    if (loaderApi < VK_API_VERSION_1_1)
        throw BridgeError(LSFG_E_NODEVICE, "the Vulkan loader is 1.0; 1.1 is needed");
    uint32_t api = std::min<uint32_t>(loaderApi, VK_API_VERSION_1_3);

    uint32_t n = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n);
    vkEnumerateInstanceExtensionProperties(nullptr, &n, exts.data());
    std::vector<const char *> enable;
    VkInstanceCreateFlags flags = 0;
#ifdef VK_KHR_portability_enumeration
    // MoltenVK through the loader is a "portability" driver, listed only when asked for.
    if (has_ext(exts, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        enable.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
#endif
    const VkApplicationInfo app{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "xboxrecomp-lsfg",
        .applicationVersion = 1,
        .pEngineName = "xboxrecomp",
        .engineVersion = 1,
        .apiVersion = api,
    };
    const VkInstanceCreateInfo ci{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .flags = flags,
        .pApplicationInfo = &app,
        .enabledExtensionCount = (uint32_t)enable.size(),
        .ppEnabledExtensionNames = enable.data(),
    };
    VkInstance inst = VK_NULL_HANDLE;
    VkResult r = vkCreateInstance(&ci, nullptr, &inst);
    if (r != VK_SUCCESS)
        throw BridgeError(r == VK_ERROR_INCOMPATIBLE_DRIVER ? LSFG_E_NODEVICE : LSFG_E_VULKAN,
                          fmt("vkCreateInstance failed (VkResult %d)", (int)r));
    volkLoadInstance(inst);
    *apiOut = api;
    return inst;
}

Caps probe_caps(VkPhysicalDevice pd, uint32_t instApi) {
    Caps c;
    vkGetPhysicalDeviceProperties(pd, &c.props);
    c.api = std::min(c.props.apiVersion, instApi);
    if (c.api < VK_API_VERSION_1_1) {
        c.missing = "Vulkan 1.1";
        return c;
    }
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n);
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, exts.data());
    c.extFp16 = has_ext(exts, VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME);
    c.extTimeline = has_ext(exts, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
    c.extMemModel = has_ext(exts, VK_KHR_VULKAN_MEMORY_MODEL_EXTENSION_NAME);
    c.extPortability = has_ext(exts, "VK_KHR_portability_subset");

    VkPhysicalDeviceVulkan12Features f12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceShaderFloat16Int8Features f16{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES};
    VkPhysicalDeviceTimelineSemaphoreFeatures ftl{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    VkPhysicalDeviceVulkanMemoryModelFeatures fmm{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES};
    VkPhysicalDeviceFeatures2 f2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    if (c.api >= VK_API_VERSION_1_2) {
        f2.pNext = &f12;
    } else {
        void *next = nullptr;
        if (c.extFp16) { f16.pNext = next; next = &f16; }
        if (c.extTimeline) { ftl.pNext = next; next = &ftl; }
        if (c.extMemModel) { fmm.pNext = next; next = &fmm; }
        f2.pNext = next;
    }
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    c.core = f2.features;
    if (c.api >= VK_API_VERSION_1_2) {
        c.fp16 = f12.shaderFloat16;
        c.timeline = f12.timelineSemaphore;
        c.memModel = f12.vulkanMemoryModel;
    } else {
        c.fp16 = c.extFp16 && f16.shaderFloat16;
        c.timeline = c.extTimeline && ftl.timelineSemaphore;
        c.memModel = c.extMemModel && fmm.vulkanMemoryModel;
    }

    vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, nullptr);
    std::vector<VkQueueFamilyProperties> fams(n);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, fams.data());
    for (uint32_t i = 0; i < n && c.queueFamily == UINT32_MAX; i++)
        if (fams[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
            c.queueFamily = i;

    auto need = [&](bool ok, const char *what) {
        if (!ok)
            c.missing += c.missing.empty() ? what : std::string(", ") + what;
    };
    auto want = [&](bool ok, const char *what) {
        if (!ok)
            c.notes += c.notes.empty() ? what : std::string(", ") + what;
    };
    need(c.queueFamily != UINT32_MAX, "compute queue");
    need(c.core.shaderStorageImageExtendedFormats, "shaderStorageImageExtendedFormats");
    want(c.core.shaderStorageImageReadWithoutFormat, "shaderStorageImageReadWithoutFormat");
    want(c.core.shaderStorageImageWriteWithoutFormat, "shaderStorageImageWriteWithoutFormat");
    want(c.fp16, "shaderFloat16");
    // The formats framegen's images use (framegen/src/core/image.cpp callers): storage + sampled, optimal tiling.
    const VkFormat fmts[] = {VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT};
    const char *names[] = {"R8_UNORM storage", "R8G8B8A8_UNORM storage", "R16G16B16A16_SFLOAT storage"};
    for (int i = 0; i < 3; i++) {
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(pd, fmts[i], &fp);
        const VkFormatFeatureFlags f = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        need((fp.optimalTilingFeatures & f) == f, names[i]);
    }
    return c;
}

int type_rank(VkPhysicalDeviceType t) {
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 0;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 1;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 2;
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return 3;
    default: return 4;
    }
}

// Picks a usable physical device (RECOMP_LSFG_GPU=<index> forces one). Throws when none is usable.
VkPhysicalDevice pick_device(VkInstance inst, uint32_t instApi, Caps *out) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, nullptr);
    if (n == 0)
        throw BridgeError(LSFG_E_NODEVICE, "no Vulkan device");
    std::vector<VkPhysicalDevice> pds(n);
    vkEnumeratePhysicalDevices(inst, &n, pds.data());
    const char *force = std::getenv("RECOMP_LSFG_GPU");
    int best = -1;
    std::string why;
    std::vector<Caps> caps(n);
    for (uint32_t i = 0; i < n; i++) {
        caps[i] = probe_caps(pds[i], instApi);
        if (force && *force && (uint32_t)std::atoi(force) != i)
            continue;
        if (!caps[i].missing.empty()) {
            why += fmt("%s%s: missing %s", why.empty() ? "" : "; ", caps[i].props.deviceName, caps[i].missing.c_str());
            continue;
        }
        if (best < 0 || type_rank(caps[i].props.deviceType) < type_rank(caps[best].props.deviceType))
            best = (int)i;
    }
    if (best < 0)
        throw BridgeError(LSFG_E_FEATURE, why.empty() ? "no matching Vulkan device" : why);
    *out = caps[best];
    return pds[best];
}

std::string describe(const Caps &c) {
    return fmt("%s (Vulkan %u.%u.%u, driver 0x%x, vendor 0x%04x device 0x%04x)", c.props.deviceName,
               VK_VERSION_MAJOR(c.api), VK_VERSION_MINOR(c.api), VK_VERSION_PATCH(c.api), c.props.driverVersion,
               c.props.vendorID, c.props.deviceID);
}

void create_device() {
    Caps &c = g.caps;
    std::vector<const char *> exts;
    if (c.extPortability)
        exts.push_back("VK_KHR_portability_subset");   // required whenever the driver lists it
    VkPhysicalDeviceVulkan12Features f12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceShaderFloat16Int8Features f16{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES};
    VkPhysicalDeviceTimelineSemaphoreFeatures ftl{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    VkPhysicalDeviceVulkanMemoryModelFeatures fmm{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES};
    VkPhysicalDeviceFeatures2 f2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    // Only what the device reports: some drivers accept an unsupported feature and fail on the first dispatch.
    f2.features.shaderStorageImageExtendedFormats = c.core.shaderStorageImageExtendedFormats;
    f2.features.shaderStorageImageReadWithoutFormat = c.core.shaderStorageImageReadWithoutFormat;
    f2.features.shaderStorageImageWriteWithoutFormat = c.core.shaderStorageImageWriteWithoutFormat;
    f2.features.shaderInt16 = c.core.shaderInt16;
    if (c.api >= VK_API_VERSION_1_2) {
        f12.shaderFloat16 = c.fp16;
        f12.timelineSemaphore = c.timeline;
        f12.vulkanMemoryModel = c.memModel;
        f2.pNext = &f12;
    } else {
        void *next = nullptr;
        if (c.fp16) { exts.push_back(VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME); f16.shaderFloat16 = VK_TRUE; f16.pNext = next; next = &f16; }
        if (c.timeline) { exts.push_back(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME); ftl.timelineSemaphore = VK_TRUE; ftl.pNext = next; next = &ftl; }
        if (c.memModel) { exts.push_back(VK_KHR_VULKAN_MEMORY_MODEL_EXTENSION_NAME); fmm.vulkanMemoryModel = VK_TRUE; fmm.pNext = next; next = &fmm; }
        f2.pNext = next;
    }
    const float prio = 1.0f;
    const VkDeviceQueueCreateInfo q{
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = c.queueFamily,
        .queueCount = 1,
        .pQueuePriorities = &prio,
    };
    const VkDeviceCreateInfo ci{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &f2,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &q,
        .enabledExtensionCount = (uint32_t)exts.size(),
        .ppEnabledExtensionNames = exts.data(),
    };
    check(vkCreateDevice(g.phys, &ci, nullptr, &g.dev), "vkCreateDevice");
    volkLoadDevice(g.dev);
    g.qfi = c.queueFamily;
    vkGetDeviceQueue(g.dev, g.qfi, 0, &g.queue);
    vkGetPhysicalDeviceMemoryProperties(g.phys, &g.mem);
}

uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags prefer) {
    int fallback = -1;
    for (uint32_t i = 0; i < g.mem.memoryTypeCount; i++) {
        if (!(bits & (1u << i)))
            continue;
        VkMemoryPropertyFlags f = g.mem.memoryTypes[i].propertyFlags;
        if ((f & want) != want)
            continue;
        if ((f & prefer) == prefer)
            return i;
        if (fallback < 0)
            fallback = (int)i;
    }
    if (fallback < 0)
        throw BridgeError(LSFG_E_VULKAN, "no suitable memory type");
    return (uint32_t)fallback;
}

Image make_image(uint32_t w, uint32_t h) {
    Image im;
    const VkImageCreateInfo ci{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {w, h, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    check(vkCreateImage(g.dev, &ci, nullptr, &im.img), "vkCreateImage");
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g.dev, im.img, &req);
    const VkMemoryAllocateInfo ai{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0),
    };
    check(vkAllocateMemory(g.dev, &ai, nullptr, &im.mem), "vkAllocateMemory(image)");
    check(vkBindImageMemory(g.dev, im.img, im.mem, 0), "vkBindImageMemory");
    return im;
}

Buffer make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags prefer) {
    Buffer b;
    b.size = size;
    const VkBufferCreateInfo ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    check(vkCreateBuffer(g.dev, &ci, nullptr, &b.buf), "vkCreateBuffer");
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g.dev, b.buf, &req);
    uint32_t t = mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, prefer);
    b.coherent = g.mem.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const VkMemoryAllocateInfo ai{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = t,
    };
    check(vkAllocateMemory(g.dev, &ai, nullptr, &b.mem), "vkAllocateMemory(buffer)");
    check(vkBindBufferMemory(g.dev, b.buf, b.mem, 0), "vkBindBufferMemory");
    check(vkMapMemory(g.dev, b.mem, 0, VK_WHOLE_SIZE, 0, &b.map), "vkMapMemory");
    return b;
}

void destroy_image(Image &im) {
    if (im.img) vkDestroyImage(g.dev, im.img, nullptr);
    if (im.mem) vkFreeMemory(g.dev, im.mem, nullptr);
    im = Image{};
}

void destroy_buffer(Buffer &b) {
    if (b.buf) vkDestroyBuffer(g.dev, b.buf, nullptr);
    if (b.mem) vkFreeMemory(g.dev, b.mem, nullptr);
    b = Buffer{};
}

void begin(VkCommandBuffer cb) {
    check(vkResetCommandBuffer(cb, 0), "vkResetCommandBuffer");
    const VkCommandBufferBeginInfo bi{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    check(vkBeginCommandBuffer(cb, &bi), "vkBeginCommandBuffer");
}

void submit(VkCommandBuffer cb, VkFence fence) {
    check(vkEndCommandBuffer(cb), "vkEndCommandBuffer");
    const VkSubmitInfo si{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = cb ? 1u : 0u,
        .pCommandBuffers = &cb,
    };
    check(vkQueueSubmit(g.queue, 1, &si, fence), "vkQueueSubmit");
}

// Waits for the fence (which also covers every earlier submission on the queue), then resets it.
void wait_fence() {
    VkResult r = vkWaitForFences(g.dev, 1, &g.fence, VK_TRUE, kWaitNs);
    if (r == VK_TIMEOUT)
        throw BridgeError(LSFG_E_TIMEOUT, "frame generation did not finish within 2 s");
    check(r, "vkWaitForFences");
    check(vkResetFences(g.dev, 1, &g.fence), "vkResetFences");
}

// A fence-only submit: completes when everything submitted before it does.
void drain() {
    check(vkQueueSubmit(g.queue, 0, nullptr, g.fence), "vkQueueSubmit(fence)");
    wait_fence();
}

void image_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkPipelineStageFlags src, VkAccessFlags srcA,
                   VkPipelineStageFlags dst, VkAccessFlags dstA) {
    const VkImageMemoryBarrier b{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = srcA,
        .dstAccessMask = dstA,
        .oldLayout = from,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    vkCmdPipelineBarrier(cb, src, dst, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// ── pipeline cache ──────────────────────────────────────────────────────────

// framegen creates its compute pipelines with VK_NULL_HANDLE as the cache (framegen/src/core/pipeline.cpp). volk's
// dispatch pointers are plain globals, so while the bridge is open this wrapper hands the driver our cache instead.
VKAPI_ATTR VkResult VKAPI_CALL create_compute_pipelines(VkDevice dev, VkPipelineCache cache, uint32_t n,
                                                        const VkComputePipelineCreateInfo *ci,
                                                        const VkAllocationCallbacks *alloc, VkPipeline *out) {
    return g.realCreateComputePipelines(dev, cache ? cache : g.pcache, n, ci, alloc, out);
}

void open_pipeline_cache() {
    const VkPhysicalDeviceProperties &p = g.caps.props;
    g.pcachePath = g.dir + fmt("/pipeline-%04x-%04x-%08x.bin", p.vendorID, p.deviceID, p.driverVersion);
    std::vector<uint8_t> data;
    if (FILE *f = std::fopen(g.pcachePath.c_str(), "rb")) {
        uint8_t buf[65536];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
            data.insert(data.end(), buf, buf + n);
        std::fclose(f);
    }
    // VkPipelineCacheHeaderVersionOne: length, version, vendor, device, UUID. A stale file is ignored.
    auto rd = [&](size_t o) { uint32_t v; std::memcpy(&v, data.data() + o, 4); return v; };
    if (data.size() < 32 || rd(4) != VK_PIPELINE_CACHE_HEADER_VERSION_ONE || rd(8) != p.vendorID ||
        rd(12) != p.deviceID || std::memcmp(data.data() + 16, p.pipelineCacheUUID, VK_UUID_SIZE) != 0)
        data.clear();
    const VkPipelineCacheCreateInfo ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
        .initialDataSize = data.size(),
        .pInitialData = data.empty() ? nullptr : data.data(),
    };
    if (vkCreatePipelineCache(g.dev, &ci, nullptr, &g.pcache) != VK_SUCCESS) {
        const VkPipelineCacheCreateInfo empty{.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        check(vkCreatePipelineCache(g.dev, &empty, nullptr, &g.pcache), "vkCreatePipelineCache");
        data.clear();
    }
    g.info.pipeline_cache_bytes_in = data.size();
}

int save_pipeline_cache() {
    if (!g.dev || !g.pcache || g.pcachePath.empty())
        return LSFG_E_STATE;
    size_t n = 0;
    if (vkGetPipelineCacheData(g.dev, g.pcache, &n, nullptr) != VK_SUCCESS || n == 0)
        return LSFG_E_VULKAN;
    std::vector<uint8_t> data(n);
    if (vkGetPipelineCacheData(g.dev, g.pcache, &n, data.data()) != VK_SUCCESS)
        return LSFG_E_VULKAN;
    std::string tmp = g.pcachePath + ".tmp";
    FILE *f = std::fopen(tmp.c_str(), "wb");
    if (!f)
        return LSFG_E_VULKAN;
    bool ok = std::fwrite(data.data(), 1, n, f) == n;
    ok = std::fclose(f) == 0 && ok;
    if (!ok || std::rename(tmp.c_str(), g.pcachePath.c_str()) != 0) {
        std::remove(tmp.c_str());
        return LSFG_E_VULKAN;
    }
    return 0;
}

// ── framegen ────────────────────────────────────────────────────────────────

void fg_finalize() {
    if (g.haveCtx) {
        g.haveCtx = false;
        try {
            if (g.perf) LSFG_3_1P::deleteContext(g.ctx);
            else LSFG_3_1::deleteContext(g.ctx);
        } catch (...) {
        }
    }
    if (g.fgInit) {
        g.fgInit = false;
        try {
            if (g.perf) LSFG_3_1P::finalize();
            else LSFG_3_1::finalize();
        } catch (...) {
        }
    }
}

void teardown() {
    fg_finalize();
    if (g.dev) {
        vkDeviceWaitIdle(g.dev);
        if (g.realCreateComputePipelines) {
            vkCreateComputePipelines = g.realCreateComputePipelines;
            g.realCreateComputePipelines = nullptr;
        }
        if (g.pcache) {
            save_pipeline_cache();
            vkDestroyPipelineCache(g.dev, g.pcache, nullptr);
        }
        for (auto &im : g.in) destroy_image(im);
        for (auto &im : g.out) destroy_image(im);
        destroy_buffer(g.up);
        for (auto &b : g.down) destroy_buffer(b);
        if (g.fence) vkDestroyFence(g.dev, g.fence, nullptr);
        if (g.pool) vkDestroyCommandPool(g.dev, g.pool, nullptr);
        vkDestroyDevice(g.dev, nullptr);
    }
    if (g.inst)
        vkDestroyInstance(g.inst, nullptr);
    g = Bridge{};
}

// Chooses the shader form from the cache manifest and the device; throws LSFG_E_SHADERS when nothing usable.
void choose_shaders(unsigned *allowed, bool *fp16) {
    LsfgCacheInfo ci;
    int rc = lsfg_cache_read_manifest(g.dir.c_str(), &ci);
    if (rc)
        throw BridgeError(LSFG_E_SHADERS, fmt("%s: %s", g.dir.c_str(), lsfg_cache_strerror(rc)));
    int h16 = 0, h32 = 0, hspv = 0;
    lsfg_cache_variant_ready(&ci, g.perf, &h16, &h32, &hspv);
    bool want16 = g.cfg.fp16 == LSFG_FP16_ON || (g.cfg.fp16 == LSFG_FP16_AUTO && g.caps.fp16);
    if (want16 && h16 && g.caps.fp16) {
        *allowed = LSFG_FORM_FP16;
        *fp16 = true;
        return;
    }
    if (g.cfg.fp16 == LSFG_FP16_ON && !(h16 && g.caps.fp16))
        throw BridgeError(h16 ? LSFG_E_FEATURE : LSFG_E_SHADERS,
                          h16 ? "FP16 shaders need shaderFloat16" : "no FP16 shaders in the cache");
    *allowed = (hspv ? LSFG_FORM_SPV : 0u) | (h32 ? LSFG_FORM_FP32 : 0u);
    *fp16 = false;
    if (*allowed)
        return;
        throw BridgeError(LSFG_E_SHADERS,
                          fmt("the shader cache has no complete %s set (%u/%u DXBC, %u translated, %u FP16, %u FP32): "
                              "run lsfg_translate",
                              g.perf ? "LSFG 3.1 performance" : "LSFG 3.1", ci.dxbc[g.perf], ci.needed[g.perf],
                              ci.spv[g.perf], ci.fp16[g.perf], ci.fp32[g.perf]));
}

// Loads every shader of the variant once before Vulkan sees any: the cache must be complete and valid, and the
// device must offer what the modules declare (translated DXBC is SPIR-V 1.6 with VulkanMemoryModel: Vulkan 1.3).
void check_shaders(unsigned allowed, bool fp16) {
    const Caps &c = g.caps;
    const uint32_t maxVersion = c.api >= VK_API_VERSION_1_3 ? 0x10600u : c.api >= VK_API_VERSION_1_2 ? 0x10500u : 0x10300u;
    size_t n = 0;
    const LsfgShaderInfo *table = lsfg_shader_table(&n);
    for (size_t i = 0; i < n; i++) {
        if (table[i].performance != (g.perf ? 1 : 0))
            continue;
        uint8_t *spv = nullptr;
        size_t len = 0;
        char err[512] = "";
        int rc = lsfg_cache_load_shader(g.dir.c_str(), table[i].name, allowed, fp16, &spv, &len, nullptr, err,
                                        sizeof err);
        if (rc)
            throw BridgeError(LSFG_E_SHADERS, err);
        uint32_t version = 0, caps = 0;
        rc = lsfg_spv_requirements(reinterpret_cast<const uint32_t *>(spv), len / 4, &version, &caps);
        std::free(spv);
        if (rc)
            throw BridgeError(LSFG_E_SHADERS, fmt("%s: %s", table[i].name, lsfg_spv_strerror(rc)));
        std::string miss;
        auto need = [&](bool ok, const char *what) {
            if (!ok)
                miss += miss.empty() ? what : std::string(", ") + what;
        };
        need(version <= maxVersion, version >= 0x10600u ? "Vulkan 1.3 (SPIR-V 1.6)" : "a newer Vulkan (SPIR-V version)");
        need(!(caps & LSFG_SPV_CAP_VULKAN_MEMORY_MODEL) || c.memModel, "vulkanMemoryModel");
        need(!(caps & LSFG_SPV_CAP_READ_WITHOUT_FORMAT) || c.core.shaderStorageImageReadWithoutFormat,
             "shaderStorageImageReadWithoutFormat");
        need(!(caps & LSFG_SPV_CAP_WRITE_WITHOUT_FORMAT) || c.core.shaderStorageImageWriteWithoutFormat,
             "shaderStorageImageWriteWithoutFormat");
        need(!(caps & LSFG_SPV_CAP_FLOAT16) || c.fp16, "shaderFloat16");
        need(!(caps & LSFG_SPV_CAP_INT16) || c.core.shaderInt16, "shaderInt16");
        if (!miss.empty())
            throw BridgeError(LSFG_E_FEATURE, fmt("the shaders need %s", miss.c_str()));
    }
}

void init_framegen() {
    unsigned allowed = 0;
    bool fp16 = false;
    choose_shaders(&allowed, &fp16);
    check_shaders(allowed, fp16);
    g.info.fp16 = fp16;
    std::string dir = g.dir;
    auto loader = [dir, allowed, fp16](const std::string &name) -> std::vector<uint8_t> {
        uint8_t *spv = nullptr;
        size_t len = 0;
        unsigned form = 0;
        char err[512] = "";
        if (lsfg_cache_load_shader(dir.c_str(), name.c_str(), allowed, fp16, &spv, &len, &form, err, sizeof err)) {
            g_loader_err = err;
            return {};
        }
        g.info.shader_form |= form;
        std::vector<uint8_t> v(spv, spv + len);
        std::free(spv);
        return v;
    };
    float flow = g.cfg.flow_scale > 0.0f ? g.cfg.flow_scale : 1.0f;
    if (g.perf)
        LSFG_3_1P::initializeExternal(vkGetInstanceProcAddr, g.inst, g.phys, g.dev, g.qfi, g.queue, false, flow,
                                      g.gens, loader);
    else
        LSFG_3_1::initializeExternal(vkGetInstanceProcAddr, g.inst, g.phys, g.dev, g.qfi, g.queue, false, flow,
                                     g.gens, loader);
    g.fgInit = true;

    // initializeExternal reloaded volk's device table: install the pipeline-cache wrapper now.
    open_pipeline_cache();
    g.realCreateComputePipelines = vkCreateComputePipelines;
    vkCreateComputePipelines = create_compute_pipelines;

    std::vector<VkImage> outs;
    for (uint32_t i = 0; i < g.gens; i++)
        outs.push_back(g.out[i].img);
    const VkExtent2D ext{g.width, g.height};
    auto t0 = std::chrono::steady_clock::now();
    try {
        g.ctx = g.perf ? LSFG_3_1P::createContextFromImages(g.in[0].img, g.in[1].img, outs, ext, VK_FORMAT_R8G8B8A8_UNORM)
                       : LSFG_3_1::createContextFromImages(g.in[0].img, g.in[1].img, outs, ext, VK_FORMAT_R8G8B8A8_UNORM);
    } catch (const LSFG::vulkan_error &e) {
        // Context creation is where shader modules and pipelines are made: a driver refusal lands here.
        throw BridgeError(code_for(e.error(), LSFG_E_MODULE), fmt("shader module or pipeline rejected: %s", e.what()));
    }
    g.haveCtx = true;
    g.info.pipeline_ms = ms_since(t0);
    save_pipeline_cache();
}

void open_impl(const LsfgConfig *cfg) {
    if (g.open)
        throw BridgeError(LSFG_E_STATE, "the bridge is already open");
    if (!cfg || cfg->width == 0 || cfg->height == 0 || cfg->width > 8192 || cfg->height > 8192 ||
        cfg->multiplier < 2 || cfg->multiplier > kMaxGen + 1 || cfg->flow_scale < 0.0f || cfg->flow_scale > 1.0f)
        throw BridgeError(LSFG_E_ARG, "invalid configuration (size 1..8192, multiplier 2..4, flow 0..1)");
    g = Bridge{};
    g.cfg = *cfg;
    g.perf = cfg->performance != 0;
    g.gens = cfg->multiplier - 1;
    g.width = cfg->width;
    g.height = cfg->height;
    if (cfg->shader_dir && cfg->shader_dir[0]) {
        g.dir = cfg->shader_dir;
    } else {
        char base[2048], dir[4096];
        if (lsfg_cache_default_base(base, sizeof base) || lsfg_cache_current(base, dir, sizeof dir))
            throw BridgeError(LSFG_E_SHADERS, "no shader cache (set RECOMP_LSFG_DLL or RECOMP_LSFG_DIR)");
        g.dir = dir;
    }

    auto t0 = std::chrono::steady_clock::now();
    load_vulkan();
    uint32_t api = 0;
    g.inst = create_instance(&api);
    g.phys = pick_device(g.inst, api, &g.caps);
    create_device();
    snprintf(g.info.device, sizeof g.info.device, "%s", describe(g.caps).c_str());
    g.info.vendor_id = g.caps.props.vendorID;
    g.info.device_id = g.caps.props.deviceID;
    g.info.driver_version = g.caps.props.driverVersion;
    g.info.api_version = g.caps.api;

    const VkCommandPoolCreateInfo pci{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = g.qfi,
    };
    check(vkCreateCommandPool(g.dev, &pci, nullptr, &g.pool), "vkCreateCommandPool");
    VkCommandBuffer cbs[2];
    const VkCommandBufferAllocateInfo cai{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = g.pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 2,
    };
    check(vkAllocateCommandBuffers(g.dev, &cai, cbs), "vkAllocateCommandBuffers");
    g.cbUp = cbs[0];
    g.cbDown = cbs[1];
    const VkFenceCreateInfo fci{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    check(vkCreateFence(g.dev, &fci, nullptr, &g.fence), "vkCreateFence");

    const VkDeviceSize frame = (VkDeviceSize)g.width * g.height * 4;
    for (auto &im : g.in) im = make_image(g.width, g.height);
    for (uint32_t i = 0; i < g.gens; i++) g.out[i] = make_image(g.width, g.height);
    g.up = make_buffer(frame, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    for (uint32_t i = 0; i < g.gens; i++)
        g.down[i] = make_buffer(frame, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT);

    // Every image to GENERAL once (framegen's convention for wrapped images), cleared to black.
    begin(g.cbUp);
    std::vector<VkImage> all = {g.in[0].img, g.in[1].img};
    for (uint32_t i = 0; i < g.gens; i++) all.push_back(g.out[i].img);
    const VkClearColorValue black{};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    for (VkImage im : all) {
        image_barrier(g.cbUp, im, VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        vkCmdClearColorImage(g.cbUp, im, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &range);
        image_barrier(g.cbUp, im, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    }
    submit(g.cbUp, g.fence);
    wait_fence();
    g.info.device_ms = ms_since(t0);

    init_framegen();
    g.open = true;
}

void mark_broken(int code) {
    g.broken = true;
    g.brokenCode = code;
}

// Records and submits the upload of frame into in[slot].
void upload(const uint8_t *frame, uint32_t pitch, uint32_t slot) {
    const uint32_t row = g.width * 4;
    auto *dst = static_cast<uint8_t *>(g.up.map);
    for (uint32_t y = 0; y < g.height; y++)
        std::memcpy(dst + (size_t)y * row, frame + (size_t)y * pitch, row);
    if (!g.up.coherent) {
        const VkMappedMemoryRange r{.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = g.up.mem, .size = VK_WHOLE_SIZE};
        check(vkFlushMappedMemoryRanges(g.dev, 1, &r), "vkFlushMappedMemoryRanges");
    }
    begin(g.cbUp);
    // Write-after-read: framegen read this image in the last two generations.
    image_barrier(g.cbUp, g.in[slot].img, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    const VkBufferImageCopy copy{
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {g.width, g.height, 1},
    };
    vkCmdCopyBufferToImage(g.cbUp, g.up.buf, g.in[slot].img, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    image_barrier(g.cbUp, g.in[slot].img, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    submit(g.cbUp, VK_NULL_HANDLE);
}

void present() {
    if (g.perf) LSFG_3_1P::presentContext(g.ctx, -1, {});
    else LSFG_3_1::presentContext(g.ctx, -1, {});
    g.fgIdx++;
}

int generate_impl(const uint8_t *frame, uint32_t pitch, uint8_t *const *out, uint32_t out_pitch) {
    if (!g.open)
        throw BridgeError(LSFG_E_STATE, "the bridge is not open");
    if (g.broken)
        throw BridgeError(g.brokenCode, g_err.empty() ? "the bridge failed earlier" : g_err);
    if (!frame || pitch < g.width * 4 || (out && out_pitch < g.width * 4))
        throw BridgeError(LSFG_E_ARG, "invalid frame or output");
    for (uint32_t i = 0; out && i < g.gens; i++)
        if (!out[i])
            throw BridgeError(LSFG_E_ARG, "missing output buffer");
    auto t0 = std::chrono::steady_clock::now();
    if (!g.primed || !out) {
        // History: the frame goes where framegen expects the previous one at its next present.
        upload(frame, pitch, (uint32_t)((g.fgIdx + 1) % 2));
        drain();
        g.primed = true;
        return LSFG_PRIMED;
    }
    upload(frame, pitch, (uint32_t)(g.fgIdx % 2));
    present();

    begin(g.cbDown);
    const VkBufferImageCopy copy{
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {g.width, g.height, 1},
    };
    for (uint32_t i = 0; i < g.gens; i++) {
        image_barrier(g.cbDown, g.out[i].img, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                      VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        vkCmdCopyImageToBuffer(g.cbDown, g.out[i].img, VK_IMAGE_LAYOUT_GENERAL, g.down[i].buf, 1, &copy);
    }
    const VkMemoryBarrier host{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
    };
    vkCmdPipelineBarrier(g.cbDown, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr,
                         0, nullptr);
    // Write-after-read for the next generation into the outputs.
    vkCmdPipelineBarrier(g.cbDown, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 0, nullptr);
    submit(g.cbDown, g.fence);
    wait_fence();

    const uint32_t row = g.width * 4;
    for (uint32_t i = 0; i < g.gens; i++) {
        if (!g.down[i].coherent) {
            const VkMappedMemoryRange r{.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = g.down[i].mem,
                                        .size = VK_WHOLE_SIZE};
            check(vkInvalidateMappedMemoryRanges(g.dev, 1, &r), "vkInvalidateMappedMemoryRanges");
        }
        const auto *src = static_cast<const uint8_t *>(g.down[i].map);
        for (uint32_t y = 0; y < g.height; y++)
            std::memcpy(out[i] + (size_t)y * out_pitch, src + (size_t)y * row, row);
    }
    g.lastGenUs = (uint32_t)std::min(ms_since(t0) * 1000.0, 4.0e9);
    return LSFG_OK;
}

// Runs fn, turning every exception into an LSFG_E_* code and a message.
template <typename F>
int guarded(F &&fn, char *err, size_t errlen, bool breaks) {
    int rc;
    try {
        return fn();
    } catch (const BridgeError &e) {
        rc = e.code;
        g_err = e.what();
    } catch (const LSFG::vulkan_error &e) {
        rc = code_for(e.error(), LSFG_E_VULKAN);
        g_err = e.what();
    } catch (const std::bad_alloc &) {
        rc = LSFG_E_NOMEM;
        g_err = "out of memory";
    } catch (const std::exception &e) {
        // framegen throws runtime_error("Shader code is empty: <name>") when the loader returned nothing.
        rc = std::strstr(e.what(), "Shader code is empty") ? LSFG_E_SHADERS : LSFG_E_INTERNAL;
        g_err = rc == LSFG_E_SHADERS && !g_loader_err.empty() ? g_loader_err : e.what();
    } catch (...) {
        rc = LSFG_E_INTERNAL;
        g_err = "unknown exception";
    }
    if (breaks && g.open && rc != LSFG_E_ARG && rc != LSFG_E_STATE)
        mark_broken(rc);
    if (err && errlen)
        snprintf(err, errlen, "%s", g_err.c_str());
    return rc;
}

} // namespace

extern "C" {

int lsfg_bridge_open(const LsfgConfig *cfg, char *err, size_t errlen) {
    g_loader_err.clear();
    bool was_open = g.open;
    int rc = guarded([&] { open_impl(cfg); return 0; }, err, errlen, false);
    if (rc && !was_open) {
        try {
            teardown();
        } catch (...) {
            g = Bridge{};
        }
    }
    return rc;
}

int lsfg_bridge_generate(const uint8_t *frame, uint32_t pitch, uint8_t *const *out, uint32_t out_pitch) {
    return guarded([&] { return generate_impl(frame, pitch, out, out_pitch); }, nullptr, 0, true);
}

void lsfg_bridge_reset(void) {
    g.primed = false;
}

int lsfg_bridge_save_pipeline_cache(void) {
    if (!g.open)
        return LSFG_E_STATE;
    return save_pipeline_cache();
}

void lsfg_bridge_close(void) {
    try {
        teardown();
    } catch (...) {
        g = Bridge{};
    }
}

int lsfg_bridge_get_info(LsfgBridgeInfo *info) {
    if (!g.open || !info)
        return LSFG_E_STATE;
    *info = g.info;
    return 0;
}

uint32_t lsfg_bridge_last_gen_us(void) {
    return g.lastGenUs;
}

int lsfg_bridge_probe(char *desc, size_t desclen) {
    if (g.open) {
        if (desc && desclen)
            snprintf(desc, desclen, "%s", g.info.device);
        return 0;
    }
    VkInstance inst = VK_NULL_HANDLE;
    int rc = guarded([&] {
        load_vulkan();
        uint32_t api = 0;
        inst = create_instance(&api);
        Caps c;
        pick_device(inst, api, &c);
        std::string d = describe(c);
        if (!c.notes.empty())
            d += "; without " + c.notes;
        if (desc && desclen)
            snprintf(desc, desclen, "%s", d.c_str());
        return 0;
    }, desc, desclen, false);
    if (inst)
        vkDestroyInstance(inst, nullptr);
    return rc;
}

int lsfg_bridge_bench(const LsfgConfig *cfg, uint32_t frames, LsfgBench *out, char *err, size_t errlen) {
    if (!out || frames == 0)
        return LSFG_E_ARG;
    *out = LsfgBench{};
    int rc = lsfg_bridge_open(cfg, err, errlen);
    if (rc)
        return rc;
    out->info = g.info;
    out->frames = frames;
    rc = guarded([&] {
        // Two different inputs, made on the GPU: no upload in the timed loop.
        begin(g.cbUp);
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        for (int i = 0; i < 2; i++) {
            VkClearColorValue c{};
            c.float32[0] = i ? 0.8f : 0.2f;
            c.float32[1] = 0.5f;
            c.float32[2] = i ? 0.1f : 0.9f;
            c.float32[3] = 1.0f;
            image_barrier(g.cbUp, g.in[i].img, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            vkCmdClearColorImage(g.cbUp, g.in[i].img, VK_IMAGE_LAYOUT_GENERAL, &c, 1, &range);
            image_barrier(g.cbUp, g.in[i].img, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        }
        submit(g.cbUp, g.fence);
        wait_fence();
        double sum = 0, mn = 1e30, mx = 0;
        for (uint32_t i = 0; i < frames + 2; i++) {
            auto t0 = std::chrono::steady_clock::now();
            present();
            drain();
            double ms = ms_since(t0);
            if (i < 2)
                continue;   // warm-up
            sum += ms;
            mn = std::min(mn, ms);
            mx = std::max(mx, ms);
        }
        out->gen_ms_avg = sum / frames;
        out->gen_ms_min = mn;
        out->gen_ms_max = mx;

        // Round trips with a moving square, through the public path.
        const uint32_t w = g.width, h = g.height, gens = g.gens;
        std::vector<uint8_t> frame((size_t)w * h * 4), outs((size_t)w * h * 4 * gens);
        std::vector<uint8_t *> outp(gens);
        for (uint32_t i = 0; i < gens; i++)
            outp[i] = outs.data() + (size_t)i * w * h * 4;
        g.primed = false;
        double rt = 0;
        uint32_t n = 0;
        for (uint32_t i = 0; i < frames + 1; i++) {
            std::fill(frame.begin(), frame.end(), 0x20);
            uint32_t s = std::max(8u, h / 8), x0 = (i * 8) % (w > s ? w - s : 1), y0 = h / 2 - s / 2;
            for (uint32_t y = y0; y < y0 + s && y < h; y++)
                std::memset(frame.data() + ((size_t)y * w + x0) * 4, 0xe0, (size_t)s * 4);
            auto t0 = std::chrono::steady_clock::now();
            int r = generate_impl(frame.data(), w * 4, outp.data(), w * 4);
            if (r == LSFG_OK) {
                rt += ms_since(t0);
                n++;
            }
        }
        out->roundtrip_ms_avg = n ? rt / n : 0;
        return 0;
    }, err, errlen, true);
    lsfg_bridge_close();
    return rc;
}

const char *lsfg_bridge_strerror(int err) {
    switch (err) {
    case LSFG_OK: return "ok";
    case LSFG_PRIMED: return "history primed";
    case LSFG_E_NOLOADER: return "no Vulkan loader";
    case LSFG_E_NODEVICE: return "no Vulkan 1.1 device";
    case LSFG_E_FEATURE: return "missing Vulkan feature";
    case LSFG_E_SHADERS: return "shader cache missing or invalid";
    case LSFG_E_MODULE: return "shader module rejected";
    case LSFG_E_VULKAN: return "Vulkan error";
    case LSFG_E_DEVICE_LOST: return "device lost";
    case LSFG_E_TIMEOUT: return "generation timed out";
    case LSFG_E_STATE: return "bridge not open (or already open)";
    case LSFG_E_ARG: return "invalid argument";
    case LSFG_E_NOMEM: return "out of memory";
    case LSFG_E_INTERNAL: return "internal error";
    default: return "unknown error";
    }
}

const char *lsfg_bridge_last_error(void) {
    return g_err.c_str();
}

} // extern "C"
