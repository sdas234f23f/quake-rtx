// Q2RTX compositing pass on the Q2RTX-convention descriptor sets
// (PORTING.md, S3). Mirrors Q2RTX asvgf.c COMPOSITING pipeline: combines
// the lighting channels (LF/HF/specular/throughput) with the surface
// parameters into IMG_ASVGF_COLOR when the denoiser is disabled.
//
// The output is not displayed yet (the legacy renderer still owns the
// screen); like the checkerboard and bloom passes, this validates the
// Q2RTX shader pipeline and image bindings end to end.

#pragma once

#include "Common.h"
#include "ShaderManager.h"

namespace vkpt
{

class GlobalUniformQ2;
class FramebuffersQ2;

class CompositingQ2
{
public:
    CompositingQ2(VkDevice device,
                  std::shared_ptr<ShaderManager> shaderManager,
                  std::shared_ptr<GlobalUniformQ2> uniformQ2,
                  std::shared_ptr<FramebuffersQ2> framebuffersQ2);
    ~CompositingQ2();

    CompositingQ2(const CompositingQ2 &other) = delete;
    CompositingQ2(CompositingQ2 &&other) noexcept = delete;
    CompositingQ2 &operator=(const CompositingQ2 &other) = delete;
    CompositingQ2 &operator=(CompositingQ2 &&other) noexcept = delete;

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
