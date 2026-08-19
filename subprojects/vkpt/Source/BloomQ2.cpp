#include "BloomQ2.h"

#include "GlobalUniformQ2.h"
#include "FramebuffersQ2.h"
#include "RgException.h"

// Image indices (VKPT_IMG_*) come from the vendored dual header, same as
// FramebuffersQ2 uses (MAX_RIMAGES must be defined before the include).
#define MAX_RIMAGES 8192
#include "../q2rtx-shaders/global_textures.h"

#include <array>
#include <cmath>

using namespace vkpt;

namespace
{
// Images used by the bloom passes (read or written).
constexpr std::array<int, 3> IMAGES_USED = {
    VKPT_IMG_TAA_OUTPUT,
    VKPT_IMG_BLOOM_HBLUR,
    VKPT_IMG_BLOOM_VBLUR,
};
}

BloomQ2::BloomQ2(VkDevice _device,
                 std::shared_ptr<ShaderManager> _shaderManager,
                 std::shared_ptr<GlobalUniformQ2> _uniformQ2,
                 std::shared_ptr<FramebuffersQ2> _framebuffersQ2)
:
    device(_device),
    shaderManager(std::move(_shaderManager)),
    uniformQ2(std::move(_uniformQ2)),
    framebuffersQ2(std::move(_framebuffersQ2)),
    blurLayout(VK_NULL_HANDLE),
    generalLayout(VK_NULL_HANDLE),
    pipelineDownscale(VK_NULL_HANDLE),
    pipelineBlur(VK_NULL_HANDLE),
    pipelineComposite(VK_NULL_HANDLE)
{
    CreatePipelines();
}

BloomQ2::~BloomQ2()
{
    vkDestroyPipeline(device, pipelineComposite, nullptr);
    vkDestroyPipeline(device, pipelineBlur, nullptr);
    vkDestroyPipeline(device, pipelineDownscale, nullptr);
    vkDestroyPipelineLayout(device, generalLayout, nullptr);
    vkDestroyPipelineLayout(device, blurLayout, nullptr);
}

void BloomQ2::CreatePipelines()
{
    // Same set layout as the checkerboard pass: set 0 = UBO, set 1 = images.
    VkDescriptorSetLayout setLayouts[] = {
        uniformQ2->GetDescSetLayout(),
        framebuffersQ2->GetDescSetLayout(),
    };

    // blur uses push constants (see BlurPushConstants in the header).
    VkPushConstantRange blurRange = {};
    blurRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    blurRange.offset = 0;
    blurRange.size = sizeof(BlurPushConstants);

    VkPipelineLayoutCreateInfo blurLayoutInfo = {};
    blurLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    blurLayoutInfo.setLayoutCount = static_cast<uint32_t>(std::size(setLayouts));
    blurLayoutInfo.pSetLayouts = setLayouts;
    blurLayoutInfo.pushConstantRangeCount = 1;
    blurLayoutInfo.pPushConstantRanges = &blurRange;

    VkResult r = vkCreatePipelineLayout(device, &blurLayoutInfo, nullptr, &blurLayout);
    VK_CHECKERROR(r);

    VkPipelineLayoutCreateInfo generalLayoutInfo = {};
    generalLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    generalLayoutInfo.setLayoutCount = static_cast<uint32_t>(std::size(setLayouts));
    generalLayoutInfo.pSetLayouts = setLayouts;

    r = vkCreatePipelineLayout(device, &generalLayoutInfo, nullptr, &generalLayout);
    VK_CHECKERROR(r);

    const char *names[] = {
        "Q2BloomDownscale",
        "Q2BloomBlur",
        "Q2BloomComposite",
    };
    VkShaderModule modules[] = {
        shaderManager->GetShaderModule(names[0]),
        shaderManager->GetShaderModule(names[1]),
        shaderManager->GetShaderModule(names[2]),
    };
    for (int i = 0; i < 3; i++)
    {
        if (modules[i] == VK_NULL_HANDLE)
        {
            throw RgException(RG_WRONG_ARGUMENT, std::string(names[i]) + " shader module is not loaded");
        }
    }

    VkPipelineShaderStageCreateInfo stages[3] = {};
    for (int i = 0; i < 3; i++)
    {
        stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[i].stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stages[i].module = modules[i];
        stages[i].pName = "main";
    }

    VkComputePipelineCreateInfo infos[3] = {};
    for (int i = 0; i < 3; i++)
    {
        infos[i].sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        infos[i].stage = stages[i];
    }

    // downscale + composite use the general layout, blur uses the push
    // constant layout (as in Q2RTX bloom.c).
    infos[0].layout = generalLayout;
    infos[1].layout = blurLayout;
    infos[2].layout = generalLayout;

    VkPipeline *pipelines[] = { &pipelineDownscale, &pipelineBlur, &pipelineComposite };
    r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 3, infos, nullptr, pipelines[0]);
    VK_CHECKERROR(r);
}

