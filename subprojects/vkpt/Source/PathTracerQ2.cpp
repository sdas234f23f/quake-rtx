#include "PathTracerQ2.h"

#include "ASManagerQ2.h"
#include "CommandBufferManager.h"
#include "FramebuffersQ2.h"
#include "GlobalUniformQ2.h"
#include "PhysicalDevice.h"
#include "ShaderManager.h"
#include "Utils.h"
#include "VertexBufferQ2.h"

// Q2RTX binding contract: constants.h defines the SBT group indices
// (SBT_ENTRIES_PER_PIPELINE) and RAY_GEN descriptor set layout.
#include "../q2rtx-shaders/constants.h"

#include <cstring>
#include <string>

using namespace vkpt;

namespace
{

// Matches Q2RTX pt_push_constants_t.
struct PathTracerPushConstants
{
    int gpu_index;
    int bounce;
};

// Q2RTX path_tracer.c pipeline_index_t order: PRIMARY_RAYS=0,
// REFLECT_REFRACT_1=1, REFLECT_REFRACT_2=2, DIRECT_LIGHTING=3, ...
constexpr uint32_t PIPELINE_DIRECT_LIGHTING = 3;
// Q2RTX allocates the SBT for every pipeline; we only fill blocks 0 and 3
// and leave the rest zeroed (never referenced).
constexpr uint32_t SBT_PIPELINE_COUNT = 4;

} // namespace

PathTracerQ2::PathTracerQ2(VkDevice _device,
                           std::shared_ptr<PhysicalDevice> physDevice,
                           std::shared_ptr<MemoryAllocator> _allocator,
                           std::shared_ptr<CommandBufferManager> _cmdManager,
                           const ShaderManager *_shaderManager,
                           std::shared_ptr<ASManagerQ2> _asManagerQ2,
                           std::shared_ptr<GlobalUniformQ2> _uniformQ2,
                           std::shared_ptr<FramebuffersQ2> _framebuffersQ2,
                           std::shared_ptr<VertexBufferQ2> _vertexBufferQ2)
: device(_device),
  allocator(std::move(_allocator)),
  cmdManager(std::move(_cmdManager)),
  asManagerQ2(std::move(_asManagerQ2)),
  uniformQ2(std::move(_uniformQ2)),
  framebuffersQ2(std::move(_framebuffersQ2)),
  vertexBufferQ2(std::move(_vertexBufferQ2)),
  shaderManager(_shaderManager),
  pipelineLayout(VK_NULL_HANDLE),
  pipeline(VK_NULL_HANDLE),
  pipelineDirect(VK_NULL_HANDLE),
  groupBaseAlignment(0),
  handleSize(0),
  alignedHandleSize(0),
  shaderRgen("Q2PrimaryRays"),
  shaderRmiss("Q2PathTracerRmiss"),
  shaderRchit("Q2PathTracerRchit"),
  shaderRahit("Q2PathTracerMaskedRahit"),
  shaderParticle("Q2PathTracerParticleRahit"),
  shaderExplosion("Q2PathTracerExplosionRahit"),
  shaderSprite("Q2PathTracerSpriteRahit"),
  shaderBeamRahit("Q2PathTracerBeamRahit"),
  shaderBeamRint("Q2PathTracerBeamRint"),
  shaderDirect("Q2DirectLighting")
{
    groupBaseAlignment = physDevice->GetRTPipelineProperties().shaderGroupBaseAlignment;
    handleSize = physDevice->GetRTPipelineProperties().shaderGroupHandleSize;
    alignedHandleSize = static_cast<uint32_t>(Utils::Align<VkDeviceSize>(handleSize, groupBaseAlignment));

    CreatePipeline();
    CreateShaderBindingTable();
}

PathTracerQ2::~PathTracerQ2()
{
    sbtBuffer.Destroy();
    vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyPipeline(device, pipelineDirect, nullptr);
    vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
}

