// Q2RTX ASVGF gradient image pass on the Q2RTX-convention descriptor sets
// (PORTING.md, S3). Mirrors Q2RTX asvgf.c GRADIENT_IMAGE pipeline: builds
// the low-res gradient image used by the A-SVGF filters.
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

class AsvgfGradientImgQ2
{
public:
    AsvgfGradientImgQ2(VkDevice device,
                       std::shared_ptr<ShaderManager> shaderManager,
                       std::shared_ptr<GlobalUniformQ2> uniformQ2,
                       std::shared_ptr<FramebuffersQ2> framebuffersQ2);
    ~AsvgfGradientImgQ2();

    AsvgfGradientImgQ2(const AsvgfGradientImgQ2 &other) = delete;
    AsvgfGradientImgQ2(AsvgfGradientImgQ2 &&other) noexcept = delete;
    AsvgfGradientImgQ2 &operator=(const AsvgfGradientImgQ2 &other) = delete;
    AsvgfGradientImgQ2 &operator=(AsvgfGradientImgQ2 &&other) noexcept = delete;

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
