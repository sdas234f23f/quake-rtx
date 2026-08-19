// Q2RTX sky buffer resolve pass on the Q2RTX-convention descriptor sets
// (PORTING.md, S3). Mirrors Q2RTX physical_sky.c pipeline_resolve: converts
// the fixed-point sun/sky accumulation in SunColorBuffer into float values
// (single 1x1 dispatch).
//
// Uses set 0 (UBO) and set 1 (vertex buffer, which carries SunColorBuffer).

#pragma once

#include "Common.h"
#include "ShaderManager.h"

namespace vkpt
{

class GlobalUniformQ2;
class VertexBufferQ2;

class SkyBufferResolveQ2
{
public:
    SkyBufferResolveQ2(VkDevice device,
                       std::shared_ptr<ShaderManager> shaderManager,
                       std::shared_ptr<GlobalUniformQ2> uniformQ2,
                       std::shared_ptr<VertexBufferQ2> vertexBufferQ2);
    ~SkyBufferResolveQ2();

    SkyBufferResolveQ2(const SkyBufferResolveQ2 &other) = delete;
    SkyBufferResolveQ2(SkyBufferResolveQ2 &&other) noexcept = delete;
    SkyBufferResolveQ2 &operator=(const SkyBufferResolveQ2 &other) = delete;
    SkyBufferResolveQ2 &operator=(SkyBufferResolveQ2 &&other) noexcept = delete;

    void Dispatch(VkCommandBuffer cmd);

private:
    void CreatePipeline();

private:
    VkDevice device;
    std::shared_ptr<ShaderManager> shaderManager;
    std::shared_ptr<GlobalUniformQ2> uniformQ2;
    std::shared_ptr<VertexBufferQ2> vertexBufferQ2;

    VkPipelineLayout pipelineLayout;
    VkPipeline       pipeline;
};

}