void PathTracerQ2::CreatePipeline()
{
    // Descriptor sets in Q2RTX order: [0] = RT (TLAS + texel buffers),
    // [1] = UBO + instance buffer, [2] = textures, [3] = vertex buffer.
    VkDescriptorSetLayout setLayouts[] =
    {
        asManagerQ2->GetDescSetLayout(),
        uniformQ2->GetDescSetLayout(),
        framebuffersQ2->GetDescSetLayout(),
        vertexBufferQ2->GetDescSetLayout(),
    };

    VkPushConstantRange pushRange = {};
    pushRange.stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    pushRange.offset = 0;
    pushRange.size = sizeof(PathTracerPushConstants);

    VkPipelineLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = static_cast<uint32_t>(std::size(setLayouts));
    layoutInfo.pSetLayouts = setLayouts;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;

    VkResult r = vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pipelineLayout);
    VK_CHECKERROR(r);

    // Shader stages in Q2RTX order (path_tracer.c vkpt_pt_create_pipelines):
    // 0 rgen, 1 rmiss, 2 rchit, 3 masked.rahit, 4 particle, 5 explosion,
    // 6 sprite, 7 beam.rahit, 8 beam.rint. PRIMARY_RAYS uses all 9.
    VkShaderModule modRgen = shaderManager->GetShaderModule(shaderRgen);
    VkShaderModule modRmiss = shaderManager->GetShaderModule(shaderRmiss);
    VkShaderModule modRchit = shaderManager->GetShaderModule(shaderRchit);
    VkShaderModule modRahit = shaderManager->GetShaderModule(shaderRahit);
    VkShaderModule modParticle = shaderManager->GetShaderModule(shaderParticle);
    VkShaderModule modExplosion = shaderManager->GetShaderModule(shaderExplosion);
    VkShaderModule modSprite = shaderManager->GetShaderModule(shaderSprite);
    VkShaderModule modBeamRahit = shaderManager->GetShaderModule(shaderBeamRahit);
    VkShaderModule modBeamRint = shaderManager->GetShaderModule(shaderBeamRint);

    VkPipelineShaderStageCreateInfo stages[] =
    {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR,
            .module = modRgen,
            .pName = "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_MISS_BIT_KHR,
            .module = modRmiss,
            .pName = "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
            .module = modRchit,
            .pName = "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
            .module = modRahit,
            .pName = "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
            .module = modParticle,
            .pName = "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
            .module = modExplosion,
            .pName = "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
            .module = modSprite,
            .pName = "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
            .module = modBeamRahit,
            .pName = "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_INTERSECTION_BIT_KHR,
            .module = modBeamRint,
            .pName = "main",
        },
    };

    // SBT groups in Q2RTX order (path_tracer.c rt_shader_group_info).
    constexpr uint32_t GROUPS = 9;
    VkRayTracingShaderGroupCreateInfoKHR groups[GROUPS] = {};

    auto SetGeneral = [&](uint32_t idx, uint32_t shader)
    {
        groups[idx].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
        groups[idx].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
        groups[idx].generalShader = shader;
        groups[idx].closestHitShader = VK_SHADER_UNUSED_KHR;
        groups[idx].anyHitShader = VK_SHADER_UNUSED_KHR;
        groups[idx].intersectionShader = VK_SHADER_UNUSED_KHR;
    };
    auto SetHit = [&](uint32_t idx, uint32_t closest, uint32_t any)
    {
        groups[idx].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
        groups[idx].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
        groups[idx].generalShader = VK_SHADER_UNUSED_KHR;
        groups[idx].closestHitShader = closest;
        groups[idx].anyHitShader = any;
        groups[idx].intersectionShader = VK_SHADER_UNUSED_KHR;
    };

    // 0: rgen, 1: rmiss.
    SetGeneral(0, 0);
    SetGeneral(1, 1);
    // 2: opaque closest hit, 3: masked any hit.
    SetHit(2, 2, VK_SHADER_UNUSED_KHR);
    SetHit(3, 2, 3);
    // 4: effects (empty), 5-7: particle/explosion/sprite any hit.
    SetHit(4, VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR);
    SetHit(5, VK_SHADER_UNUSED_KHR, 4);
    SetHit(6, VK_SHADER_UNUSED_KHR, 5);
    SetHit(7, VK_SHADER_UNUSED_KHR, 6);
    // 8: beam procedural hit group.
    groups[8].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[8].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR;
    groups[8].generalShader = VK_SHADER_UNUSED_KHR;
    groups[8].closestHitShader = VK_SHADER_UNUSED_KHR;
    groups[8].anyHitShader = 7;
    groups[8].intersectionShader = 8;

    VkRayTracingPipelineCreateInfoKHR pipelineInfo = {};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
    pipelineInfo.stageCount = static_cast<uint32_t>(std::size(stages));
    pipelineInfo.pStages = stages;
    pipelineInfo.groupCount = GROUPS;
    pipelineInfo.pGroups = groups;
    pipelineInfo.maxPipelineRayRecursionDepth = 1;
    pipelineInfo.layout = pipelineLayout;

    // Empty pipeline library, mirroring legacy RayTracingPipeline / Q2RTX
    // (VK_KHR_pipeline_library is enabled on the device).
    VkPipelineLibraryCreateInfoKHR libraryInfo = {};
    libraryInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR;
    pipelineInfo.pLibraryInfo = &libraryInfo;

    r = svkCreateRayTracingPipelinesKHR(device, VK_NULL_HANDLE, VK_NULL_HANDLE,
                                        1, &pipelineInfo, nullptr, &pipeline);
    if (r != VK_SUCCESS)
    {
        pipeline = VK_NULL_HANDLE;
        return;
    }

    // G5: direct lighting pipeline - the same stages/groups, raygen swapped
    // to direct_lighting.rgen. Skipped if the shader is missing.
    VkShaderModule modDirect = shaderManager->GetShaderModule(shaderDirect);
    if (modDirect != VK_NULL_HANDLE)
    {
        VkPipelineShaderStageCreateInfo directStages[std::size(stages)];
        std::memcpy(directStages, stages, sizeof(stages));
        directStages[0].module = modDirect;

        VkRayTracingPipelineCreateInfoKHR directInfo = pipelineInfo;
        directInfo.stageCount = static_cast<uint32_t>(std::size(directStages));
        directInfo.pStages = directStages;

        r = svkCreateRayTracingPipelinesKHR(device, VK_NULL_HANDLE, VK_NULL_HANDLE,
                                            1, &directInfo, nullptr, &pipelineDirect);
        if (r != VK_SUCCESS)
        {
            pipelineDirect = VK_NULL_HANDLE;
        }
    }
}

