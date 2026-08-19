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

private:
    void CreateDescriptors();

private:
    VkDevice device;

    // 4-byte placeholder that every binding points at until real data is
    // uploaded (same trick as Q2RTX null_buffer).
    Buffer nullBuffer;

    // Real backing for the tone mapping buffer (histogram accumulator +
    // tone curve), written by the tone mapping shaders.
    Buffer toneMappingBuffer;

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
