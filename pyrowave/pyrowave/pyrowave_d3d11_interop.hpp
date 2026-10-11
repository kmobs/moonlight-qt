#pragma once

// Query only the image/fence families used by Moonlight's D3D11 decoder.
// Callbacks keep the production predicate testable without a GPU or OS handles.
namespace PyroWave {
template<class ImageQuery, class SemaphoreQuery>
const char* checkD3D11Interop(VkFormat format, bool externalAvailable,
                            ImageQuery imageQuery, SemaphoreQuery semaphoreQuery)
{
    if (format != VK_FORMAT_R8_UNORM && format != VK_FORMAT_R16_UNORM)
        return "invalid D3D11 plane format";
    if (!externalAvailable) return "external Win32 memory/semaphore extensions unavailable";
    VkPhysicalDeviceExternalImageFormatInfo external = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
    external.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
    VkExternalImageFormatProperties imported = {
        VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 properties = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &imported};
    if (!imageQuery(format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    0, &external, &properties) ||
        !(imported.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT))
        return format == VK_FORMAT_R8_UNORM ? "R8 D3D11 NT texture import unavailable" :
                                             "R16 D3D11 NT texture import unavailable";
    // D3D11 shared fences use D3D12_FENCE timeline handles. The legacy query
    // intentionally has no semaphore-type pNext, matching the actual import.
    VkPhysicalDeviceExternalSemaphoreInfo fence = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};
    fence.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
    VkExternalSemaphoreProperties semaphore = {VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
    semaphoreQuery(&fence, &semaphore);
    if (!(semaphore.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT))
        return "D3D12-fence timeline import unavailable";
    return nullptr;
}
}
