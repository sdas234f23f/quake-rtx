// Q2RTX ASVGF temporal pass on the Q2RTX-convention descriptor sets
// (PORTING.md, S3). Mirrors Q2RTX asvgf.c TEMPORAL pipeline: temporal
// accumulation / filtering of the lighting channels into the history
// images and the atrous ping-pong buffers.
//
// The output is not displayed yet (the legacy renderer still owns the
// screen); like the other swapped passes, this validates the Q2RTX shader
// pipeline and image bindings end to end.

#pragma once

#include "Common.h"
#include "ShaderManager.h"

namespace vkpt
{

class GlobalUniformQ2;
class FramebuffersQ2;

class AsvgfTemporalQ2
{
public:
    AsvgfTemporalQ2(VkDevice device,
                    std::shared_ptr<ShaderManager> shaderManager,
                    std::shared_ptr<GlobalUniformQ2> uniformQ2,
                    std::shared_ptr<FramebuffersQ2> framebuffersQ2);
    ~AsvgfTemporalQ2();

    AsvgfTemporalQ2(const AsvgfTemporalQ2 &other) = delete;
    AsvgfTemporalQ2(AsvgfTemporalQ2 &&other) noexcept = delete;
    AsvgfTemporalQ2 &operator=(const AsvgfTemporalQ2 &other) = delete;
    AsvgfTemporalQ2 &operator=(AsvgfTemporalQ2 &&other) noexcept = delete;

    void Dispatch(VkCommandBuffer cmd, uint32_t width, uint32_t height);

private:
    void CreatePipeline();

private:
    VkDevice device;
    std::shared_ptr<ShaderManager> shaderManager;
    std::shared_ptr<GlobalUniformQ2> uniformQ2;
    std::shared_ptr<FramebuffersQ2> framebuffersQ2;

    VkPipelineLayout pipelineLayout;
    VkPipeline       pipeline;
};

}
