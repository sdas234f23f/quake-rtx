// Q2RTX ASVGF low-frequency filter pass on the Q2RTX-convention descriptor
// sets (PORTING.md, S3). Mirrors Q2RTX asvgf.c ATROUS_LF pipeline: 4
// iterations of the low-frequency wavelet filter driven by an iteration
// push constant, ping-ponging between the ATROUS_PING/PONG LF images.
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

class AsvgfLfQ2
{
public:
    AsvgfLfQ2(VkDevice device,
              std::shared_ptr<ShaderManager> shaderManager,
              std::shared_ptr<GlobalUniformQ2> uniformQ2,
              std::shared_ptr<FramebuffersQ2> framebuffersQ2);
    ~AsvgfLfQ2();

    AsvgfLfQ2(const AsvgfLfQ2 &other) = delete;
    AsvgfLfQ2(AsvgfLfQ2 &&other) noexcept = delete;
    AsvgfLfQ2 &operator=(const AsvgfLfQ2 &other) = delete;
    AsvgfLfQ2 &operator=(AsvgfLfQ2 &&other) noexcept = delete;

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
