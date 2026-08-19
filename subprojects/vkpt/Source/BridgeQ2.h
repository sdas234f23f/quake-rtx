// Control-point bridge between the legacy renderer and the Q2RTX
// post-processing chain (PORTING.md, S3 "bridge").
//
// Until the Q2RTX ray tracing core produces its own image, we copy the
// legacy renderer's HDR result (FB_IMAGE_INDEX_PRE_FINAL) into the Q2RTX
// IMG_TAA_OUTPUT, run the already-swapped Q2RTX post-processing (bloom +
// tone mapping) on it, and blit the result back into the legacy
// FB_IMAGE_INDEX_FINAL so the rest of the legacy pipeline (upscale,
// effects, present) shows it. This makes the Q2RTX post-processing visible
// and gives a reference point for the remaining swaps.

#pragma once

#include "Common.h"

namespace vkpt
{

class Framebuffers;
class FramebuffersQ2;
class BloomQ2;
class ToneMappingQ2;

class BridgeQ2
{
public:
    BridgeQ2(VkDevice device,
             std::shared_ptr<Framebuffers> framebuffers,
             std::shared_ptr<FramebuffersQ2> framebuffersQ2,
             std::shared_ptr<BloomQ2> bloomQ2,
             std::shared_ptr<ToneMappingQ2> toneMappingQ2);
    ~BridgeQ2();

    BridgeQ2(const BridgeQ2 &other) = delete;
    BridgeQ2(BridgeQ2 &&other) noexcept = delete;
    BridgeQ2 &operator=(const BridgeQ2 &other) = delete;
    BridgeQ2 &operator=(BridgeQ2 &&other) noexcept = delete;

    // Copies the legacy PRE_FINAL image into Q2 IMG_TAA_OUTPUT, runs the
    // Q2RTX bloom + tone mapping, and blits the result into the legacy
    // FINAL image.
    void Run(VkCommandBuffer cmd, uint32_t frameIndex,
             uint32_t width, uint32_t height, float frameTime);

private:
    VkDevice device;
    std::shared_ptr<Framebuffers> framebuffers;
    std::shared_ptr<FramebuffersQ2> framebuffersQ2;
    std::shared_ptr<BloomQ2> bloomQ2;
    std::shared_ptr<ToneMappingQ2> toneMappingQ2;
};

}
