#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_METAL_EXT
#import <Metal/Metal.h>
#include "pyrowavemetal.h"
#include <SDL.h>
#include <dlfcn.h>
#include <array>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {
constexpr size_t kMaxSurfaces = 8;
constexpr uint32_t kFrameMagic = 0x4d505952; // MPYR
#define DEVICE_FUNCTIONS(X) \
    X(DestroyDevice) X(GetDeviceQueue) X(CreateImage) X(DestroyImage) \
    X(GetImageMemoryRequirements) X(AllocateMemory) X(FreeMemory) X(BindImageMemory) \
    X(CreateSemaphore) X(DestroySemaphore) X(WaitSemaphores) X(GetSemaphoreCounterValue) \
    X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) \
    X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdPipelineBarrier) \
    X(QueueSubmit) X(DeviceWaitIdle) X(ExportMetalObjectsEXT)
}

struct PyroWaveMetalPool::State {
    void* loader = nullptr;
    PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;
    PFN_vkGetDeviceProcAddr getDeviceProcAddr = nullptr;
    PFN_vkDestroyInstance destroyInstance = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkPhysicalDeviceMemoryProperties memoryProperties = {};
    VkApplicationInfo appInfo = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    VkExportMetalObjectCreateInfoEXT exportDevice = { VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT };
    VkInstanceCreateInfo instanceInfo = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    VkPhysicalDeviceVulkan12Features features12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceFeatures2 features2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    VkPhysicalDeviceSubgroupSizeControlFeatures subgroup = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES };
    VkPhysicalDeviceSynchronization2Features synchronization2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES };
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    VkDeviceCreateInfo deviceInfo = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    std::vector<const char*> extensions;
    VkSemaphore ready = VK_NULL_HANDLE, done = VK_NULL_HANDLE;
    uint64_t readyValue = 0, doneValue = 0;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    std::mutex surfaceLock, queueLock;
    struct Surface {
        bool busy = false;
        int width = 0, height = 0;
        bool chroma444 = false, sixteenBit = false;
        std::array<VkImage, 3> images {};
        std::array<VkDeviceMemory, 3> memory {};
        std::array<id<MTLTexture>, 3> textures {};
        uint64_t readyValue = 0;
    };
    std::array<Surface, kMaxSurfaces> surfaces;
#define DECLARE_FUNCTION(name) PFN_vk##name name = nullptr;
    DEVICE_FUNCTIONS(DECLARE_FUNCTION)
