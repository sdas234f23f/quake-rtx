#include "ASManagerQ2.h"

#include "CommandBufferManager.h"
#include "GeometryQ2.h"
#include "GlobalUniformQ2.h"
#include "PhysicalDevice.h"
#include "Utils.h"

// Q2RTX binding contract: constants.h defines TLAS_COUNT / AS_FLAG_* /
// SBTO_*, global_ubo.h defines ModelInstance / InstanceBuffer in C mode,
// vertex_buffer.h defines VERTEX_BUFFER_WORLD.
#include "../q2rtx-shaders/constants.h"
#include "../q2rtx-shaders/global_ubo.h"
#include "../q2rtx-shaders/vertex_buffer.h"

#include <cstring>
#include <vector>

using namespace vkpt;

// The 64-byte TLAS instance record used by Q2RTX (see path_tracer.c,
// QvkGeometryInstance_t). Matches VkAccelerationStructureInstanceKHR's
// transform+id+mask+offset+flags layout except the accel is a device address.
struct QvkGeometryInstance
{
    float transform[12];
    uint32_t instance_id : 24;
    uint32_t mask : 8;
    uint32_t instance_offset : 24;
    uint32_t flags : 8;
    VkDeviceAddress acceleration_structure;
};
static_assert(sizeof(QvkGeometryInstance) == 64, "Q2RTX TLAS instance must be 64 bytes");

namespace
{

// Column-major identity, matching what Q2RTX writes into QvkGeometryInstance.
const float IDENTITY_12[12] =
{
    1.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f,
    0.0f, 0.0f, 1.0f,
    0.0f, 0.0f, 0.0f,
};

} // namespace

ASManagerQ2::ASManagerQ2(VkDevice _device,
                         std::shared_ptr<PhysicalDevice> physDevice,
                         std::shared_ptr<MemoryAllocator> _allocator,
                         std::shared_ptr<CommandBufferManager> _cmdManager,
                         std::shared_ptr<GeometryQ2> _geometryQ2,
                         std::shared_ptr<GlobalUniformQ2> _uniformQ2)
: device(_device),
  allocator(std::move(_allocator)),
  cmdManager(std::move(_cmdManager)),
  geometryQ2(std::move(_geometryQ2)),
  uniformQ2(std::move(_uniformQ2)),
  blas(_device, static_cast<VertexCollectorFilterTypeFlags>(VertexCollectorFilterTypeFlagBits::CF_STATIC_NON_MOVABLE)),
  tlasGeometry(_device, "Q2RTX geometry TLAS"),
  tlasEffects(_device, "Q2RTX effects TLAS"),
  descPool(VK_NULL_HANDLE),
  descSetLayout(VK_NULL_HANDLE),
  descSet(VK_NULL_HANDLE),
  fence(VK_NULL_HANDLE),
  worldPrimCount(0),
  submitted(false)
{
    const uint32_t scratchAlignment = physDevice->GetASProperties().minAccelerationStructureScratchOffsetAlignment;
    scratchBuffer = std::make_shared<ScratchBuffer>(allocator, scratchAlignment);
    asBuilder = std::make_shared<ASBuilder>(device, scratchBuffer);

    // TLAS instance records, host-visible like Q2RTX buf_instances. Two
    // slots: slot 0 = geometry (world), slot 1 = effects (never hit).
    instanceBuffer.Init(allocator, 2 * sizeof(QvkGeometryInstance),
                        VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        "Q2RTX TLAS instance buffer");

    VkFenceCreateInfo fenceInfo = {};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkResult r = vkCreateFence(device, &fenceInfo, nullptr, &fence);
    VK_CHECKERROR(r);

    CreateDescSet();
}

ASManagerQ2::~ASManagerQ2()
{
    blas.Destroy();
    tlasGeometry.Destroy();
    tlasEffects.Destroy();

    for (auto &view : texelViews)
    {
        if (view != VK_NULL_HANDLE)
        {
            vkDestroyBufferView(device, view, nullptr);
        }
    }
    for (auto &buf : texelBuffers)
    {
        buf.Destroy();
    }

    instanceBuffer.Destroy();
    vkDestroyDescriptorPool(device, descPool, nullptr);
    vkDestroyDescriptorSetLayout(device, descSetLayout, nullptr);
    vkDestroyFence(device, fence, nullptr);
}

