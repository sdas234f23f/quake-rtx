// Q2RTX ASVGF a-trous pass on the Q2RTX-convention descriptor sets
// (PORTING.md, S3). Mirrors Q2RTX asvgf.c ATROUS_ITER_0..3 pipelines: 4
// spatial wavelet iterations (specialized on spec_iteration 0..3) that
// filter the HF/specular channels and finally composite everything into
// IMG_ASVGF_COLOR on the last iteration.
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

class AsvgfAtrousQ2
{
public:
    AsvgfAtrousQ2(VkDevice device,
                  std::shared_ptr<ShaderManager> shaderManager,
                  std::shared_ptr<GlobalUniformQ2> uniformQ2,
                  std::shared_ptr<FramebuffersQ2> framebuffersQ2);
    ~AsvgfAtrousQ2();

    AsvgfAtrousQ2(const AsvgfAtrousQ2 &other) = delete;
    AsvgfAtrousQ2(AsvgfAtrousQ2 &&other) noexcept = delete;
    AsvgfAtrousQ2 &operator=(const AsvgfAtrousQ2 &other) = delete;
    AsvgfAtrousQ2 &operator=(AsvgfAtrousQ2 &&other) noexcept = delete;

    void Dispatch(VkCommandBuffer cmd, uint32_t width, uint32_t height);

private:
    void CreatePipelines();

private:
    VkDevice device;
    std::shared_ptr<ShaderManager> shaderManager;
    std::shared_ptr<GlobalUniformQ2> uniformQ2;
    std::shared_ptr<FramebuffersQ2> framebuffersQ2;

    VkPipelineLayout pipelineLayout;
    VkPipeline       pipelines[4]; // spec_iteration 0..3
};

}