#undef DECLARE_FUNCTION

    ~State()
    {
        if (device) {
            if (DeviceWaitIdle) DeviceWaitIdle(device);
            for (auto& surface : surfaces) destroySurface(surface);
            if (ready) DestroySemaphore(device, ready, nullptr);
            if (done) DestroySemaphore(device, done, nullptr);
            if (commandPool) DestroyCommandPool(device, commandPool, nullptr);
            DestroyDevice(device, nullptr);
        }
        if (instance && destroyInstance) destroyInstance(instance, nullptr);
        if (loader) dlclose(loader);
    }

    void destroySurface(Surface& surface)
    {
        for (size_t plane = 0; plane < 3; ++plane) {
            [surface.textures[plane] release];
            surface.textures[plane] = nil;
            if (surface.images[plane]) DestroyImage(device, surface.images[plane], nullptr);
            if (surface.memory[plane]) FreeMemory(device, surface.memory[plane], nullptr);
            surface.images[plane] = VK_NULL_HANDLE;
            surface.memory[plane] = VK_NULL_HANDLE;
        }
        surface.width = surface.height = 0;
    }

    bool createSurface(Surface& surface, int width, int height, bool chroma444, bool sixteenBit)
    {
        // A session's geometry is immutable: cached decoder views must never
        // outlive an image replacement.
        if (surface.width) return surface.width == width && surface.height == height &&
            surface.chroma444 == chroma444 && surface.sixteenBit == sixteenBit;
        const VkFormat format = sixteenBit ? VK_FORMAT_R16_UNORM : VK_FORMAT_R8_UNORM;
        std::array<VkImageMemoryBarrier, 3> barriers {};
        for (size_t plane = 0; plane < 3; ++plane) {
            const bool subsampled = plane && !chroma444;
            VkExportMetalObjectCreateInfoEXT exportInfo { VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT };
            exportInfo.exportObjectType = VK_EXPORT_METAL_OBJECT_TYPE_METAL_TEXTURE_BIT_EXT;
            VkImageCreateInfo imageInfo { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &exportInfo };
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.format = format;
            imageInfo.extent = { uint32_t(subsampled ? width / 2 : width), uint32_t(subsampled ? height / 2 : height), 1 };
            imageInfo.mipLevels = imageInfo.arrayLayers = 1;
            imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (CreateImage(device, &imageInfo, nullptr, &surface.images[plane]) != VK_SUCCESS) return false;
            VkMemoryRequirements requirements {};
            GetImageMemoryRequirements(device, surface.images[plane], &requirements);
            uint32_t memoryType = UINT32_MAX;
            for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
                if ((requirements.memoryTypeBits & (1u << i)) &&
                    (memoryProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                    memoryType = i;
                    break;
                }
            }
            if (memoryType == UINT32_MAX) return false;
            VkMemoryAllocateInfo allocation { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = memoryType;
            if (AllocateMemory(device, &allocation, nullptr, &surface.memory[plane]) != VK_SUCCESS ||
                BindImageMemory(device, surface.images[plane], surface.memory[plane], 0) != VK_SUCCESS) return false;
            VkExportMetalTextureInfoEXT textureInfo { VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT };
            textureInfo.image = surface.images[plane];
            textureInfo.plane = VK_IMAGE_ASPECT_COLOR_BIT;
            VkExportMetalObjectsInfoEXT objects { VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT, &textureInfo };
            ExportMetalObjectsEXT(device, &objects);
            surface.textures[plane] = [textureInfo.mtlTexture retain];
            if (!surface.textures[plane]) return false;
            auto& barrier = barriers[plane];
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = surface.images[plane];
            barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        }
        VkCommandBufferAllocateInfo allocate { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        allocate.commandPool = commandPool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        VkCommandBuffer command = VK_NULL_HANDLE;
        if (AllocateCommandBuffers(device, &allocate, &command) != VK_SUCCESS) return false;
        VkCommandBufferBeginInfo begin { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (BeginCommandBuffer(command, &begin) != VK_SUCCESS) return false;
        CmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           0, 0, nullptr, 0, nullptr, uint32_t(barriers.size()), barriers.data());
        if (EndCommandBuffer(command) != VK_SUCCESS) return false;
        const uint64_t value = readyValue + 1;
        VkTimelineSemaphoreSubmitInfo timeline { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
        timeline.signalSemaphoreValueCount = 1;
        timeline.pSignalSemaphoreValues = &value;
        VkSubmitInfo submit { VK_STRUCTURE_TYPE_SUBMIT_INFO, &timeline };
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &ready;
        {
            std::lock_guard<std::mutex> lock(queueLock);
            if (QueueSubmit(queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS) return false;
        }
        readyValue = value;
        surface.readyValue = value;
        surface.width = width; surface.height = height;
        surface.chroma444 = chroma444; surface.sixteenBit = sixteenBit;
        return true;
    }
};

struct PyroWaveMetalPool::FrameRef {
    uint32_t magic = kFrameMagic;
    std::shared_ptr<State> state;
    int index;
    uint64_t doneValue;
};

PyroWaveMetalPool::PyroWaveMetalPool() = default;
PyroWaveMetalPool::~PyroWaveMetalPool() = default;

bool PyroWaveMetalPool::initialize(void* metalDevice)
{
    if (m_State) return true;
    id<MTLDevice> selectedMetal = (id<MTLDevice>)metalDevice;
    if (!selectedMetal) return false;
    auto state = std::make_shared<State>();
    const char* overrideLibrary = SDL_getenv("GRANITE_VULKAN_LIBRARY");
    state->loader = dlopen(overrideLibrary ? overrideLibrary : "libMoltenVK.dylib", RTLD_NOW | RTLD_LOCAL);
    if (!state->loader) {
        char* base = SDL_GetBasePath();
        if (base) {
            const std::string path = std::string(base) + "../Frameworks/libMoltenVK.dylib";
            state->loader = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
            SDL_free(base);
        }
    }
    if (!state->loader) return false;
    state->getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(state->loader, "vkGetInstanceProcAddr"));
    if (!state->getInstanceProcAddr) return false;
    auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(state->getInstanceProcAddr(nullptr, "vkCreateInstance"));
    state->appInfo.apiVersion = VK_API_VERSION_1_2;
    state->appInfo.pApplicationName = "Moonlight PyroWave Metal";
    state->exportDevice.exportObjectType = VK_EXPORT_METAL_OBJECT_TYPE_METAL_DEVICE_BIT_EXT;
    state->instanceInfo.pNext = &state->exportDevice;
    state->instanceInfo.pApplicationInfo = &state->appInfo;
    if (!createInstance || createInstance(&state->instanceInfo, nullptr, &state->instance) != VK_SUCCESS) return false;
#define INSTANCE_FUNCTION(name) auto name = reinterpret_cast<PFN_vk##name>(state->getInstanceProcAddr(state->instance, "vk" #name))
    INSTANCE_FUNCTION(EnumeratePhysicalDevices);
    INSTANCE_FUNCTION(GetPhysicalDeviceFeatures2);
    INSTANCE_FUNCTION(GetPhysicalDeviceQueueFamilyProperties);
    INSTANCE_FUNCTION(GetPhysicalDeviceMemoryProperties);
    INSTANCE_FUNCTION(EnumerateDeviceExtensionProperties);
    INSTANCE_FUNCTION(CreateDevice);
    state->destroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(state->getInstanceProcAddr(state->instance, "vkDestroyInstance"));
    state->getDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(state->getInstanceProcAddr(state->instance, "vkGetDeviceProcAddr"));
    uint32_t count = 0;
    if (EnumeratePhysicalDevices(state->instance, &count, nullptr) != VK_SUCCESS) return false;
    std::vector<VkPhysicalDevice> devices(count);
    if (EnumeratePhysicalDevices(state->instance, &count, devices.data()) != VK_SUCCESS) return false;
    for (auto physical : devices) {
        uint32_t extensionCount = 0;
        if (EnumerateDeviceExtensionProperties(physical, nullptr, &extensionCount, nullptr) != VK_SUCCESS) continue;
        std::vector<VkExtensionProperties> available(extensionCount);
        if (EnumerateDeviceExtensionProperties(physical, nullptr, &extensionCount, available.data()) != VK_SUCCESS) continue;
        state->extensions.clear();
        bool metalObjects = false, subgroupControl = false, sync2 = false;
        for (const auto& extension : available) {
            if (!strcmp(extension.extensionName, VK_EXT_METAL_OBJECTS_EXTENSION_NAME)) {
                state->extensions.push_back(VK_EXT_METAL_OBJECTS_EXTENSION_NAME); metalObjects = true;
            }
            if (!strcmp(extension.extensionName, VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME)) {
                state->extensions.push_back(VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME); subgroupControl = true;
            }
            if (!strcmp(extension.extensionName, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME)) {
                state->extensions.push_back(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME); sync2 = true;
            }
            if (!strcmp(extension.extensionName, "VK_KHR_portability_subset"))
                state->extensions.push_back("VK_KHR_portability_subset");
        }
        if (!metalObjects || !subgroupControl || !sync2) continue;
        VkPhysicalDeviceFeatures2 supported { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        state->features12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
        state->subgroup = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES };
        state->synchronization2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES };
        supported.pNext = &state->features12;
        state->features12.pNext = &state->subgroup;
        state->subgroup.pNext = &state->synchronization2;
        GetPhysicalDeviceFeatures2(physical, &supported);
        if (!state->features12.timelineSemaphore || !state->subgroup.subgroupSizeControl ||
            !state->subgroup.computeFullSubgroups || !state->synchronization2.synchronization2) continue;
        // Enable supported core features. Granite's borrowed-device context
        // reads this exact persistent create-info chain.
        state->features2.features = supported.features;
        state->features2.pNext = &state->features12;
        uint32_t familyCount = 0;
        GetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        GetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, families.data());
        uint32_t family = UINT32_MAX;
        for (uint32_t i = 0; i < familyCount; ++i)
            if ((families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
                (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) { family = i; break; }
        if (family == UINT32_MAX) continue;
        state->queueFamily = family;
        state->queueInfo.queueFamilyIndex = family;
        state->queueInfo.queueCount = 1;
        state->queueInfo.pQueuePriorities = &state->priority;
        state->deviceInfo.pNext = &state->features2;
        state->deviceInfo.queueCreateInfoCount = 1;
        state->deviceInfo.pQueueCreateInfos = &state->queueInfo;
        state->deviceInfo.enabledExtensionCount = uint32_t(state->extensions.size());
        state->deviceInfo.ppEnabledExtensionNames = state->extensions.data();
        if (CreateDevice(physical, &state->deviceInfo, nullptr, &state->device) != VK_SUCCESS) continue;
#define LOAD_FUNCTION(name) state->name = reinterpret_cast<PFN_vk##name>(state->getDeviceProcAddr(state->device, "vk" #name));
        DEVICE_FUNCTIONS(LOAD_FUNCTION)
#undef LOAD_FUNCTION
        bool completeDispatch = true;
#define CHECK_FUNCTION(name) completeDispatch = completeDispatch && state->name != nullptr;
        DEVICE_FUNCTIONS(CHECK_FUNCTION)
#undef CHECK_FUNCTION
        if (!completeDispatch) {
            if (state->DestroyDevice) state->DestroyDevice(state->device, nullptr);
            state->device = VK_NULL_HANDLE;
            continue;
        }
        VkExportMetalDeviceInfoEXT metalInfo { VK_STRUCTURE_TYPE_EXPORT_METAL_DEVICE_INFO_EXT };
        VkExportMetalObjectsInfoEXT objects { VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT, &metalInfo };
        if (state->ExportMetalObjectsEXT) state->ExportMetalObjectsEXT(state->device, &objects);
        if (!metalInfo.mtlDevice || metalInfo.mtlDevice.registryID != selectedMetal.registryID) {
            state->DestroyDevice(state->device, nullptr); state->device = VK_NULL_HANDLE;
            continue;
        }
        state->physicalDevice = physical;
        GetPhysicalDeviceMemoryProperties(physical, &state->memoryProperties);
        state->GetDeviceQueue(state->device, family, 0, &state->queue);
        break;
    }
#undef INSTANCE_FUNCTION
    if (!state->device || !state->WaitSemaphores || !state->GetSemaphoreCounterValue) return false;
    VkSemaphoreTypeCreateInfo type { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo semInfo { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type };
    if (state->CreateSemaphore(state->device, &semInfo, nullptr, &state->ready) != VK_SUCCESS ||
        state->CreateSemaphore(state->device, &semInfo, nullptr, &state->done) != VK_SUCCESS) return false;
    VkCommandPoolCreateInfo commands { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    commands.queueFamilyIndex = state->queueFamily;
    if (state->CreateCommandPool(state->device, &commands, nullptr, &state->commandPool) != VK_SUCCESS) return false;
    m_State = std::move(state);
    return true;
}

void PyroWaveMetalPool::lockQueues(void* opaque) { static_cast<State*>(opaque)->queueLock.lock(); }
void PyroWaveMetalPool::unlockQueues(void* opaque) { static_cast<State*>(opaque)->queueLock.unlock(); }

bool PyroWaveMetalPool::pyroWaveVulkanDevice(PyroWaveVulkanDevice& device)
{
    if (!m_State) return false;
    device = {};
    device.getInstanceProcAddr = m_State->getInstanceProcAddr;
    device.instance = m_State->instance; device.physicalDevice = m_State->physicalDevice;
    device.device = m_State->device;
    device.instanceInfo = &m_State->instanceInfo; device.deviceInfo = &m_State->deviceInfo;
    device.lockQueues = lockQueues; device.unlockQueues = unlockQueues;
    device.userdata = m_State.get();
    return true;
}

bool PyroWaveMetalPool::holdPyroWaveSurface(int width, int height, bool chroma444, bool sixteenBit,
                                           PyroWaveVulkanSurface& output)
{
    if (!m_State) return false;
    std::lock_guard<std::mutex> lock(m_State->surfaceLock);
    for (size_t i = 0; i < kMaxSurfaces; ++i) {
        auto& target = m_State->surfaces[i];
        if (target.busy) continue;
        // Do not destroy an image with decoder-cached views on a geometry
        // mismatch. A different stream format must use a new pool.
        if (target.width && (target.width != width || target.height != height ||
            target.chroma444 != chroma444 || target.sixteenBit != sixteenBit)) return false;
        if (!m_State->createSurface(target, width, height, chroma444, sixteenBit)) {
            m_State->destroySurface(target);
            return false;
        }
        target.busy = true;
        output = {};
        output.index = int(i);
        output.format = sixteenBit ? VK_FORMAT_R16_UNORM : VK_FORMAT_R8_UNORM;
        output.ready = m_State->ready; output.readyValue = target.readyValue;
        output.done = m_State->done; output.doneValue = ++m_State->doneValue;
        for (size_t plane = 0; plane < 3; ++plane) {
            output.images[plane] = target.images[plane];
            const bool subsampled = plane && !chroma444;
            output.widths[plane] = uint32_t(subsampled ? width / 2 : width);
            output.heights[plane] = uint32_t(subsampled ? height / 2 : height);
        }
        return true;
    }
    return false;
}

void PyroWaveMetalPool::freeFrameRef(void*, uint8_t* data)
{
    auto ref = reinterpret_cast<FrameRef*>(data);
    {
        std::lock_guard<std::mutex> lock(ref->state->surfaceLock);
        ref->state->surfaces[size_t(ref->index)].busy = false;
    }
    delete ref;
}

bool PyroWaveMetalPool::releasePyroWaveSurface(const PyroWaveVulkanSurface& surface, bool decoded, AVFrame* frame)
{
    if (!m_State || surface.index < 0 || size_t(surface.index) >= kMaxSurfaces) return false;
    if (!decoded) {
        std::lock_guard<std::mutex> lock(m_State->surfaceLock);
        m_State->surfaces[size_t(surface.index)].busy = false;
        return false;
    }
    auto ref = new FrameRef { kFrameMagic, m_State, surface.index, surface.doneValue };
    frame->buf[0] = av_buffer_create(reinterpret_cast<uint8_t*>(ref), sizeof(*ref), freeFrameRef, nullptr, 0);
    if (!frame->buf[0]) { freeFrameRef(nullptr, reinterpret_cast<uint8_t*>(ref)); return false; }
    const auto& target = m_State->surfaces[size_t(surface.index)];
    frame->width = target.width; frame->height = target.height;
    frame->format = target.sixteenBit ?
        (target.chroma444 ? AV_PIX_FMT_YUV444P16 : AV_PIX_FMT_YUV420P16) :
        (target.chroma444 ? AV_PIX_FMT_YUV444P : AV_PIX_FMT_YUV420P);
    return true;
}

const PyroWaveMetalPool::FrameRef* PyroWaveMetalPool::frameRef(const AVFrame* frame) const
{
    if (!frame || !frame->buf[0] || frame->buf[0]->size != sizeof(FrameRef)) return nullptr;
    auto ref = reinterpret_cast<const FrameRef*>(frame->buf[0]->data);
    return ref->magic == kFrameMagic && ref->state == m_State ? ref : nullptr;
}

bool PyroWaveMetalPool::ownsFrame(const AVFrame* frame) const { return frameRef(frame) != nullptr; }

bool PyroWaveMetalPool::mapFrame(const AVFrame* frame, void* textures[3]) const
{
    const auto ref = frameRef(frame);
    if (!ref) return false;
    const auto& surface = ref->state->surfaces[size_t(ref->index)];
    for (size_t plane = 0; plane < 3; ++plane) textures[plane] = (void*)surface.textures[plane];
    return true;
}

VkResult PyroWaveMetalPool::waitForFrame(const AVFrame* frame, uint64_t timeoutNs) const
{
    const auto ref = frameRef(frame);
    if (!ref) return VK_ERROR_INITIALIZATION_FAILED;
    VkSemaphoreWaitInfo wait { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
    wait.semaphoreCount = 1;
    wait.pSemaphores = &ref->state->done; wait.pValues = &ref->doneValue;
    return ref->state->WaitSemaphores(ref->state->device, &wait, timeoutNs);
}