void PathTracerQ2::CreateShaderBindingTable()
{
    if (pipeline == VK_NULL_HANDLE)
    {
        return;
    }

    // Q2RTX sizes the SBT for all pipelines (SBT_ENTRIES_PER_PIPELINE per
    // pipeline); dispatch uses the pipeline index as the SBT block offset.
    // Only the blocks we dispatch are filled (PRIMARY_RAYS = 0,
    // DIRECT_LIGHTING = 3); the rest stays zeroed and is never referenced.
    const uint32_t sbtSize = SBT_PIPELINE_COUNT * SBT_ENTRIES_PER_PIPELINE * alignedHandleSize;

    sbtBuffer.Init(allocator, sbtSize,
                   VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR |
                       VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   "Q2RTX path tracer SBT");

    void *mapped = sbtBuffer.Map();
    memset(mapped, 0, sbtSize);

    WriteSbtBlock(static_cast<uint8_t *>(mapped), pipeline, 0);
    WriteSbtBlock(static_cast<uint8_t *>(mapped), pipelineDirect, PIPELINE_DIRECT_LIGHTING);

    sbtBuffer.Unmap();
}

void PathTracerQ2::WriteSbtBlock(uint8_t *dstBase, VkPipeline pipeline, uint32_t blockIndex)
{
    if (pipeline == VK_NULL_HANDLE)
    {
        return;
    }

    std::vector<uint8_t> handles(handleSize * SBT_ENTRIES_PER_PIPELINE);
    VkResult r = svkGetRayTracingShaderGroupHandlesKHR(device, pipeline, 0, SBT_ENTRIES_PER_PIPELINE,
                                                       handles.size(), handles.data());
    VK_CHECKERROR(r);

    uint8_t *dst = dstBase + (VkDeviceSize)blockIndex * SBT_ENTRIES_PER_PIPELINE * alignedHandleSize;
    for (uint32_t i = 0; i < SBT_ENTRIES_PER_PIPELINE; i++)
    {
        memcpy(dst + i * alignedHandleSize, handles.data() + i * handleSize, handleSize);
    }
}

