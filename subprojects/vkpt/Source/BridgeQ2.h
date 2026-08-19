// Control-point bridge between the legacy renderer and the Q2RTX frame
// (PORTING.md, G4 "bridge switch").
//
// The full Q2RTX chain (primary rays -> ASVGF denoise -> compositing ->
// checkerboard interleave -> TAA upscale) runs in DrawFrame and produces
// IMG_TAA_OUTPUT. When the "rt_q2bridge" cvar is enabled (host flag
// RG_DEBUG_DRAW_Q2_BRIDGE_BIT), BridgeQ2 finishes the Q2RTX post-processing
// (bloom + tone mapping) on that image and blits the result into the legacy
// FB_IMAGE_INDEX_FINAL, so the rest of the legacy pipeline (upscale,
// effects, present) shows the Q2RTX frame. When disabled, the legacy
// renderer's own image is displayed unchanged (reference point).

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

    // When enabled, runs bloom + tone mapping on the Q2RTX TAA_OUTPUT image
    // and blits the result into the legacy FINAL image. Otherwise does
    // nothing (the legacy image stays on screen).
    void Run(VkCommandBuffer cmd, uint32_t frameIndex,
             uint32_t width, uint32_t height, float frameTime, bool enabled);

private:
    VkDevice device;
    std::shared_ptr<Framebuffers> framebuffers;
    std::shared_ptr<FramebuffersQ2> framebuffersQ2;
    std::shared_ptr<BloomQ2> bloomQ2;
    std::shared_ptr<ToneMappingQ2> toneMappingQ2;
};

}
