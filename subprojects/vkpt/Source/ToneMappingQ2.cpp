#include "ToneMappingQ2.h"

#include "GlobalUniformQ2.h"
#include "FramebuffersQ2.h"
#include "VertexBufferQ2.h"
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
// IMG_TAA_OUTPUT is read and written by the tone mapping passes.
constexpr std::array<int, 1> IMAGES_USED = {
    VKPT_IMG_TAA_OUTPUT,
};
}

ToneMappingQ2::ToneMappingQ2(VkDevice _device,
                             std::shared_ptr<ShaderManager> _shaderManager,
                             std::shared_ptr<GlobalUniformQ2> _uniformQ2,
                             std::shared_ptr<FramebuffersQ2> _framebuffersQ2,
                             std::shared_ptr<VertexBufferQ2> _vertexBufferQ2)
:
    device(_device),
    shaderManager(std::move(_shaderManager)),
    uniformQ2(std::move(_uniformQ2)),
    framebuffersQ2(std::move(_framebuffersQ2)),
    vertexBufferQ2(std::move(_vertexBufferQ2)),
    histogramLayout(VK_NULL_HANDLE),
    curveLayout(VK_NULL_HANDLE),
    applyLayout(VK_NULL_HANDLE),
    pipelineHistogram(VK_NULL_HANDLE),
    pipelineCurve(VK_NULL_HANDLE),
    pipelineApplySDR(VK_NULL_HANDLE)
{
    CreatePipelines();
}

ToneMappingQ2::~ToneMappingQ2()
{
    vkDestroyPipeline(device, pipelineApplySDR, nullptr);
    vkDestroyPipeline(device, pipelineCurve, nullptr);
    vkDestroyPipeline(device, pipelineHistogram, nullptr);
    vkDestroyPipelineLayout(device, applyLayout, nullptr);
    vkDestroyPipelineLayout(device, curveLayout, nullptr);
    vkDestroyPipelineLayout(device, histogramLayout, nullptr);
}

void ToneMappingQ2::CreatePipelines()
{
    // All three Q2RTX descriptor sets: set 0 = UBO, set 1 = textures,
    // set 2 = vertex buffer (carries the ToneMappingBuffer).
    VkDescriptorSetLayout setLayouts[] = {
        uniformQ2->GetDescSetLayout(),
        framebuffersQ2->GetDescSetLayout(),
        vertexBufferQ2->GetDescSetLayout(),
    };

    VkPipelineLayoutCreateInfo baseLayoutInfo = {};
    baseLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    baseLayoutInfo.setLayoutCount = static_cast<uint32_t>(std::size(setLayouts));
    baseLayoutInfo.pSetLayouts = setLayouts;

    VkPushConstantRange curveRange = {};
    curveRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    curveRange.offset = 0;
    curveRange.size = sizeof(CurvePushConstants);

    VkPipelineLayoutCreateInfo curveLayoutInfo = baseLayoutInfo;
    curveLayoutInfo.pushConstantRangeCount = 1;
    curveLayoutInfo.pPushConstantRanges = &curveRange;

    VkPushConstantRange applyRange = {};
    applyRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    applyRange.offset = 0;
    applyRange.size = sizeof(ApplyPushConstants);

    VkPipelineLayoutCreateInfo applyLayoutInfo = baseLayoutInfo;
    applyLayoutInfo.pushConstantRangeCount = 1;
    applyLayoutInfo.pPushConstantRanges = &applyRange;

    VkResult r = vkCreatePipelineLayout(device, &baseLayoutInfo, nullptr, &histogramLayout);
    VK_CHECKERROR(r);
    r = vkCreatePipelineLayout(device, &curveLayoutInfo, nullptr, &curveLayout);
    VK_CHECKERROR(r);
    r = vkCreatePipelineLayout(device, &applyLayoutInfo, nullptr, &applyLayout);
    VK_CHECKERROR(r);

    const char *names[] = {
        "Q2ToneMappingHistogram",
        "Q2ToneMappingCurve",
        "Q2ToneMappingApply",
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

    infos[0].layout = histogramLayout; // histogram
    infos[1].layout = curveLayout;     // curve

    // apply: specialize spec_tone_mapping_hdr = 0 (SDR).
    VkSpecializationMapEntry specEntry = {};
    specEntry.constantID = 0;
    specEntry.offset = 0;
    specEntry.size = sizeof(uint32_t);

    uint32_t hdr = 0;
    VkSpecializationInfo specInfo = {};
    specInfo.mapEntryCount = 1;
    specInfo.pMapEntries = &specEntry;
    specInfo.dataSize = sizeof(hdr);
    specInfo.pData = &hdr;

    infos[2].stage.pSpecializationInfo = &specInfo;
    infos[2].layout = applyLayout;

    VkPipeline *pipelines[] = { &pipelineHistogram, &pipelineCurve, &pipelineApplySDR };
    r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 3, infos, nullptr, pipelines[0]);
    VK_CHECKERROR(r);
}

