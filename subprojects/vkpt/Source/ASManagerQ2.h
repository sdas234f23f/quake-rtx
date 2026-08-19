// ASManagerQ2: builds the Q2RTX acceleration structures and instance data
// for the world geometry (stage G2 of the porting plan, see PORTING.md).
//
// Part of the Q2RTX binding layer (Block 2): after GeometryQ2 has filled the
// Q2RTX world buffer (primitives + positions), this module builds one BLAS
// over the world positions (non-indexed, stride 12) and one TLAS with a
// single opaque instance. It also fills the InstanceBuffer SSBO (model
// instances + TLAS instance bookkeeping) and creates descriptor set 0
// (TLAS array + texel buffer placeholders) that the Q2RTX rgen shaders
// expect. Nothing is dispatched yet - G3 traces primary rays against it.

#pragma once

#include "ASBuilder.h"
#include "ASComponent.h"
#include "Buffer.h"
#include "Common.h"
#include "ScratchBuffer.h"

#include <array>

namespace vkpt
{

class CommandBufferManager;
class GeometryQ2;
class GlobalUniformQ2;
class MemoryAllocator;
class PhysicalDevice;

class ASManagerQ2
{
public:
    ASManagerQ2(VkDevice device,
                std::shared_ptr<PhysicalDevice> physDevice,
                std::shared_ptr<MemoryAllocator> allocator,
                std::shared_ptr<CommandBufferManager> cmdManager,
                std::shared_ptr<GeometryQ2> geometryQ2,
                std::shared_ptr<GlobalUniformQ2> uniformQ2);
    ~ASManagerQ2();

    ASManagerQ2(const ASManagerQ2 &other) = delete;
    ASManagerQ2(ASManagerQ2 &&other) noexcept = delete;
    ASManagerQ2 &operator=(const ASManagerQ2 &other) = delete;
    ASManagerQ2 &operator=(ASManagerQ2 &&other) noexcept = delete;

    // Called after GeometryQ2::SubmitStatic (world buffer ready).
    void SubmitStatic();

    // Whether a TLAS has been built (level geometry submitted).
    bool HasTLAS() const;

    VkDescriptorSet GetDescSet() const;
    VkDescriptorSetLayout GetDescSetLayout() const;

private:
    void BuildBLAS(VkCommandBuffer cmd);
    void BuildTLAS(VkCommandBuffer cmd);
    void FillInstanceBuffer();
    void CreateDescSet();

private:
    VkDevice device;
    std::shared_ptr<MemoryAllocator> allocator;
    std::shared_ptr<CommandBufferManager> cmdManager;
    std::shared_ptr<GeometryQ2> geometryQ2;
    std::shared_ptr<GlobalUniformQ2> uniformQ2;

    std::shared_ptr<ScratchBuffer> scratchBuffer;
    std::shared_ptr<ASBuilder> asBuilder;

    // One BLAS over the whole world geometry (non-indexed positions).
    BLASComponent blas;
    // Geometry and effects TLAS. For now effects reuses the same BLAS with a
    // zero mask so the descriptor slot stays valid but never gets hit.
    TLASComponent tlasGeometry;
    TLASComponent tlasEffects;

    // Q2RTX TLAS instance records (QvkGeometryInstance, 64 bytes each),
    // host-visible like Q2RTX buf_instances.
    Buffer instanceBuffer;

    // Descriptor set 0: TLAS array + 4 texel buffer placeholders.
    VkDescriptorPool      descPool;
    VkDescriptorSetLayout descSetLayout;
    VkDescriptorSet       descSet;

    std::array<Buffer, 4>       texelBuffers;
    std::array<VkBufferView, 4> texelViews;

    VkFence fence;
    uint32_t worldPrimCount;
    bool submitted;
};

}
