#include "AsvgfTemporalQ2.h"

#include "GlobalUniformQ2.h"
#include "FramebuffersQ2.h"
#include "RgException.h"

// Image indices (VKPT_IMG_*) come from the vendored dual header, same as
// FramebuffersQ2 uses (MAX_RIMAGES must be defined before the include).
#define MAX_RIMAGES 8192
#include "../q2rtx-shaders/global_textures.h"

#include <array>

using namespace vkpt;

namespace
{
// Images used by asvgf_temporal.comp (read or written). The shader reads
// the current and previous frame surface/lighting data and writes the
// history images plus the atrous ping-pong buffers.
constexpr std::array<int, 29> IMAGES_USED = {
    // current frame surface
    VKPT_IMG_PT_VIEW_DEPTH_A,
    VKPT_IMG_PT_NORMAL_A,
    VKPT_IMG_PT_GEO_NORMAL_A,
    VKPT_IMG_PT_METALLIC_A,
    VKPT_IMG_PT_MOTION,
    VKPT_IMG_PT_VIEW_DIRECTION,
    // current frame lighting
    VKPT_IMG_PT_COLOR_HF,
    VKPT_IMG_PT_COLOR_SPEC,
    VKPT_IMG_PT_COLOR_LF_SH,
    VKPT_IMG_PT_COLOR_LF_COCG,
    // previous frame surface
    VKPT_IMG_PT_VIEW_DEPTH_B,
    VKPT_IMG_PT_NORMAL_B,
    VKPT_IMG_PT_GEO_NORMAL_B,
    // previous frame history
    VKPT_IMG_ASVGF_HIST_COLOR_LF_SH_B,
    VKPT_IMG_ASVGF_HIST_COLOR_LF_COCG_B,
    VKPT_IMG_ASVGF_HIST_COLOR_HF,
    VKPT_IMG_ASVGF_FILTERED_SPEC_B,
    VKPT_IMG_ASVGF_HIST_MOMENTS_HF_B,
    // gradient data
    VKPT_IMG_ASVGF_GRAD_LF_PONG,
    VKPT_IMG_ASVGF_GRAD_HF_SPEC_PONG,
    // current frame history (write)
    VKPT_IMG_ASVGF_HIST_MOMENTS_HF_A,
    VKPT_IMG_ASVGF_HIST_COLOR_LF_SH_A,
    VKPT_IMG_ASVGF_HIST_COLOR_LF_COCG_A,
    VKPT_IMG_ASVGF_FILTERED_SPEC_A,
    // atrous ping-pong (write)
    VKPT_IMG_ASVGF_ATROUS_PING_HF,
    VKPT_IMG_ASVGF_ATROUS_PING_SPEC,
    VKPT_IMG_ASVGF_ATROUS_PING_MOMENTS,
    VKPT_IMG_ASVGF_ATROUS_PING_LF_SH,
    VKPT_IMG_ASVGF_ATROUS_PING_LF_COCG,
};
}

AsvgfTemporalQ2::AsvgfTemporalQ2(VkDevice _device,
                                 std::shared_ptr<ShaderManager> _shaderManager,
                                 std::shared_ptr<GlobalUniformQ2> _uniformQ2,
                                 std::shared_ptr<FramebuffersQ2> _framebuffersQ2)
:
    device(_device),
    shaderManager(std::move(_shaderManager)),
    uniformQ2(std::move(_uniformQ2)),
    framebuffersQ2(std::move(_framebuffersQ2)),
    pipelineLayout(VK_NULL_HANDLE),
    pipeline(VK_NULL_HANDLE)
{
    CreatePipeline();
}

AsvgfTemporalQ2::~AsvgfTemporalQ2()
{
    vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
}

void AsvgfTemporalQ2::CreatePipeline()
{
    VkDescriptorSetLayout setLayouts[] = {
        uniformQ2->GetDescSetLayout(),
        framebuffersQ2->GetDescSetLayout(),
    };

    VkPipelineLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = static_cast<uint32_t>(std::size(setLayouts));
    layoutInfo.pSetLayouts = setLayouts;

    VkResult r = vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pipelineLayout);
    VK_CHECKERROR(r);

    VkShaderModule module = shaderManager->GetShaderModule("Q2AsvgfTemporal");
    if (module == VK_NULL_HANDLE)
    {
        throw RgException(RG_WRONG_ARGUMENT, "Q2AsvgfTemporal shader module is not loaded");
    }

    VkPipelineShaderStageCreateInfo stage = {};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = module;
    stage.pName = "main";

    VkComputePipelineCreateInfo pipelineInfo = {};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = stage;
    pipelineInfo.layout = pipelineLayout;

    r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);
    VK_CHECKERROR(r);
}

void AsvgfTemporalQ2::Dispatch(VkCommandBuffer cmd, uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0)
    {
        return;
    }

    std::array<VkImageMemoryBarrier, IMAGES_USED.size()> barriers = {};
    for (size_t i = 0; i < IMAGES_USED.size(); i++)
    {
        barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[i].image = framebuffersQ2->GetImage(IMAGES_USED[i]);
        barriers[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barriers[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barriers[i].srcAccessMask = 0;
        barriers[i].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barriers[i].subresourceRange.baseMipLevel = 0;
        barriers[i].subresourceRange.levelCount = 1;
        barriers[i].subresourceRange.baseArrayLayer = 0;
        barriers[i].subresourceRange.layerCount = 1;
    }

    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr,
                         static_cast<uint32_t>(barriers.size()), barriers.data());

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);

    VkDescriptorSet descSets[] = {
        uniformQ2->GetDescSet(),
        framebuffersQ2->GetDescSet(),
    };

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipelineLayout, 0,
                            static_cast<uint32_t>(std::size(descSets)), descSets,
                            0, nullptr);

    // Q2RTX dispatches asvgf_temporal.comp with 15-pixel groups.
    vkCmdDispatch(cmd, (width + 14) / 15, (height + 14) / 15, 1);
}
