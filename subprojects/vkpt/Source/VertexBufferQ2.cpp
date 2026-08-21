#include "VertexBufferQ2.h"

// Q2RTX binding contract: the vendored dual C/GLSL header. In C mode
// (VKPT_SHADER undefined) it defines the VboPrimitive / LightBuffer /
// IqmMatrixBuffer / ToneMappingBuffer / ReadbackBuffer / SunColorBuffer
// structs and the buffer binding indices. constants.h must come first
// (HISTOGRAM_BINS, MAX_LIGHT_STYLES, NUM_LIGHT_STATS_BUFFERS).
#include "../q2rtx-shaders/constants.h"
#include "../q2rtx-shaders/vertex_buffer.h"

#include <cstring>
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

    readbackBuffer.Init(_allocator, sizeof(ReadbackBuffer),
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                        "Q2RTX readback buffer");

    // Q2RTX binds the same sun-color buffer as storage AND as a UBO
    // (sky_buffer_resolve.comp fills it, shaders read it as sun_color_ubo).
    // Host-visible so the accumulation can be seeded on the CPU (G5 fake
    // sun until the sky port).
    sunColorBuffer.Init(_allocator, sizeof(SunColorBuffer),
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        "Q2RTX sun color buffer");

    // Real LightBuffer (material table + light lists). Only the default
    // material and the sky-visibility mask are filled for now; the light
    // lists come with the light port.
    lightBuffer.Init(_allocator, sizeof(LightBuffer),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     "Q2RTX light buffer");

    FillLightBuffer();
    FillSunColor();

    CreateDescriptors();
}

VertexBufferQ2::~VertexBufferQ2()
{
    vkDestroyDescriptorPool(device, descPool, nullptr);
    vkDestroyDescriptorSetLayout(device, descSetLayout, nullptr);

    sunColorBuffer.Destroy();
    lightBuffer.Destroy();
    readbackBuffer.Destroy();
    toneMappingBuffer.Destroy();
    nullBuffer.Destroy();
}

// IEEE-754 half packing, GLSL unpackHalf2x16 layout: x = low 16 bits,
// y = high 16 bits. Only used with the fixed default-material constants.
static uint32_t PackHalf2x16(uint16_t x, uint16_t y)
{
    return (static_cast<uint32_t>(y) << 16) | x;
}

void VertexBufferQ2::FillLightBuffer()
{
    void *mapped = lightBuffer.Map();
    if (!mapped)
    {
        return;
    }
    memset(mapped, 0, sizeof(LightBuffer));

    LightBuffer *lb = static_cast<LightBuffer *>(mapped);

    // Default PBR material at index 1 (geometry uses material_id = 1 until
    // the real material table port): white, dielectric, rough.
    // half: 0.0 = 0x0000, 0.5 = 0x3800, 1.0 = 0x3C00.
    uint32_t *mat = &lb->material_table[1 * MATERIAL_UINTS];
    mat[0] = 0;                                   // base_texture 0 (white placeholder), no normals
    mat[1] = 0;                                   // no emissive / mask texture
    mat[2] = PackHalf2x16(0x0000, 0x3C00);        // bump_scale 0, roughness_override 1
    mat[3] = PackHalf2x16(0x0000, 0x0000);        // metalness 0, emissive_factor 0
    mat[4] = 1;                                   // num_frames 1
    mat[5] = PackHalf2x16(0x3800, 0x3C00);        // specular_factor 0.5, base_factor 1

    // Sky visibility: mark every BSP cluster as seeing the sky (G5
    // placeholder so the sun light passes the visibility test).
    for (size_t i = 0; i < MAX_LIGHT_LISTS / 32; i++)
    {
        lb->sky_visibility[i] = ~0u;
    }

    lightBuffer.Unmap();
}

void VertexBufferQ2::SetQ2Materials(const uint32_t *entries, uint32_t count)
{
    if (!entries || count == 0)
    {
        return;
    }

    // Entries are indexed from 2 in the geometry material ids (0 = empty,
    // 1 = default white); overwrite only that tail of the table.
    const uint32_t maxCount = MAX_PBR_MATERIALS - 2;
    if (count > maxCount)
    {
        count = maxCount;
    }

    void *mapped = lightBuffer.Map();
    if (!mapped)
    {
        return;
    }

    LightBuffer *lb = static_cast<LightBuffer *>(mapped);
    std::memcpy(lb->material_table + 2 * MATERIAL_UINTS, entries,
                static_cast<size_t>(count) * MATERIAL_UINTS * sizeof(uint32_t));

    lightBuffer.Unmap();
}

