// Q2RTX-convention uniform buffer (GlobalUniformQ2).
//
// First piece of the Q2RTX binding layer (see PORTING.md, stage S2b): fills
// the Q2RTX `QVKUniformBuffer_t` (q2rtx-shaders/global_ubo.h, C branch) with
// the same per-frame data the legacy pipeline uses and uploads it through an
// AutoBuffer. The descriptor set is NOT bound to any pipeline yet - it is
// prepared so Q2RTX shaders can be swapped in one module at a time.

#pragma once

#include "AutoBuffer.h"

namespace vkpt
{

struct ShGlobalUniform;

class GlobalUniformQ2
{
public:
    explicit GlobalUniformQ2(VkDevice device, std::shared_ptr<MemoryAllocator> allocator);
    ~GlobalUniformQ2();

    GlobalUniformQ2(const GlobalUniformQ2 &other) = delete;
    GlobalUniformQ2(GlobalUniformQ2 &&other) noexcept = delete;
    GlobalUniformQ2 &operator=(const GlobalUniformQ2 &other) = delete;
    GlobalUniformQ2 &operator=(GlobalUniformQ2 &&other) noexcept = delete;

    // Rebuilds the Q2RTX UBO from the legacy uniform data and uploads it.
    void Upload(VkCommandBuffer cmd, uint32_t frameIndex, const ShGlobalUniform *src);

    VkDescriptorSet GetDescSet() const;
    VkDescriptorSetLayout GetDescSetLayout() const;

private:
    void CreateDescriptors();

private:
    VkDevice device;

    std::shared_ptr<AutoBuffer> buffer;

    VkDescriptorPool      descPool;
    VkDescriptorSetLayout descSetLayout;
    VkDescriptorSet       descSet;
};

}
