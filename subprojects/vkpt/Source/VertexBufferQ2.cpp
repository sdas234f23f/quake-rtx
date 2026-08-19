#include "VertexBufferQ2.h"

// Q2RTX binding contract: the vendored dual C/GLSL header. In C mode
// (VKPT_SHADER undefined) it defines the VboPrimitive / LightBuffer /
// IqmMatrixBuffer / ToneMappingBuffer / ReadbackBuffer / SunColorBuffer
// structs and the buffer binding indices. constants.h must come first
// (HISTOGRAM_BINS, MAX_LIGHT_STYLES, NUM_LIGHT_STATS_BUFFERS).
#include "../q2rtx-shaders/constants.h"
#include "../q2rtx-shaders/vertex_buffer.h"

#include <vector>

using namespace vkpt;

// Q2RTX sizes the primitive-buffer descriptor array with MAX_MODELS from
// inc/shared/shared.h (8192). Our Quake defines the same value in
// Quake/quakedef.h; keep the two in sync.
#define Q2_MAX_MODELS 8192

VertexBufferQ2::VertexBufferQ2(VkDevice _device, std::shared_ptr<MemoryAllocator> _allocator)
:
    device(_device),
    descPool(VK_NULL_HANDLE),
    descSetLayout(VK_NULL_HANDLE),
    descSet(VK_NULL_HANDLE)
{
    nullBuffer.Init(_allocator, 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "Q2RTX null buffer");

    // Real tone mapping buffer: the histogram / curve / apply shaders read
    // and write it (Q2RTX creates the same buffer in vertex_buffer.c).
    toneMappingBuffer.Init(_allocator, sizeof(ToneMappingBuffer),
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                               VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                           "Q2RTX tone mapping buffer");

    CreateDescriptors();
}

VertexBufferQ2::~VertexBufferQ2()
{
    vkDestroyDescriptorPool(device, descPool, nullptr);
    vkDestroyDescriptorSetLayout(device, descSetLayout, nullptr);

    toneMappingBuffer.Destroy();
    nullBuffer.Destroy();
}