void VertexBufferQ2::FillSunColor()
{
    void *mapped = sunColorBuffer.Map();
    if (!mapped)
    {
        return;
    }
    memset(mapped, 0, sizeof(SunColorBuffer));

    SunColorBuffer *sun = static_cast<SunColorBuffer *>(mapped);

    // G5 fake sun accumulation: sky_buffer_resolve.comp converts this to
    // sun_color = accum / SUN_COLOR_ACCUMULATOR_FIXED_POINT_SCALE (then
    // scales by pt_env_scale from the UBO). Warm white sun.
    const int scale = SUN_COLOR_ACCUMULATOR_FIXED_POINT_SCALE;
    const float sunColor[3] = { 1.0f, 0.95f, 0.85f };
    sun->accum_sun_color[0] = static_cast<int>(sunColor[0] * scale);
    sun->accum_sun_color[1] = static_cast<int>(sunColor[1] * scale);
    sun->accum_sun_color[2] = static_cast<int>(sunColor[2] * scale);

    sunColorBuffer.Unmap();
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
    {
        VkDescriptorBufferInfo lightInfo = {};
        lightInfo.buffer = lightBuffer.GetBuffer();
        lightInfo.offset = 0;
        lightInfo.range = sizeof(LightBuffer);
        write.pBufferInfo = &lightInfo;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }

    write.dstBinding = LIGHT_COUNTS_HISTORY_BUFFER_BINDING_IDX;
    write.descriptorCount = LIGHT_COUNT_HISTORY;
    write.pBufferInfo = lightCountInfos.data();
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    write.descriptorCount = 1;
    write.pBufferInfo = &nullInfo;
    write.dstBinding = IQM_MATRIX_BUFFER_BINDING_IDX;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    // READBACK_BUFFER points at the real buffer.
    VkDescriptorBufferInfo readbackInfo = {};
    readbackInfo.buffer = readbackBuffer.GetBuffer();
    readbackInfo.offset = 0;
    readbackInfo.range = sizeof(ReadbackBuffer);

    write.dstBinding = READBACK_BUFFER_BINDING_IDX;
    write.pBufferInfo = &readbackInfo;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    write.pBufferInfo = &nullInfo;

    // TONE_MAPPING_BUFFER points at the real buffer.
    VkDescriptorBufferInfo toneMappingInfo = {};
    toneMappingInfo.buffer = toneMappingBuffer.GetBuffer();
    toneMappingInfo.offset = 0;
    toneMappingInfo.range = sizeof(ToneMappingBuffer);

    write.dstBinding = TONE_MAPPING_BUFFER_BINDING_IDX;
    write.pBufferInfo = &toneMappingInfo;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    // SUN_COLOR_UBO is a uniform buffer binding; Q2RTX points both at the
    // same sun-color buffer.
    VkDescriptorBufferInfo sunColorInfo = {};
    sunColorInfo.buffer = sunColorBuffer.GetBuffer();
    sunColorInfo.offset = 0;
    sunColorInfo.range = sizeof(SunColorBuffer);

    write.dstBinding = SUN_COLOR_UBO_BINDING_IDX;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.pBufferInfo = &sunColorInfo;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    // SUN_COLOR_BUFFER (storage) also points at the real buffer.
    write.dstBinding = SUN_COLOR_BUFFER_BINDING_IDX;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &sunColorInfo;
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

void VertexBufferQ2::SetWorldBufferInfo(const VkDescriptorBufferInfo &primInfo,
                                        const VkDescriptorBufferInfo &posInfo)
{
    VkWriteDescriptorSet write = {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = descSet;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.descriptorCount = 1;

    // binding 0 element VERTEX_BUFFER_WORLD (0) -> primitive array.
    write.dstBinding = PRIMITIVE_BUFFER_BINDING_IDX;
    write.dstArrayElement = VERTEX_BUFFER_WORLD;
    write.pBufferInfo = &primInfo;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    // binding 1 -> BLAS source positions.
    write.dstBinding = POSITION_BUFFER_BINDING_IDX;
    write.dstArrayElement = 0;
    write.pBufferInfo = &posInfo;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
}
