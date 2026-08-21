// Q2RTX-convention vertex buffer descriptor set (VertexBufferQ2).
//
// Part of the Q2RTX binding layer (see PORTING.md, stage S2b): prepares
// descriptor set 3 (VERTEX_BUFFER_DESC_SET_IDX), which the Q2RTX shaders
// use for primitive / position / light / tonemapping / sun-color buffers.
// The layout mirrors Q2RTX vertex_buffer.c exactly. All bindings point at
// a null placeholder buffer for now; real geometry data comes with the
// geometry port. The set is NOT bound to any pipeline yet - it is prepared
// so Q2RTX shaders can be swapped in one module at a time.

#pragma once

#include "Buffer.h"

namespace vkpt
{

class VertexBufferQ2
{
public:
    explicit VertexBufferQ2(VkDevice device, std::shared_ptr<MemoryAllocator> allocator);
    ~VertexBufferQ2();

    VertexBufferQ2(const VertexBufferQ2 &other) = delete;
    VertexBufferQ2(VertexBufferQ2 &&other) noexcept = delete;
    VertexBufferQ2 &operator=(const VertexBufferQ2 &other) = delete;
    VertexBufferQ2 &operator=(VertexBufferQ2 &&other) noexcept = delete;

    VkDescriptorSet GetDescSet() const;
    VkDescriptorSetLayout GetDescSetLayout() const;

    // Point the world bindings at real geometry: binding 0 element
    // VERTEX_BUFFER_WORLD gets the primitive array, binding 1
    // (POSITION_BUFFER_BINDING_IDX) gets the BLAS source positions.
    // Called by GeometryQ2 after the static level geometry is uploaded.
    void SetWorldBufferInfo(const VkDescriptorBufferInfo &primInfo,
                            const VkDescriptorBufferInfo &posInfo);

    // Stage G6: overwrite the material_table entries starting at material
    // index 2 (0 = empty, 1 = default white). entries has count * 6 uints
    // packed like Q2RTX material_table (get_material_info format). Called
    // by GeometryQ2::SubmitStatic after the static level uploads.
    void SetQ2Materials(const uint32_t *entries, uint32_t count);

private:
    void CreateDescriptors();
    void FillLightBuffer();
    void FillSunColor();

private:
    VkDevice device;

    // 4-byte placeholder that every binding points at until real data is
    // uploaded (same trick as Q2RTX null_buffer).
    Buffer nullBuffer;

    // Real backing for the tone mapping buffer (histogram accumulator +
    // tone curve), written by the tone mapping shaders.
    Buffer toneMappingBuffer;

    // Real backing for the Q2RTX LightBuffer (material table + light
    // lists), host-visible so the default material can be written from CPU.
    Buffer lightBuffer;

    // Real backing for the readback buffer (a few pixels read back to the
    // CPU every frame), written by asvgf_taau.comp.
    Buffer readbackBuffer;

    // Real backing for the sun/sky color buffer, written by
    // sky_buffer_resolve.comp and bound as both storage and UBO.
    Buffer sunColorBuffer;

    VkDescriptorPool      descPool;
    VkDescriptorSetLayout descSetLayout;
    VkDescriptorSet       descSet;
};

}