void VertexBufferQ2::CreateDescriptors()
{
    // Exact mirror of Q2RTX vertex_buffer.c vkpt_vertex_buffer_create().
    VkDescriptorSetLayoutBinding bindings[] = {
        {
            .binding         = PRIMITIVE_BUFFER_BINDING_IDX,
            .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = VERTEX_BUFFER_FIRST_MODEL + Q2_MAX_MODELS,
            .stageFlags      = VK_SHADER_STAGE_ALL,
        },
        {
            .binding         = POSITION_BUFFER_BINDING_IDX,
            .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags      = VK_SHADER_STAGE_ALL,
        },
        {
            .binding         = LIGHT_BUFFER_BINDING_IDX,
            .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags      = VK_SHADER_STAGE_ALL,
        },
        {
            .binding         = LIGHT_COUNTS_HISTORY_BUFFER_BINDING_IDX,
            .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = LIGHT_COUNT_HISTORY,
            .stageFlags      = VK_SHADER_STAGE_ALL,
        },
        {
            .binding         = IQM_MATRIX_BUFFER_BINDING_IDX,
            .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags      = VK_SHADER_STAGE_ALL,
        },
        {
            .binding         = READBACK_BUFFER_BINDING_IDX,
            .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags      = VK_SHADER_STAGE_ALL,
        },
        {
            .binding         = TONE_MAPPING_BUFFER_BINDING_IDX,
            .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags      = VK_SHADER_STAGE_ALL,
        },
        {
            .binding         = SUN_COLOR_BUFFER_BINDING_IDX,
            .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags      = VK_SHADER_STAGE_ALL,
        },
        {
            .binding         = SUN_COLOR_UBO_BINDING_IDX,
            .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 1,
            .stageFlags      = VK_SHADER_STAGE_ALL,
        },
        {
            .binding         = LIGHT_STATS_BUFFER_BINDING_IDX,
            .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = NUM_LIGHT_STATS_BUFFERS,
            .stageFlags      = VK_SHADER_STAGE_ALL,
        },
    };

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(std::size(bindings));
    layoutInfo.pBindings = bindings;

    VkResult r = vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &descSetLayout);
    VK_CHECKERROR(r);

    // Pool sizes must cover the primitive array (binding 0) plus every other
    // storage/uniform binding, like Q2RTX does.
    const uint32_t storageCount =
        (VERTEX_BUFFER_FIRST_MODEL + Q2_MAX_MODELS) + // PRIMITIVE_BUFFER
        1 +                                           // POSITION_BUFFER
        1 +                                           // LIGHT_BUFFER
        LIGHT_COUNT_HISTORY +                         // LIGHT_COUNTS_HISTORY
        1 +                                           // IQM_MATRIX_BUFFER
        1 +                                           // READBACK_BUFFER
        1 +                                           // TONE_MAPPING_BUFFER
        1 +                                           // SUN_COLOR_BUFFER
        NUM_LIGHT_STATS_BUFFERS;                      // LIGHT_STATS

    VkDescriptorPoolSize poolSizes[2] = {};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[0].descriptorCount = storageCount;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[1].descriptorCount = 1; // SUN_COLOR_UBO

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

    // Everything points at the 4-byte null buffer for now. For array
    // bindings Vulkan requires pBufferInfo to be an array of descriptorCount
    // entries (a single pointer is only valid for descriptorCount == 1).
    const uint32_t primCount = VERTEX_BUFFER_FIRST_MODEL + Q2_MAX_MODELS;

    std::vector<VkDescriptorBufferInfo> primInfos(primCount);
    std::vector<VkDescriptorBufferInfo> lightCountInfos(LIGHT_COUNT_HISTORY);
    std::vector<VkDescriptorBufferInfo> lightStatsInfos(NUM_LIGHT_STATS_BUFFERS);

    VkDescriptorBufferInfo nullInfo = {};
    nullInfo.buffer = nullBuffer.GetBuffer();
    nullInfo.offset = 0;
    nullInfo.range = VK_WHOLE_SIZE;

    for (auto &info : primInfos)
    {
        info = nullInfo;
    }
    for (auto &info : lightCountInfos)
    {
        info = nullInfo;
    }
    for (auto &info : lightStatsInfos)
    {
        info = nullInfo;
    }

    VkWriteDescriptorSet write = {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = descSet;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;

    // binding 0: the whole primitive array (world, instanced, then models).
    write.dstBinding = PRIMITIVE_BUFFER_BINDING_IDX;
    write.dstArrayElement = 0;
    write.descriptorCount = primCount;
    write.pBufferInfo = primInfos.data();
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    // single-descriptor storage bindings.
    write.descriptorCount = 1;
    write.dstArrayElement = 0;
    write.pBufferInfo = &nullInfo;

    write.dstBinding = POSITION_BUFFER_BINDING_IDX;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    write.dstBinding = LIGHT_BUFFER_BINDING_IDX;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    write.dstBinding = LIGHT_COUNTS_HISTORY_BUFFER_BINDING_IDX;
    write.descriptorCount = LIGHT_COUNT_HISTORY;
    write.pBufferInfo = lightCountInfos.data();
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    write.descriptorCount = 1;
    write.pBufferInfo = &nullInfo;
    write.dstBinding = IQM_MATRIX_BUFFER_BINDING_IDX;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    write.dstBinding = READBACK_BUFFER_BINDING_IDX;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    // TONE_MAPPING_BUFFER points at the real buffer.
    VkDescriptorBufferInfo toneMappingInfo = {};
    toneMappingInfo.buffer = toneMappingBuffer.GetBuffer();
    toneMappingInfo.offset = 0;
    toneMappingInfo.range = sizeof(ToneMappingBuffer);

    write.dstBinding = TONE_MAPPING_BUFFER_BINDING_IDX;
    write.pBufferInfo = &toneMappingInfo;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    write.pBufferInfo = &nullInfo;
    write.dstBinding = SUN_COLOR_BUFFER_BINDING_IDX;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    // SUN_COLOR_UBO is a uniform buffer binding.
    write.dstBinding = SUN_COLOR_UBO_BINDING_IDX;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.dstBinding = LIGHT_STATS_BUFFER_BINDING_IDX;
    write.descriptorCount = NUM_LIGHT_STATS_BUFFERS;
    write.pBufferInfo = lightStatsInfos.data();
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
}

VkDescriptorSet VertexBufferQ2::GetDescSet() const
{
    return descSet;
}

VkDescriptorSetLayout VertexBufferQ2::GetDescSetLayout() const
{
    return descSetLayout;
}
