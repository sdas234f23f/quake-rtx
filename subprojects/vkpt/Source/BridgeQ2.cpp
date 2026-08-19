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
                   uint32_t width, uint32_t height, float frameTime)
{
    if (width == 0 || height == 0)
    {
        return;
    }

    VkImage legacyPreFinal = framebuffers->GetImage(FramebufferImageIndex::FB_IMAGE_INDEX_PRE_FINAL, frameIndex);
    VkImage legacyFinal = framebuffers->GetImage(FramebufferImageIndex::FB_IMAGE_INDEX_FINAL, frameIndex);
    VkImage q2TaaOutput = framebuffersQ2->GetImage(VKPT_IMG_TAA_OUTPUT);

    if (legacyPreFinal == VK_NULL_HANDLE || legacyFinal == VK_NULL_HANDLE || q2TaaOutput == VK_NULL_HANDLE)
    {
        return;
    }

    // 1. legacy PRE_FINAL -> Q2 IMG_TAA_OUTPUT
    BarrierImage(cmd, legacyPreFinal,
                 VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    BarrierImage(cmd, q2TaaOutput,
                 VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    BlitImage(cmd, legacyPreFinal, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              q2TaaOutput, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, width, height);

    // back to GENERAL for the compute passes
    BarrierImage(cmd, legacyPreFinal,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                 VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    BarrierImage(cmd, q2TaaOutput,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    // 2. Q2RTX post-processing on TAA_OUTPUT
    bloomQ2->Dispatch(cmd, width, height);
    toneMappingQ2->Dispatch(cmd, width, height, frameTime);

    // 3. Q2 IMG_TAA_OUTPUT -> legacy FINAL
    BarrierImage(cmd, q2TaaOutput,
                 VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    BarrierImage(cmd, legacyFinal,
                 VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    BlitImage(cmd, q2TaaOutput, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              legacyFinal, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, width, height);

    // back to GENERAL for the rest of the legacy pipeline
    BarrierImage(cmd, q2TaaOutput,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                 VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    BarrierImage(cmd, legacyFinal,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
}
