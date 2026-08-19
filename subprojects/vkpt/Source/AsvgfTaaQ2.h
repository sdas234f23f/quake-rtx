// Q2RTX ASVGF TAA upscale pass on the Q2RTX-convention descriptor sets
// (PORTING.md, S3). Mirrors Q2RTX asvgf.c TAAU pipeline: temporal
// anti-aliasing + upscale that combines the flat color/motion buffers with
// the previous frame's TAA output into IMG_TAA_OUTPUT.
//
// Uses all three Q2RTX descriptor sets (the readback buffer write is in
// the vertex buffer set).
//
// The output is not displayed yet (the legacy renderer still owns the
// screen); like the other swapped passes, this validates the shader
// pipeline and image bindings end to end.

#pragma once

#include "Common.h"
#include "ShaderManager.h"

namespace vkpt
{

class GlobalUniformQ2;
class FramebuffersQ2;
class VertexBufferQ2;

class AsvgfTaaQ2
{
public:
    AsvgfTaaQ2(VkDevice device,
               std::shared_ptr<ShaderManager> shaderManager,
               std::shared_ptr<GlobalUniformQ2> uniformQ2,
               std::shared_ptr<FramebuffersQ2> framebuffersQ2,
               std::shared_ptr<VertexBufferQ2> vertexBufferQ2);
    ~AsvgfTaaQ2();

    AsvgfTaaQ2(const AsvgfTaaQ2 &other) = delete;
    AsvgfTaaQ2(AsvgfTaaQ2 &&other) noexcept = delete;
    AsvgfTaaQ2 &operator=(const AsvgfTaaQ2 &other) = delete;
    AsvgfTaaQ2 &operator=(AsvgfTaaQ2 &&other) noexcept = delete;

    void Dispatch(VkCommandBuffer cmd, uint32_t width, uint32_t height);

private:
    void CreatePipeline();

private:
    VkDevice device;
    std::shared_ptr<ShaderManager> shaderManager;
    std::shared_ptr<GlobalUniformQ2> uniformQ2;
    std::shared_ptr<FramebuffersQ2> framebuffersQ2;
    std::shared_ptr<VertexBufferQ2> vertexBufferQ2;

    VkPipelineLayout pipelineLayout;
    VkPipeline       pipeline;
};

}