void BloomQ2::Dispatch(VkCommandBuffer cmd, uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0)
    {
        return;
    }

    // Transition the bloom images to GENERAL (no-op once they are there).
    // GENERAL->GENERAL (not UNDEFINED) so previously copied content is kept.
    std::array<VkImageMemoryBarrier, IMAGES_USED.size()> barriers = {};
    for (size_t i = 0; i < IMAGES_USED.size(); i++)
    {
        barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[i].image = framebuffersQ2->GetImage(IMAGES_USED[i]);
        barriers[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
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

    VkDescriptorSet descSets[] = {
        uniformQ2->GetDescSet(),
        framebuffersQ2->GetDescSet(),
    };

    const uint32_t wgX = (width + 15) / 16;
    const uint32_t wgY = (height + 15) / 16;
    const uint32_t wgXQuarter = ((width / 4) + 15) / 16;
    const uint32_t wgYQuarter = ((height / 4) + 15) / 16;

    // 1. downscale TAA_OUTPUT -> BLOOM_VBLUR (quarter res)
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineDownscale);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            generalLayout, 0,
                            static_cast<uint32_t>(std::size(descSets)), descSets,
                            0, nullptr);
    vkCmdDispatch(cmd, wgXQuarter, wgYQuarter, 1);

    // 2. horizontal blur: BLOOM_VBLUR -> BLOOM_HBLUR (quarter res)
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineBlur);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            blurLayout, 0,
                            static_cast<uint32_t>(std::size(descSets)), descSets,
                            0, nullptr);

    BlurPushConstants hblur = {};
    // Mirror Q2RTX compute_push_constants(): bloom_sigma is relative to the
    // TAA output height, the blur runs at a quarter of that resolution.
    const float bloomSigma = 0.037f; // Q2RTX default bloom_sigma cvar
    float effectiveSigma = bloomSigma * static_cast<float>(height) * 0.25f;
    effectiveSigma = std::min(effectiveSigma, 100.0f);
    effectiveSigma = std::max(effectiveSigma, 1.0f);

    hblur.pixstep_x = 1.0f;
    hblur.pixstep_y = 0.0f;
    hblur.argument_scale = -1.0f / (2.0f * effectiveSigma * effectiveSigma);
    hblur.normalization_scale = 1.0f / (std::sqrt(2.0f * 3.14159265f) * effectiveSigma);
    hblur.num_samples = static_cast<int>(std::lround(effectiveSigma * 4.0f));
    hblur.pass = 0;

    vkCmdPushConstants(cmd, blurLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(hblur), &hblur);
    vkCmdDispatch(cmd, wgXQuarter, wgYQuarter, 1);

    // 3. vertical blur: BLOOM_HBLUR -> BLOOM_VBLUR (quarter res)
    BlurPushConstants vblur = hblur;
    vblur.pixstep_x = 0.0f;
    vblur.pixstep_y = 1.0f;
    vblur.pass = 1;

    vkCmdPushConstants(cmd, blurLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(vblur), &vblur);
    vkCmdDispatch(cmd, wgXQuarter, wgYQuarter, 1);

    // 4. composite bloom back into TAA_OUTPUT (full res)
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineComposite);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            generalLayout, 0,
                            static_cast<uint32_t>(std::size(descSets)), descSets,
                            0, nullptr);
    vkCmdDispatch(cmd, wgX, wgY, 1);
}
