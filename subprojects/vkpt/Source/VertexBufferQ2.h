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

    // Returns descSets[activeFrameIndex] (see SetActiveFrame / stage G1b):
    // every binding except the VERTEX_BUFFER_INSTANCED array element is
    // identical across ring slots, so existing no-arg callers (PathTracerQ2,
    // SkyBufferResolveQ2, AsvgfTaaQ2, ToneMappingQ2, LightManagerQ2) need no
    // changes.
    VkDescriptorSet GetDescSet() const;
    VkDescriptorSetLayout GetDescSetLayout() const;

    // Point the world bindings at real geometry: binding 0 element
    // VERTEX_BUFFER_WORLD gets the primitive array, binding 1
    // (POSITION_BUFFER_BINDING_IDX) gets the BLAS source positions. Written
    // identically into every ring slot's descriptor set (the world buffer
    // does not change per frame). Called by GeometryQ2 after the static
    // level geometry is uploaded.
    void SetWorldBufferInfo(const VkDescriptorBufferInfo &primInfo,
                            const VkDescriptorBufferInfo &posInfo);

    // Stage G1b: select which ring slot GetDescSet() returns for the rest of
    // this frame. Called once per frame (VulkanDevice::BeginFrame).
    void SetActiveFrame(uint32_t frameIndex);

    // Stage G1b: point binding 0 element VERTEX_BUFFER_INSTANCED of
    // descSets[frameIndex] at this frame's dynamic aggregate buffer. Only
    // that one ring slot's descriptor set is touched, so it never races a
    // command buffer from a different frameIndex that may still be
    // executing. Called by GeometryQ2::SubmitDynamic every frame that has
    // dynamic geometry.
    void SetDynamicBufferInfo(uint32_t frameIndex, const VkDescriptorBufferInfo &primInfo);

    // Stage G6: overwrite count material_table entries beginning at
    // material index 2 + firstEntry (0 = empty, 1 = default white). entries
    // has count * 6 uints packed like Q2RTX material_table.
    void SetQ2Materials(const uint32_t *entries, uint32_t firstEntry,
                        uint32_t count);

    // Stage G6c: Q2RTX light data, filled by LightManagerQ2 every frame.

    // Emissive triangles as Q2RTX LightPolygon entries: LIGHT_POLY_VEC4S
    // vec4 per light (positions.xyz + color in the .w lanes, then the two
    // light style scales). Clamped to MAX_LIGHT_POLYS.
    void SetLightPolys(const float *vec4Data, uint32_t count);

    // Per-cluster light lists: prefix-sum offsets (numClusters + 1 entries)
    // and the concatenated light-poly indices.
    void SetClusterLightLists(uint32_t numClusters, const uint32_t *offsets,
                              const uint32_t *indices, uint32_t totalCount);

    // Per-cluster sample counts for one history slot. sample_polygonal_lights
    // takes the light count from here, NOT from the offsets array, so this
    // has to be written or no light is ever sampled.
    void SetLightCounts(uint32_t historyIndex, const uint32_t *counts, uint32_t numClusters);

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

    // Real backing for the per-cluster light counts (LIGHT_COUNT_HISTORY
    // slots; the shader picks one by frame number from the RNG seed).
    // Sized for MAX_LIGHT_LISTS clusters so it never needs resizing.
    // The literal 3 is asserted against LIGHT_COUNT_HISTORY in the .cpp.
    Buffer lightCountsHistory[3];

    VkDescriptorPool      descPool;
    VkDescriptorSetLayout descSetLayout;
    // Stage G1b: one descriptor set per frame in flight so binding 0
    // element VERTEX_BUFFER_INSTANCED can be repointed at that frame's
    // dynamic aggregate buffer without racing a command buffer from a
    // different ring slot that may still be executing (same pattern as
    // FramebuffersQ2::descSets / activeFrameIndex).
    VkDescriptorSet       descSets[MAX_FRAMES_IN_FLIGHT];
    uint32_t              activeFrameIndex;
};

}
