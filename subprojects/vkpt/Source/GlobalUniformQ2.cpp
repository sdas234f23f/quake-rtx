#include "GlobalUniformQ2.h"

// Q2RTX binding contract: the vendored dual C/GLSL header. In C mode
// (VKPT_SHADER undefined) it defines QVKUniformBuffer_t whose layout is
// verified to match GLSL std140 by Tools/check_q2rtx_ubo.py.
#include "../q2rtx-shaders/global_ubo.h"

#include "GlobalUniform.h"
#include "Generated/ShaderCommonC.h"
#include "CmdLabel.h"

#include <algorithm>
#include <cstring>

using namespace vkpt;

namespace
{

void FillUniformBuffer(QVKUniformBuffer_t &ubo, const ShGlobalUniform &src)
{
    memset(&ubo, 0, sizeof(ubo));

    // Start the cvar block from the defaults declared in global_ubo.h.
    // UBO_CVAR_LIST stays defined after the include (only UBO_CVAR_DO is
    // undefined), so redefine it to expand the list into assignments.
#define UBO_CVAR_DO(name, default_value) ubo.name = (default_value);
    UBO_CVAR_LIST
#undef UBO_CVAR_DO

    // Per-frame data, mirroring what Q2RTX main.c fills.
    ubo.current_frame_idx = static_cast<int>(src.frameId);
    ubo.width             = static_cast<int>(src.renderWidth);
    ubo.height            = static_cast<int>(src.renderHeight);
    ubo.current_gpu_slice_width = ubo.width;
    ubo.time              = src.time;

    ubo.first_person_model = 1;
    ubo.environment_type   = 0;

    // Default sun pointing down (a real value comes with the light port).
    ubo.sun_direction[0] = 0.0f;
    ubo.sun_direction[1] = -1.0f;
    ubo.sun_direction[2] = 0.0f;

    // Camera.
    memcpy(ubo.cam_pos,  src.cameraPosition, 3 * sizeof(float));
    memcpy(ubo.V,        src.view,           16 * sizeof(float));
    memcpy(ubo.invV,     src.invView,        16 * sizeof(float));
    memcpy(ubo.V_prev,   src.viewPrev,       16 * sizeof(float));
    memcpy(ubo.P,        src.projection,     16 * sizeof(float));
    memcpy(ubo.invP,     src.invProjection,  16 * sizeof(float));
    memcpy(ubo.P_prev,   src.projectionPrev, 16 * sizeof(float));
    // The legacy uniform does not store an inverse of the previous
    // projection; reuse the current one.
    memcpy(ubo.invP_prev, src.invProjection, 16 * sizeof(float));

    ubo.screen_image_width  = ubo.width;
    ubo.screen_image_height = ubo.height;
    ubo.inv_width           = 1.0f / static_cast<float>(ubo.width);
    ubo.inv_height          = 1.0f / static_cast<float>(ubo.height);

    // Q2RTX bloom fields. taa_image == render resolution for now (no TAA
    // upscale yet); prev_taa_output starts at 0 on the first frame, which is
    // what Q2RTX effectively has before its first assignment.
    ubo.bloom_intensity = 0.002f;
    ubo.taa_image_width  = ubo.width;
    ubo.taa_image_height = ubo.height;
    ubo.taa_output_width  = ubo.width;
    ubo.taa_output_height = ubo.height;
    ubo.prev_taa_output_width  = 0;
    ubo.prev_taa_output_height = 0;
}

} // namespace

// Q2RTX keeps the UBO and the instance SSBO in one buffer (see Q2RTX
// uniform_buffer.c): binding 0 is the UBO at offset 0, binding 1 is the
// InstanceBuffer right after it, aligned to minUniformBufferOffsetAlignment.
// 256 is a safe multiple of every typical alignment (16/64/256) and keeps
// the instance data 256-byte aligned for the descriptor offset.
const VkDeviceSize Q2_UBO_ALIGNMENT = 256;

GlobalUniformQ2::GlobalUniformQ2(VkDevice _device, std::shared_ptr<MemoryAllocator> _allocator)
:
    device(_device),
    descPool(VK_NULL_HANDLE),
    descSetLayout(VK_NULL_HANDLE),
    descSet(VK_NULL_HANDLE)
{
    const VkDeviceSize bufferSize =
        (sizeof(QVKUniformBuffer_t) + Q2_UBO_ALIGNMENT - 1) & ~(Q2_UBO_ALIGNMENT - 1);

    buffer = std::make_shared<AutoBuffer>(_device, _allocator);
    buffer->Create(bufferSize + sizeof(InstanceBuffer),
                   VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                       VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                   "Q2RTX uniform + instance buffer");

    CreateDescriptors();
}

