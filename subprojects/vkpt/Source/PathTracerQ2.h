// PathTracerQ2: Q2RTX-convention ray tracing pipeline (stage G3 of the
// porting plan, see PORTING.md).
//
// Part of the Q2RTX binding layer (Block 2): builds the PRIMARY_RAYS ray
// tracing pipeline from the vendored Q2RTX shaders (primary_rays.rgen +
// path_tracer.rmiss + path_tracer.rchit + path_tracer_masked.rahit), creates
// the shader binding table, and dispatches primary rays against the Q2RTX
// TLAS every frame. The results are written into the Q2RTX G-buffer images
// (IMG_PT_*) which nothing displays yet - G4 switches the frame source.

#pragma once

#include "Buffer.h"
#include "Common.h"

#include <vector>

namespace vkpt
{

class ASManagerQ2;
class CommandBufferManager;
class FramebuffersQ2;
class GlobalUniformQ2;
class MemoryAllocator;
class PhysicalDevice;
class ShaderManager;
class VertexBufferQ2;

class PathTracerQ2
{
public:
    PathTracerQ2(VkDevice device,
                 std::shared_ptr<PhysicalDevice> physDevice,
                 std::shared_ptr<MemoryAllocator> allocator,
                 std::shared_ptr<CommandBufferManager> cmdManager,
                 const ShaderManager *shaderManager,
                 std::shared_ptr<ASManagerQ2> asManagerQ2,
                 std::shared_ptr<GlobalUniformQ2> uniformQ2,
                 std::shared_ptr<FramebuffersQ2> framebuffersQ2,
                 std::shared_ptr<VertexBufferQ2> vertexBufferQ2);
    ~PathTracerQ2();

    PathTracerQ2(const PathTracerQ2 &other) = delete;
    PathTracerQ2(PathTracerQ2 &&other) noexcept = delete;
    PathTracerQ2 &operator=(const PathTracerQ2 &other) = delete;
    PathTracerQ2 &operator=(PathTracerQ2 &&other) noexcept = delete;

    // Traces primary rays into the Q2RTX G-buffer. No-op until the Q2RTX
    // TLAS exists (level loaded).
    void DispatchPrimaryRays(VkCommandBuffer cmd, uint32_t width, uint32_t height);

    // G5: direct lighting (sun + local lights) into the lighting channels
    // (IMG_PT_COLOR_LF/HF/SPEC), consumed by the ASVGF chain.
    void DispatchDirectLighting(VkCommandBuffer cmd, uint32_t width, uint32_t height);

private:
    void CreatePipeline();
    void CreateShaderBindingTable();
    void WriteSbtBlock(uint8_t *dstBase, VkPipeline pipeline, uint32_t blockIndex);
    void DispatchRayTrace(VkCommandBuffer cmd, VkPipeline pipeline, uint32_t sbtBlock,
                          uint32_t width, uint32_t height);
    void OnShaderReload(const ShaderManager *shaderManager);

private:
    VkDevice device;
    std::shared_ptr<MemoryAllocator> allocator;
    std::shared_ptr<CommandBufferManager> cmdManager;

    std::shared_ptr<ASManagerQ2> asManagerQ2;
    std::shared_ptr<GlobalUniformQ2> uniformQ2;
    std::shared_ptr<FramebuffersQ2> framebuffersQ2;
    std::shared_ptr<VertexBufferQ2> vertexBufferQ2;
    const ShaderManager *shaderManager;

    VkPipelineLayout pipelineLayout;
    VkPipeline pipeline;
    VkPipeline pipelineDirect;

    Buffer sbtBuffer;
    uint32_t groupBaseAlignment;
    uint32_t handleSize;
    uint32_t alignedHandleSize;

    const char *shaderRgen;
    const char *shaderRmiss;
    const char *shaderRchit;
    const char *shaderRahit;
    const char *shaderParticle;
    const char *shaderExplosion;
    const char *shaderSprite;
    const char *shaderBeamRahit;
    const char *shaderBeamRint;
    const char *shaderDirect;
};

}
