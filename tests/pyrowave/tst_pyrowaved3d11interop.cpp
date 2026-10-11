#include <vulkan/vulkan_core.h>
#include "../../pyrowave/pyrowave/pyrowave_d3d11_interop.hpp"
#include "../vrr/assertions.h"
#include <iostream>

int main()
{
    for (const bool hdr : {false, true}) for (const bool chroma444 : {false, true}) {
        (void)chroma444; // Both chroma layouts import the same per-plane format.
        const VkFormat format = hdr ? VK_FORMAT_R16_UNORM : VK_FORMAT_R8_UNORM;
        bool imageSupported = true, imageImportable = true, fenceImportable = true;
        int imageCalls = 0, fenceCalls = 0;
        const auto image = [&](VkFormat f, VkImageType type, VkImageTiling tiling,
            VkImageUsageFlags usage, VkImageCreateFlags flags,
            const VkPhysicalDeviceExternalImageFormatInfo* external, VkImageFormatProperties2* properties) {
            ++imageCalls;
            assert(f == format && type == VK_IMAGE_TYPE_2D && tiling == VK_IMAGE_TILING_OPTIMAL);
            assert(usage == (VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT) && flags == 0);
            // NT-only Intel capability shape: KMT/D3D12 resource image queries
            // would fail. The production predicate must never request them.
            assert(external->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT);
            assert(external->pNext == nullptr);
            auto* imported = static_cast<VkExternalImageFormatProperties*>(properties->pNext);
            imported->externalMemoryProperties.externalMemoryFeatures =
                imageImportable ? VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT : 0;
            return imageSupported;
        };
        const auto fence = [&](const VkPhysicalDeviceExternalSemaphoreInfo* info,
                               VkExternalSemaphoreProperties* properties) {
            ++fenceCalls;
            assert(info->handleType == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT);
            assert(info->pNext == nullptr); // Legacy fence query, not opaque NT.
            properties->externalSemaphoreFeatures =
                fenceImportable ? VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT : 0;
        };
        const auto check = [&] { return PyroWave::checkD3D11Interop(format, true, image, fence); };
        assert(check() == nullptr && imageCalls == 1 && fenceCalls == 1);
        imageSupported = false;
        assert(check() != nullptr && fenceCalls == 1);
        imageSupported = true; imageImportable = false;
        assert(check() != nullptr && fenceCalls == 1);
        imageImportable = true; fenceImportable = false;
        assert(check() != nullptr && fenceCalls == 2);
        const int before = imageCalls;
        assert(PyroWave::checkD3D11Interop(format, false, image, fence) != nullptr);
        assert(PyroWave::checkD3D11Interop(VK_FORMAT_UNDEFINED, true, image, fence) != nullptr);
        assert(imageCalls == before); // Missing extensions/malformed format fail closed.
    }
    std::cout << "D3D11 NT-only R8/R16, SDR/HDR 420/444, import failures and legacy fence checks passed\n";
}
