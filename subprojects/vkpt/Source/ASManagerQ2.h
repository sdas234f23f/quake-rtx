// ASManagerQ2: builds the Q2RTX acceleration structures and instance data
// for the world + dynamic geometry (stages G2 and G1b of the porting plan,
// see PORTING.md).
//
// Part of the Q2RTX binding layer (Block 2): after GeometryQ2 has filled the
// Q2RTX world buffer (primitives + positions), this module builds one BLAS
// over the world positions (non-indexed, stride 12) and a combined TLAS with
// a static world instance plus separate world, view-weapon, and viewer-model
// dynamic instances rebuilt every frame from GeometryQ2's contiguous ranges.
// It also fills the InstanceBuffer SSBO (model instances + TLAS instance
// bookkeeping) and creates descriptor set 0 (TLAS array + texel buffer
// placeholders) that the Q2RTX rgen shaders expect.
//
// Stage G1b frame-safety: the static BLAS/effects-TLAS/effects-instance are
// built once (SubmitStatic, level load) and never touched again. Everything
// that must change every frame - the dynamic BLAS, the combined geometry
// TLAS, the TLAS instance buffer, and descriptor set 0's TLAS binding - is
// ring-buffered with MAX_FRAMES_IN_FLIGHT slots selected by frameIndex
// (SubmitDynamic), mirroring VertexBufferQ2/FramebuffersQ2's
// descSets[MAX_FRAMES_IN_FLIGHT] + activeFrameIndex + no-arg GetDescSet()
// pattern so PathTracerQ2 and friends need no changes. Dynamic scratch
// buffers/builders are ringed too: SubmitDynamic resets only the slot whose
// frame fence was already waited, bounding scratch allocations without
// reusing memory that an in-flight acceleration-structure build still needs.

#pragma once

#include "ASBuilder.h"
#include "ASComponent.h"
#include "Buffer.h"
#include "Common.h"
#include "GeometryQ2.h"
#include "ScratchBuffer.h"

#include <array>
#include <memory>

namespace vkpt
{

class CommandBufferManager;
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

    // Called after GeometryQ2::SubmitStatic (world buffer ready). Builds the
    // static world BLAS and the effects TLAS/instance once, at level load.
    void SubmitStatic();

    // Stage G1b: called every frame, after GeometryQ2::SubmitDynamic(frameIndex)
    // has uploaded this frame's dynamic aggregate buffer. Rebuilds one BLAS
    // for each non-empty visibility range, rebuilds the combined geometry TLAS,
    // refills the TLAS instance bookkeeping, and updates descriptor set 0's
    // TLAS binding - all in ring slot frameIndex. Must run before the Q2
    // uniform upload and before anything binds GetDescSet() this frame.
    void SubmitDynamic(VkCommandBuffer cmd, uint32_t frameIndex);

    // Whether a TLAS has been built (level geometry submitted).
    bool HasTLAS() const;

    VkDescriptorSet GetDescSet() const;
    VkDescriptorSetLayout GetDescSetLayout() const;

private:
    void BuildBLAS(VkCommandBuffer cmd);
    void BuildDynamicBLAS(VkCommandBuffer cmd, uint32_t frameIndex,
                          GeometryQ2::DynamicGeometryCategory category,
                          const GeometryQ2::DynamicGeometryRange &range);
    void BuildTLAS(VkCommandBuffer cmd);
    void BuildCombinedTLAS(VkCommandBuffer cmd, uint32_t frameIndex);
    void FillInstanceBuffer(uint32_t frameIndex);
    void CreateDescSet();

private:
    VkDevice device;
    std::shared_ptr<MemoryAllocator> allocator;
    std::shared_ptr<CommandBufferManager> cmdManager;
    std::shared_ptr<GeometryQ2> geometryQ2;
    std::shared_ptr<GlobalUniformQ2> uniformQ2;

    std::shared_ptr<ScratchBuffer> staticScratchBuffer;
    std::shared_ptr<ASBuilder> staticAsBuilder;
    std::array<std::shared_ptr<ScratchBuffer>, MAX_FRAMES_IN_FLIGHT> dynamicScratchBuffers;
    std::array<std::shared_ptr<ASBuilder>, MAX_FRAMES_IN_FLIGHT> dynamicAsBuilders;

    // One BLAS over the whole static world geometry (non-indexed positions),
    // built once at level load.
    BLASComponent blas;
    // One dynamic BLAS per visibility category and frame slot. The BLAS source
    // ranges share GeometryQ2's aggregate buffer, but separate TLAS instances
    // are required because Vulkan instance masks match any overlapping bit.
    using DynamicBlasFrame = std::array<
        std::unique_ptr<BLASComponent>,
        GeometryQ2::DYNAMIC_GEOMETRY_CATEGORY_COUNT>;
    std::array<DynamicBlasFrame, MAX_FRAMES_IN_FLIGHT> dynamicBlas;

    // Combined geometry TLAS: static world instance plus each non-empty
    // dynamic visibility category, rebuilt every frame. Effects TLAS is
    // unchanged: same never-hit static instance/BLAS built once in
    // SubmitStatic.
    std::array<std::unique_ptr<TLASComponent>, MAX_FRAMES_IN_FLIGHT> tlasGeometry;
    TLASComponent tlasEffects;

    // Q2RTX TLAS instance records (QvkGeometryInstance, 64 bytes each),
    // host-visible like Q2RTX buf_instances. Each ring slot has room for the
    // static world plus all dynamic categories; effectsInstanceBuffer is the
    // single-slot buffer for the untouched effects TLAS.
    std::array<Buffer, MAX_FRAMES_IN_FLIGHT> instanceBuffer;
    Buffer effectsInstanceBuffer;

    // Descriptor set 0: TLAS array + 4 texel buffer placeholders. Stage G1b:
    // one set per frame in flight so binding 0's geometry TLAS entry can be
    // repointed at that frame's rebuilt TLAS without racing a command buffer
    // from a different ring slot that may still be executing.
    VkDescriptorPool      descPool;
    VkDescriptorSetLayout descSetLayout;
    VkDescriptorSet       descSets[MAX_FRAMES_IN_FLIGHT];
    uint32_t              activeFrameIndex;

    std::array<Buffer, 4>       texelBuffers;
    std::array<VkBufferView, 4> texelViews;

    VkFence fence;
    uint32_t worldPrimCount;
    bool submitted;
};

}