void ASManagerQ2::SubmitStatic()
{
    worldPrimCount = geometryQ2->GetWorldPrimitiveCount();
    if (worldPrimCount == 0 || !geometryQ2->GetWorldBuffer())
    {
        return;
    }

    VkCommandBuffer cmd = cmdManager->StartGraphicsCmd();

    BuildBLAS(cmd);
    BuildTLAS(cmd);

    cmdManager->Submit(cmd, fence);
    Utils::WaitAndResetFence(device, fence);

    // Point set 0 binding 0 at the two TLAS handles.
    const VkAccelerationStructureKHR tlasHandles[TLAS_COUNT] =
    {
        tlasGeometry.GetAS(),
        tlasEffects.GetAS(),
    };

    VkWriteDescriptorSetAccelerationStructureKHR accelWrite = {};
    accelWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    accelWrite.accelerationStructureCount = TLAS_COUNT;
    accelWrite.pAccelerationStructures = tlasHandles;

    VkWriteDescriptorSet write = {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = descSet;
    write.dstBinding = 0;
    write.dstArrayElement = 0;
    write.descriptorCount = TLAS_COUNT;
    write.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    write.pNext = &accelWrite;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    // Texel buffer placeholders (bindings 1..4).
    for (uint32_t i = 0; i < 4; i++)
    {
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.pNext = nullptr;
        write.dstSet = descSet;
        write.dstBinding = i + 1;
        write.dstArrayElement = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
        write.pTexelBufferView = &texelViews[i];
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }

    FillInstanceBuffer();
    submitted = true;
}

void ASManagerQ2::BuildBLAS(VkCommandBuffer cmd)
{
    const VkDeviceSize posOffset = geometryQ2->GetWorldPositionOffset();

    VkAccelerationStructureGeometryKHR geom = {};
    geom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geom.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    // Non-indexed: positions are 3 floats per vertex, one triangle = 3 verts.
    geom.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    geom.geometry.triangles.vertexData.deviceAddress = geometryQ2->GetWorldBufferAddress() + posOffset;
    geom.geometry.triangles.vertexStride = 3 * sizeof(float);
    geom.geometry.triangles.maxVertex = worldPrimCount * 3 - 1;
    geom.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;

    const uint32_t primCount = worldPrimCount;
    VkAccelerationStructureBuildSizesInfoKHR buildSizes =
        asBuilder->GetBottomBuildSizes(1, &geom, &primCount, true);

    blas.RecreateIfNotValid(buildSizes, allocator);
    if (!blas.IsValid(buildSizes))
    {
        return;
    }

    VkAccelerationStructureBuildRangeInfoKHR range = {};
    range.primitiveCount = worldPrimCount;
    range.firstVertex = 0;
    range.primitiveOffset = 0;
    range.transformOffset = 0;

    assert(asBuilder->IsEmpty());
    asBuilder->AddBLAS(blas.GetAS(), 1, &geom, &range, buildSizes, true, false, false);
    asBuilder->BuildBottomLevel(cmd);
    Utils::ASBuildMemoryBarrier(cmd);
}

void ASManagerQ2::BuildTLAS(VkCommandBuffer cmd)
{
    const VkDeviceAddress blasAddress = blas.GetASAddress();

    // Geometry TLAS instance: the whole world, opaque, SBT offset for opaque.
    QvkGeometryInstance *instances = static_cast<QvkGeometryInstance *>(instanceBuffer.Map());
    if (!instances)
    {
        return;
    }

    QvkGeometryInstance &geomInst = instances[0];
    memcpy(geomInst.transform, IDENTITY_12, sizeof(IDENTITY_12));
    geomInst.instance_id = VERTEX_BUFFER_WORLD; // == 0, the world primitive buffer
    geomInst.mask = AS_FLAG_OPAQUE;
    geomInst.instance_offset = SBTO_OPAQUE;
    geomInst.flags = VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;
    geomInst.acceleration_structure = blasAddress;

    // Effects TLAS slot: same BLAS, but zero mask so rays never hit it.
    QvkGeometryInstance &effectInst = instances[1];
    memcpy(effectInst.transform, IDENTITY_12, sizeof(IDENTITY_12));
    effectInst.instance_id = 0;
    effectInst.mask = 0;
    effectInst.instance_offset = SBTO_OPAQUE;
    effectInst.flags = 0;
    effectInst.acceleration_structure = blasAddress;

    instanceBuffer.Unmap();

    // --- geometry TLAS ---
    VkAccelerationStructureGeometryKHR topGeom = {};
    topGeom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    topGeom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    topGeom.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    topGeom.geometry.instances.arrayOfPointers = VK_FALSE;
    topGeom.geometry.instances.data.deviceAddress = instanceBuffer.GetAddress();

    const uint32_t instCount = 1;
    VkAccelerationStructureBuildSizesInfoKHR buildSizes =
        asBuilder->GetTopBuildSizes(&topGeom, instCount, false);

    tlasGeometry.RecreateIfNotValid(buildSizes, allocator);

    VkAccelerationStructureBuildRangeInfoKHR range = {};
    range.primitiveCount = instCount;
    range.firstVertex = 0;
    range.primitiveOffset = 0;
    range.transformOffset = 0;

    assert(asBuilder->IsEmpty());
    asBuilder->AddTLAS(tlasGeometry.GetAS(), &topGeom, &range, buildSizes, false, false);
    asBuilder->BuildTopLevel(cmd);
    Utils::ASBuildMemoryBarrier(cmd);

    // --- effects TLAS (one never-hit instance) ---
    VkAccelerationStructureBuildSizesInfoKHR effectsSizes =
        asBuilder->GetTopBuildSizes(&topGeom, instCount, false);

    tlasEffects.RecreateIfNotValid(effectsSizes, allocator);

    VkAccelerationStructureBuildRangeInfoKHR effectsRange = {};
    effectsRange.primitiveCount = instCount;
    effectsRange.firstVertex = 0;
    effectsRange.primitiveOffset = 1 * sizeof(QvkGeometryInstance);
    effectsRange.transformOffset = 0;

    assert(asBuilder->IsEmpty());
    asBuilder->AddTLAS(tlasEffects.GetAS(), &topGeom, &effectsRange, effectsSizes, false, false);
    asBuilder->BuildTopLevel(cmd);
    Utils::ASBuildMemoryBarrier(cmd);
}

void ASManagerQ2::FillInstanceBuffer()
{
    // InstanceBuffer is ~1.7 MB (8192 model instances x 192 B), so it must
    // live on the heap, not the stack.
    std::vector<uint8_t> instanceData(sizeof(InstanceBuffer), 0);
    InstanceBuffer &inst = *reinterpret_cast<InstanceBuffer *>(instanceData.data());

    // One model instance for the world: identity transform (positions are
    // already world-space), primitive buffer = VERTEX_BUFFER_WORLD.
    ModelInstance &mi = inst.model_instances[0];

    for (int c = 0; c < 4; c++)
    {
        for (int r = 0; r < 4; r++)
        {
            mi.transform[c][r] = (c == r) ? 1.0f : 0.0f;
            mi.transform_prev[c][r] = (c == r) ? 1.0f : 0.0f;
        }
    }

    mi.material = 0; // default material until G6
    mi.shell = 0;
    mi.cluster = 0;
    mi.source_buffer_idx = VERTEX_BUFFER_WORLD;
    mi.prim_count = worldPrimCount;
    mi.prim_offset_curr_pose_curr_frame = 0;
    mi.prim_offset_prev_pose_curr_frame = 0;
    mi.prim_offset_curr_pose_prev_frame = 0;
    mi.prim_offset_prev_pose_prev_frame = 0;
    mi.pose_lerp_curr_frame = 0.0f;
    mi.pose_lerp_prev_frame = 0.0f;
    mi.iqm_matrix_offset_curr_frame = -1;
    mi.iqm_matrix_offset_prev_frame = -1;
    mi.alpha_and_frame = 0x3C00; // half(1.0) alpha, frame 0
    mi.render_buffer_idx = VERTEX_BUFFER_WORLD;
    mi.render_prim_offset = 0;

    // TLAS instance bookkeeping: instance 0 is the world (model index -1
    // means "world path" in the hit shaders, prim offset taken from the
    // tlas_instance_prim_offsets array).
    inst.tlas_instance_prim_offsets[0] = 0;
    inst.tlas_instance_model_indices[0] = -1;
    inst.tlas_instance_prim_offsets[1] = 0;
    inst.tlas_instance_model_indices[1] = -1;

    uniformQ2->SetInstanceBuffer(instanceData.data(), instanceData.size());
}

void ASManagerQ2::CreateDescSet()
{
    // Set 0 layout: TLAS array + 4 texel buffers (particle color, beam
    // color, sprite info, beam intersect), mirroring Q2RTX path_tracer.c.
    const uint32_t RT_TLAS_BINDING = 0;
    const uint32_t RT_PARTICLE_COLOR_BINDING = 1;
    const uint32_t RT_BEAM_COLOR_BINDING = 2;
    const uint32_t RT_SPRITE_INFO_BINDING = 3;
    const uint32_t RT_BEAM_INTERSECT_BINDING = 4;

    VkDescriptorSetLayoutBinding bindings[5] = {};
    bindings[0].binding = RT_TLAS_BINDING;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    bindings[0].descriptorCount = TLAS_COUNT;
    bindings[0].stageFlags = VK_SHADER_STAGE_ALL;

    for (uint32_t b = 1; b < 5; b++)
    {
        bindings[b].binding = b;
        bindings[b].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
        bindings[b].descriptorCount = 1;
        bindings[b].stageFlags = VK_SHADER_STAGE_ALL;
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 5;
    layoutInfo.pBindings = bindings;

    VkResult r = vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &descSetLayout);
    VK_CHECKERROR(r);

    VkDescriptorPoolSize poolSizes[2] = {};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    poolSizes[0].descriptorCount = TLAS_COUNT;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
    poolSizes[1].descriptorCount = 4;

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

    // Placeholder texel buffers: one 4-byte storage buffer per binding with
    // a matching buffer view. Filled with real particle/beam/sprite data in
    // a later stage.
    VkBufferUsageFlags texelUsage = VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    for (uint32_t i = 0; i < 4; i++)
    {
        texelBuffers[i].Init(allocator, 4, texelUsage,
                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                             "Q2RTX texel buffer placeholder");

        VkBufferViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
        viewInfo.buffer = texelBuffers[i].GetBuffer();
        viewInfo.format = VK_FORMAT_R32_UINT;
        viewInfo.offset = 0;
        viewInfo.range = 4;

        r = vkCreateBufferView(device, &viewInfo, nullptr, &texelViews[i]);
        VK_CHECKERROR(r);
    }
}

VkDescriptorSet ASManagerQ2::GetDescSet() const
{
    return descSet;
}

VkDescriptorSetLayout ASManagerQ2::GetDescSetLayout() const
{
    return descSetLayout;
}
