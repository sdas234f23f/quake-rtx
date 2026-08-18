// Q2RTX bloom pass on the Q2RTX-convention descriptor sets (PORTING.md, S3).
// Mirrors Q2RTX bloom.c: downscale TAA_OUTPUT to a quarter, horizontal +
// vertical Gaussian blur (bloom_blur.comp, driven by push constants), then
// composite the bloom back into TAA_OUTPUT.
//
// The output is not displayed yet (the legacy renderer still owns the
// screen); like the checkerboard pass, this validates the Q2RTX shader
// pipeline and image bindings end to end.

#pragma once

#include "Common.h"
#include "ShaderManager.h"

namespace vkpt
{

class GlobalUniformQ2;
class FramebuffersQ2;

class BloomQ2
{
public:
    BloomQ2(VkDevice device,
            std::shared_ptr<ShaderManager> shaderManager,
            std::shared_ptr<GlobalUniformQ2> uniformQ2,
            std::shared_ptr<FramebuffersQ2> framebuffersQ2);
    ~BloomQ2();

    BloomQ2(const BloomQ2 &other) = delete;
    BloomQ2(BloomQ2 &&other) noexcept = delete;
    BloomQ2 &operator=(const BloomQ2 &other) = delete;
    BloomQ2 &operator=(BloomQ2 &&other) noexcept = delete;

    void Dispatch(VkCommandBuffer cmd, uint32_t width, uint32_t height);

private:
    void CreatePipelines();

    // Q2RTX bloom_blur.comp push constants (must match the shader layout).
    struct BlurPushConstants
    {
        float pixstep_x;
        float pixstep_y;
        float argument_scale;
        float normalization_scale;
        int   num_samples;
        int   pass;
    };

private:
    VkDevice device;
    std::shared_ptr<ShaderManager> shaderManager;
    std::shared_ptr<GlobalUniformQ2> uniformQ2;
    std::shared_ptr<FramebuffersQ2> framebuffersQ2;

    VkPipelineLayout blurLayout;
    VkPipelineLayout generalLayout;
    VkPipeline       pipelineDownscale;
    VkPipeline       pipelineBlur;
    VkPipeline       pipelineComposite;
};

}