void ToneMappingQ2::Dispatch(VkCommandBuffer cmd, uint32_t width, uint32_t height, float frameTime)
{
    if (width == 0 || height == 0)
    {
        return;
    }

    // Transition TAA_OUTPUT to GENERAL (no-op once it is there).
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

    VkDescriptorSet descSets[] = {
        uniformQ2->GetDescSet(),
        framebuffersQ2->GetDescSet(),
        vertexBufferQ2->GetDescSet(),
    };

    // 1. histogram
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineHistogram);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            histogramLayout, 0,
                            static_cast<uint32_t>(std::size(descSets)), descSets,
                            0, nullptr);
    vkCmdDispatch(cmd, (width + 15) / 16, (height + 15) / 16, 1);

    // 2. tone curve (single workgroup; push constants like Q2RTX
    //    vkpt_tone_mapping_record_cmd_buffer: reset=0, frame_time, Gaussian
    //    slope-blur weights).
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineCurve);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            curveLayout, 0,
                            static_cast<uint32_t>(std::size(descSets)), descSets,
                            0, nullptr);

    const float slopeBlurSigma = 6.0f; // Q2RTX default tm_slope_blur_sigma
    CurvePushConstants curve = {};
    curve.resetCurve = 0.0f;
    curve.frameTime = frameTime;

    float gaussianSum = 0.0f;
    for (int i = 0; i < 14; i++)
    {
        float k = std::exp(-static_cast<float>(i * i) / (2.0f * slopeBlurSigma * slopeBlurSigma));
        gaussianSum += k * (i == 0 ? 1.0f : 2.0f);
        curve.weights[i] = k;
    }
    for (int i = 0; i < 14; i++)
    {
        curve.weights[i] /= gaussianSum;
    }

    vkCmdPushConstants(cmd, curveLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(curve), &curve);
    vkCmdDispatch(cmd, 1, 1, 1);

    // 3. apply curve in-place on TAA_OUTPUT (SDR path).
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineApplySDR);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            applyLayout, 0,
                            static_cast<uint32_t>(std::size(descSets)), descSets,
                            0, nullptr);

    // Same knee parameters as Q2RTX (tm_knee_start 0.9, tm_white_point 10).
    const float kneeStart = 0.9f;
    const float kneeWhitePoint = 10.0f;
    ApplyPushConstants apply = {};
    apply.knee_w = (kneeStart * (kneeStart - 2.0f) + kneeWhitePoint) / (kneeWhitePoint - 1.0f);
    apply.knee_a = -kneeStart * kneeStart;
    apply.knee_b = apply.knee_w - 2.0f * kneeStart;

    vkCmdPushConstants(cmd, applyLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(apply), &apply);
    vkCmdDispatch(cmd, (width + 15) / 16, (height + 15) / 16, 1);
}
