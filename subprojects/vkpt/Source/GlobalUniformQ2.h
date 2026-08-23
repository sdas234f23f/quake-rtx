// Q2RTX-convention uniform + instance buffer (GlobalUniformQ2).
//
// Part of the Q2RTX binding layer (see PORTING.md, stage S2b): fills the
// Q2RTX `QVKUniformBuffer_t` (q2rtx-shaders/global_ubo.h, C branch) with the
// same per-frame data the legacy pipeline uses and uploads it through an
// AutoBuffer. Binding 1 is the InstanceBuffer SSBO in the same buffer, like
// in Q2RTX uniform_buffer.c; its data is zeroed until the geometry port
// starts feeding real instances. The descriptor set is NOT bound to any
// pipeline yet - it is prepared so Q2RTX shaders can be swapped in one
// module at a time.

#pragma once

#include "AutoBuffer.h"

#include <vector>

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

    // Sets the CPU-side copy of the InstanceBuffer SSBO that Upload() writes
    // into the buffer each frame. Called by ASManagerQ2 after the geometry /
    // acceleration structures are built.
    void SetInstanceBuffer(const void *pData, size_t size);

    // Stage G6c: how many entries LightManagerQ2 wrote into light_polys.
    // Goes into ubo.num_static_lights, which sizes the light stats
    // addressing and gates polygonal light sampling.
    void SetStaticLightCount(uint32_t count);

    // Stage G6c: point lights for ubo.dyn_light_data / num_dyn_lights.
    // data holds count packed DynLightData entries.
    void SetDynLights(const void *data, uint32_t count);

    // The texture index of the tiling water normal map (TextureManager's
    // WaterNormal_n.ktx2). Goes into ubo.water_normal_texture, which the
    // water surface shaders sample for wave animation.
    void SetWaterNormalTextureIndex(uint32_t index);

    VkDescriptorSet GetDescSet() const;
    VkDescriptorSetLayout GetDescSetLayout() const;

private:
    void CreateDescriptors();

private:
    VkDevice device;

    std::shared_ptr<AutoBuffer> buffer;
    std::vector<uint8_t> instanceBufferCpu;
    uint32_t staticLightCount = 0;
    std::vector<uint8_t> dynLightsCpu;
    uint32_t dynLightCount = 0;
    uint32_t waterNormalTextureIndex = 0;

    VkDescriptorPool      descPool;
    VkDescriptorSetLayout descSetLayout;
    VkDescriptorSet       descSet;
};

}
