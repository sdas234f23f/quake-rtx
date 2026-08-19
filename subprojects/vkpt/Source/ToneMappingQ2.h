// Q2RTX tone mapping on the Q2RTX-convention descriptor sets (PORTING.md,
// S3). Mirrors Q2RTX tone_mapping.c: build a luminance histogram
// (tone_mapping_histogram.comp), solve the tone curve
// (tone_mapping_curve.comp), then apply it to IMG_TAA_OUTPUT in-place
// (tone_mapping_apply.comp, SDR specialization).
//
// This is the first pass that uses all three Q2RTX descriptor sets
// (UBO, textures, vertex buffer - the latter for the ToneMappingBuffer).
//
// The output is not displayed yet (the legacy renderer still owns the
// screen); like the other swapped passes, this validates the shader
// pipelines, the tone mapping buffer and the image bindings end to end.

#pragma once

#include "Common.h"
#include "ShaderManager.h"

namespace vkpt
{

class GlobalUniformQ2;
class FramebuffersQ2;
class VertexBufferQ2;

class ToneMappingQ2
{
public:
    ToneMappingQ2(VkDevice device,
                  std::shared_ptr<ShaderManager> shaderManager,
                  std::shared_ptr<GlobalUniformQ2> uniformQ2,
                  std::shared_ptr<FramebuffersQ2> framebuffersQ2,
                  std::shared_ptr<VertexBufferQ2> vertexBufferQ2);
    ~ToneMappingQ2();

    ToneMappingQ2(const ToneMappingQ2 &other) = delete;
    ToneMappingQ2(ToneMappingQ2 &&other) noexcept = delete;
    ToneMappingQ2 &operator=(const ToneMappingQ2 &other) = delete;
    ToneMappingQ2 &operator=(ToneMappingQ2 &&other) noexcept = delete;

    void Dispatch(VkCommandBuffer cmd, uint32_t width, uint32_t height, float frameTime);

private:
    void CreatePipelines();

    // Matches the tone_mapping_curve.comp push constant block: reset_curve,
    // frame_time, then 14 slope-blur weights.
    struct CurvePushConstants
    {
        float resetCurve;
        float frameTime;
        float weights[14];
    };

    // Matches the tone_mapping_apply.comp push constant block.
    struct ApplyPushConstants
    {
        float knee_w;
        float knee_a;
        float knee_b;
    };

private:
    VkDevice device;
    std::shared_ptr<ShaderManager> shaderManager;
    std::shared_ptr<GlobalUniformQ2> uniformQ2;
    std::shared_ptr<FramebuffersQ2> framebuffersQ2;
    std::shared_ptr<VertexBufferQ2> vertexBufferQ2;

    VkPipelineLayout histogramLayout;
    VkPipelineLayout curveLayout;
    VkPipelineLayout applyLayout;
    VkPipeline       pipelineHistogram;
    VkPipeline       pipelineCurve;
    VkPipeline       pipelineApplySDR;
};

}