void GlobalUniformQ2::CreateDescriptors()
{
    const VkDeviceSize instanceOffset =
        (sizeof(QVKUniformBuffer_t) + Q2_UBO_ALIGNMENT - 1) & ~(Q2_UBO_ALIGNMENT - 1);

    VkDescriptorSetLayoutBinding bindings[2] = {};

    bindings[0].binding = GLOBAL_UBO_BINDING_IDX;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_ALL;

    bindings[1].binding = GLOBAL_INSTANCE_BUFFER_BINDING_IDX;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_ALL;

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = bindings;

    VkResult r = vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &descSetLayout);
    VK_CHECKERROR(r);

    VkDescriptorPoolSize poolSizes[2] = {};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = 1;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[1].descriptorCount = 1;

    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    poolInfo.maxSets = 1;

    r = vkCreateDescriptorPool(device, &poolInfo, nullptr, &descPool);
    VK_CHECKERROR(r);

    VkDescriptorSetAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = descPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &descSetLayout;

    r = vkAllocateDescriptorSets(device, &allocInfo, &descSet);
    VK_CHECKERROR(r);

    VkDescriptorBufferInfo bufInfos[2] = {};

    bufInfos[0].buffer = buffer->GetDeviceLocal();
    bufInfos[0].offset = 0;
    bufInfos[0].range = sizeof(QVKUniformBuffer_t);

    bufInfos[1].buffer = buffer->GetDeviceLocal();
    bufInfos[1].offset = instanceOffset;
    bufInfos[1].range = sizeof(InstanceBuffer);

    VkWriteDescriptorSet writes[2] = {};

    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = descSet;
    writes[0].dstBinding = GLOBAL_UBO_BINDING_IDX;
    writes[0].dstArrayElement = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].descriptorCount = 1;
    writes[0].pBufferInfo = &bufInfos[0];

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = descSet;
    writes[1].dstBinding = GLOBAL_INSTANCE_BUFFER_BINDING_IDX;
    writes[1].dstArrayElement = 0;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].descriptorCount = 1;
    writes[1].pBufferInfo = &bufInfos[1];

    vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
}

GlobalUniformQ2::~GlobalUniformQ2()
{
    vkDestroyDescriptorPool(device, descPool, nullptr);
    vkDestroyDescriptorSetLayout(device, descSetLayout, nullptr);
}

void GlobalUniformQ2::Upload(VkCommandBuffer cmd, uint32_t frameIndex, const ShGlobalUniform *src)
{
    CmdLabel label(cmd, "Copying Q2RTX uniform");

    QVKUniformBuffer_t ubo;
    FillUniformBuffer(ubo, *src);

    const VkDeviceSize instanceOffset =
        (sizeof(QVKUniformBuffer_t) + Q2_UBO_ALIGNMENT - 1) & ~(Q2_UBO_ALIGNMENT - 1);

    void *dst = buffer->GetMapped(frameIndex);
    memcpy(dst, &ubo, sizeof(ubo));

    // Upload the CPU-side InstanceBuffer if the geometry port has set one,
    // otherwise keep the SSBO zeroed so the binding stays valid.
    uint8_t *instanceDst = static_cast<uint8_t *>(dst) + instanceOffset;
    if (!instanceBufferCpu.empty())
    {
        memcpy(instanceDst, instanceBufferCpu.data(), std::min(instanceBufferCpu.size(), sizeof(InstanceBuffer)));
    }
    else
    {
        memset(instanceDst, 0, sizeof(InstanceBuffer));
    }

    buffer->CopyFromStaging(cmd, frameIndex, instanceOffset + sizeof(InstanceBuffer));
}

void GlobalUniformQ2::SetInstanceBuffer(const void *pData, size_t size)
{
    instanceBufferCpu.assign(static_cast<const uint8_t *>(pData),
                             static_cast<const uint8_t *>(pData) + size);
}

VkDescriptorSet GlobalUniformQ2::GetDescSet() const
{
    return descSet;
}

VkDescriptorSetLayout GlobalUniformQ2::GetDescSetLayout() const
{
    return descSetLayout;
}
