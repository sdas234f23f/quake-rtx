#include "GlobalUniformQ2.h"

// Q2RTX binding contract: the vendored dual C/GLSL header. In C mode
// (VKPT_SHADER undefined) it defines QVKUniformBuffer_t whose layout is
// verified to match GLSL std140 by Tools/check_q2rtx_ubo.py.
#include "../q2rtx-shaders/global_ubo.h"

#include "GlobalUniform.h"
#include "Generated/ShaderCommonC.h"
#include "CmdLabel.h"

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
}

} // namespace

GlobalUniformQ2::GlobalUniformQ2(VkDevice _device, std::shared_ptr<MemoryAllocator> _allocator)
:
    device(_device),
    descPool(VK_NULL_HANDLE),
    descSetLayout(VK_NULL_HANDLE),
    descSet(VK_NULL_HANDLE)
{
    // Align the buffer size up to a typical minUniformBufferOffsetAlignment.
    const VkDeviceSize bufferSize = (sizeof(QVKUniformBuffer_t) + 255) & ~static_cast<VkDeviceSize>(255);

    buffer = std::make_shared<AutoBuffer>(_device, _allocator);
    buffer->Create(bufferSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, "Q2RTX uniform buffer");

    CreateDescriptors();
}

void GlobalUniformQ2::CreateDescriptors()
{
    VkDescriptorSetLayoutBinding binding = {};
    binding.binding = GLOBAL_UBO_BINDING_IDX;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_ALL;

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;

    VkResult r = vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &descSetLayout);
    VK_CHECKERROR(r);

    VkDescriptorPoolSize poolSize = {};
    poolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSize.descriptorCount = 1;

    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
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

    VkDescriptorBufferInfo bufInfo = {};
    bufInfo.buffer = buffer->GetDeviceLocal();
    bufInfo.offset = 0;
    bufInfo.range = sizeof(QVKUniformBuffer_t);

    VkWriteDescriptorSet write = {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = descSet;
    write.dstBinding = GLOBAL_UBO_BINDING_IDX;
    write.dstArrayElement = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.descriptorCount = 1;
    write.pBufferInfo = &bufInfo;

    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
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

    void *dst = buffer->GetMapped(frameIndex);
    memcpy(dst, &ubo, sizeof(ubo));

    buffer->CopyFromStaging(cmd, frameIndex, sizeof(QVKUniformBuffer_t));
}

VkDescriptorSet GlobalUniformQ2::GetDescSet() const
{
    return descSet;
}

VkDescriptorSetLayout GlobalUniformQ2::GetDescSetLayout() const
{
    return descSetLayout;
}