void PathTracerQ2::DispatchPrimaryRays(VkCommandBuffer cmd, uint32_t width, uint32_t height)
{
    // Nothing to trace until the level geometry produced a TLAS.
    if (!asManagerQ2->HasTLAS())
    {
        return;
    }

    DispatchRayTrace(cmd, pipeline, 0, width, height);
}

void PathTracerQ2::DispatchDirectLighting(VkCommandBuffer cmd, uint32_t width, uint32_t height)
{
    if (pipelineDirect == VK_NULL_HANDLE || !asManagerQ2->HasTLAS())
    {
        return;
    }

    // Make the primary-rays G-buffer writes visible to the direct lighting
    // pass (same queue, but a memory dependency is required).
    VkMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                         VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);

    DispatchRayTrace(cmd, pipelineDirect, PIPELINE_DIRECT_LIGHTING, width, height);
}

void PathTracerQ2::DispatchRayTrace(VkCommandBuffer cmd, VkPipeline pipeline, uint32_t sbtBlock,
                                    uint32_t width, uint32_t height)
{
    if (pipeline == VK_NULL_HANDLE || width == 0 || height == 0)
    {
        return;
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipeline);

    VkDescriptorSet sets[] =
    {
        asManagerQ2->GetDescSet(),
        uniformQ2->GetDescSet(),
        framebuffersQ2->GetDescSet(),
        vertexBufferQ2->GetDescSet(),
    };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                            pipelineLayout, 0, static_cast<uint32_t>(std::size(sets)),
                            sets, 0, nullptr);

    PathTracerPushConstants push = {};
    push.gpu_index = -1; // single GPU
    push.bounce = 0;

    vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_RAYGEN_BIT_KHR,
                       0, sizeof(push), &push);

    VkDeviceAddress sbtAddress = sbtBuffer.GetAddress();
    VkDeviceSize sbtOffset = (VkDeviceSize)SBT_ENTRIES_PER_PIPELINE * sbtBlock * alignedHandleSize;

    // Q2RTX: each pipeline's block starts at block * 9 * alignment; the
    // raygen entry is the first group, miss and hit share the same block.
    VkStridedDeviceAddressRegionKHR raygen = {};
    raygen.deviceAddress = sbtAddress + sbtOffset;
    raygen.stride = alignedHandleSize;
    raygen.size = alignedHandleSize;

    VkStridedDeviceAddressRegionKHR missAndHit = {};
    missAndHit.deviceAddress = sbtAddress + sbtOffset;
    missAndHit.stride = alignedHandleSize;
    missAndHit.size = (VkDeviceSize)alignedHandleSize * SBT_ENTRIES_PER_PIPELINE;

    VkStridedDeviceAddressRegionKHR callable = {};
    callable.deviceAddress = 0;
    callable.stride = 0;
    callable.size = 0;

    // Checkerboard: half width, depth 2 (odd/even columns). Q2RTX with one
    // device dispatches width/2 x height x 2.
    svkCmdTraceRaysKHR(cmd, &raygen, &missAndHit, &missAndHit, &callable,
                       width / 2, height, 2);
}
