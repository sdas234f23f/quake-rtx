#include "SkyBufferResolveQ2.h"

#include "GlobalUniformQ2.h"
#include "VertexBufferQ2.h"
#include "RgException.h"

#include <array>

using namespace vkpt;

SkyBufferResolveQ2::SkyBufferResolveQ2(VkDevice _device,
                                       std::shared_ptr<ShaderManager> _shaderManager,
                                       std::shared_ptr<GlobalUniformQ2> _uniformQ2,
                                       std::shared_ptr<VertexBufferQ2> _vertexBufferQ2)
:
    device(_device),
    shaderManager(std::move(_shaderManager)),
    uniformQ2(std::move(_uniformQ2)),
    vertexBufferQ2(std::move(_vertexBufferQ2)),
    pipelineLayout(VK_NULL_HANDLE),
    pipeline(VK_NULL_HANDLE)
{
    CreatePipeline();
}

SkyBufferResolveQ2::~SkyBufferResolveQ2()
{
    vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
}

void SkyBufferResolveQ2::CreatePipeline()
{
    // sky_buffer_resolve.comp declares only set 0 (UBO) and set 1
    // (vertex buffer - for the SunColorBuffer).
    VkDescriptorSetLayout setLayouts[] = {
        uniformQ2->GetDescSetLayout(),
        vertexBufferQ2->GetDescSetLayout(),
    };

    VkPipelineLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = static_cast<uint32_t>(std::size(setLayouts));
    layoutInfo.pSetLayouts = setLayouts;

    VkResult r = vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pipelineLayout);
    VK_CHECKERROR(r);

    VkShaderModule module = shaderManager->GetShaderModule("Q2SkyBufferResolve");
    if (module == VK_NULL_HANDLE)
    {
        throw RgException(RG_WRONG_ARGUMENT, "Q2SkyBufferResolve shader module is not loaded");
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

void SkyBufferResolveQ2::Dispatch(VkCommandBuffer cmd)
{
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);

    VkDescriptorSet descSets[] = {
        uniformQ2->GetDescSet(),
        vertexBufferQ2->GetDescSet(),
    };

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipelineLayout, 0,
                            static_cast<uint32_t>(std::size(descSets)), descSets,
                            0, nullptr);

    vkCmdDispatch(cmd, 1, 1, 1);
}
