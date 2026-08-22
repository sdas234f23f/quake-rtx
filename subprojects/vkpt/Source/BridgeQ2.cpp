#include "BridgeQ2.h"

#include "Framebuffers.h"
#include "FramebuffersQ2.h"
#include "BloomQ2.h"
#include "ToneMappingQ2.h"
#include "Generated/ShaderCommonCFramebuf.h"

// Image indices (VKPT_IMG_*) come from the vendored dual header, same as
// FramebuffersQ2 uses (MAX_RIMAGES must be defined before the include).
#define MAX_RIMAGES 8192
#include "../q2rtx-shaders/global_textures.h"

using namespace vkpt;

BridgeQ2::BridgeQ2(VkDevice _device,
                   std::shared_ptr<Framebuffers> _framebuffers,
                   std::shared_ptr<FramebuffersQ2> _framebuffersQ2,
                   std::shared_ptr<BloomQ2> _bloomQ2,
                   std::shared_ptr<ToneMappingQ2> _toneMappingQ2)
:
    device(_device),
    framebuffers(std::move(_framebuffers)),
    framebuffersQ2(std::move(_framebuffersQ2)),
    bloomQ2(std::move(_bloomQ2)),
    toneMappingQ2(std::move(_toneMappingQ2))
{
}

BridgeQ2::~BridgeQ2() = default;

namespace
{

void BarrierImage(VkCommandBuffer cmd, VkImage image,
                  VkImageLayout oldLayout, VkImageLayout newLayout,
                  VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                  VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage)
{
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.image = image;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0,
                         0, nullptr, 0, nullptr, 1, &barrier);
}

void BlitImage(VkCommandBuffer cmd,
               VkImage srcImage, VkImageLayout srcLayout,
               VkImage dstImage, VkImageLayout dstLayout,
               uint32_t width, uint32_t height)
{
    VkImageBlit region = {};
    region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.srcSubresource.baseArrayLayer = 0;
    region.srcSubresource.layerCount = 1;
    region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.dstSubresource.baseArrayLayer = 0;
    region.dstSubresource.layerCount = 1;
    region.srcOffsets[1] = { static_cast<int32_t>(width), static_cast<int32_t>(height), 1 };
    region.dstOffsets[1] = { static_cast<int32_t>(width), static_cast<int32_t>(height), 1 };

    vkCmdBlitImage(cmd, srcImage, srcLayout, dstImage, dstLayout,
                   1, &region, VK_FILTER_NEAREST);
}

} // namespace

void BridgeQ2::Run(VkCommandBuffer cmd, uint32_t frameIndex,
                   uint32_t width, uint32_t height, float frameTime, bool enabled,
                   uint32_t debugFlags)
{
    if (!enabled || width == 0 || height == 0)
    {
        return;
    }

    VkImage legacyFinal = framebuffers->GetImage(FramebufferImageIndex::FB_IMAGE_INDEX_FINAL, frameIndex);
    VkImage q2TaaOutput = framebuffersQ2->GetImage(VKPT_IMG_TAA_OUTPUT);

    // G5 debug (rt_q2debug): blit an intermediate Q2 image instead of the
    // final one.
    //
    // Viewable (RGBA16F, the blit converts to the RGBA8 final image; HDR
    // values above 1 clamp to white):
    //   0 = TAA_OUTPUT (the real final image)
    //   1 = PT_BASE_COLOR_A (albedo)
    //   2 = ASVGF_COLOR (lighting after denoise + compositing)
    //   3 = FLAT_COLOR (after checkerboard interleave)
    //   4 = ASVGF_TAA_A, 7 = ASVGF_TAA_B (TAA history)
    //
    // NOT viewable - these are R32_UINT images holding packed/encoded values,
    // and blitting them into an RGBA8 image produces meaningless colours.
    // Use 2 or 0 to judge lighting instead:
    //   5 = PT_COLOR_HF, 6 = ASVGF_ATROUS_PING_HF
    //
    // Partially viewable:
    //   8 = PT_METALLIC_A (R8G8: red = metallic, green = roughness)
    // Packed in bits 16..19: bit 12 is RG_DEBUG_DRAW_Q2_BRIDGE_BIT (4096)
    // and always set while the bridge is on, so bits 12..15 were shifted by
    // one (rt_q2debug N showed image N+1).
    const uint32_t debugSrc = (debugFlags >> 16) & 0xF;
    VkImage src = q2TaaOutput;
    if (debugSrc == 1)
    {
        src = framebuffersQ2->GetImage(VKPT_IMG_PT_BASE_COLOR_A);
    }
    else if (debugSrc == 2)
    {
        src = framebuffersQ2->GetImage(VKPT_IMG_ASVGF_COLOR);
    }
    else if (debugSrc == 3)
    {
        src = framebuffersQ2->GetImage(VKPT_IMG_FLAT_COLOR);
    }
    else if (debugSrc == 4)
    {
        src = framebuffersQ2->GetImage(VKPT_IMG_ASVGF_TAA_A);
    }
    else if (debugSrc == 5)
    {
        src = framebuffersQ2->GetImage(VKPT_IMG_PT_COLOR_HF);
    }
    else if (debugSrc == 6)
    {
        src = framebuffersQ2->GetImage(VKPT_IMG_ASVGF_ATROUS_PING_HF);
    }
    else if (debugSrc == 7)
    {
        src = framebuffersQ2->GetImage(VKPT_IMG_ASVGF_TAA_B);
    }
    else if (debugSrc == 8)
    {
        // G6 diagnostic: shows the primary G-buffer metallic/roughness.
        // R8G8 image blitted into the R8G8B8A8 FINAL -> red = metallic.
        src = framebuffersQ2->GetImage(VKPT_IMG_PT_METALLIC_A);
    }

    if (legacyFinal == VK_NULL_HANDLE || src == VK_NULL_HANDLE)
    {
        return;
    }

    // Bloom + tone mapping only on the real final image (TAA_OUTPUT).
    if (src == q2TaaOutput)
    {
        bloomQ2->Dispatch(cmd, width, height);
        toneMappingQ2->Dispatch(cmd, width, height, frameTime);
    }

    // Q2 source image -> legacy FINAL
    BarrierImage(cmd, src,
                 VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    BarrierImage(cmd, legacyFinal,
                 VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    BlitImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              legacyFinal, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, width, height);

    // back to GENERAL for the rest of the legacy pipeline
    BarrierImage(cmd, src,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                 VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    BarrierImage(cmd, legacyFinal,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
}
