// First Q2RTX shader swap (PORTING.md, stage S3): runs the vendored
// checkerboard_interleave.comp on our Q2RTX-convention descriptor sets
// (set 0 = GlobalUniformQ2, set 1 = FramebuffersQ2). The shader needs no
// geometry (set 3), so it is the smallest end-to-end test of the binding
// layer: UBO -> images -> compute dispatch.
//
// The output images are not displayed yet (the legacy renderer still owns
// the screen); this pass exists to prove the Q2RTX shader pipeline works
// and to catch binding/layout mistakes early.

#pragma once

#include "Common.h"
#include "ShaderManager.h"

namespace vkpt
{

class GlobalUniformQ2;
class FramebuffersQ2;

class ShaderSwapQ2
{
public:
    ShaderSwapQ2(VkDevice device,
                 std::shared_ptr<ShaderManager> shaderManager,
                 std::shared_ptr<GlobalUniformQ2> uniformQ2,
                 std::shared_ptr<FramebuffersQ2> framebuffersQ2);
    ~ShaderSwapQ2();

    ShaderSwapQ2(const ShaderSwapQ2 &other) = delete;
    ShaderSwapQ2(ShaderSwapQ2 &&other) noexcept = delete;
    ShaderSwapQ2 &operator=(const ShaderSwapQ2 &other) = delete;
    ShaderSwapQ2 &operator=(ShaderSwapQ2 &&other) noexcept = delete;

    // Transitions the images this pass reads/writes to GENERAL and dispatches
    // checkerboard_interleave.comp. Safe to call every frame; the transition
    // is a no-op once the images are already in GENERAL.
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
