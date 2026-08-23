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

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace vkpt;

// Temporary diagnostics (dynamic-geometry visibility): append to the same file
// GeometryQ2 uses so the whole dynamic path can be traced in one place.
static void DbgAS(const char *fmt, ...)
{
    static FILE *file = nullptr;
    if (file == nullptr)
    {
        file = fopen("q2rt_dynamic_dbg.txt", "a");
    }
    if (file == nullptr)
    {
        return;
    }
    va_list args;
    va_start(args, fmt);
    vfprintf(file, fmt, args);
    va_end(args);
    fflush(file);
}

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

// Identity for the VkTransformMatrixKHR instance transform: 3 rows x 4
// columns, exactly like Q2RTX writes into QvkGeometryInstance.
// (The previous layout - four groups of three - collapsed every triangle
// onto the line y = z = x, so no ray could ever hit anything.)
const float IDENTITY_12[12] =
{
    1.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f,
};

constexpr std::array<
    GeometryQ2::DynamicGeometryCategory,
    GeometryQ2::DYNAMIC_GEOMETRY_CATEGORY_COUNT>
    DYNAMIC_CATEGORIES =
{
    GeometryQ2::DynamicGeometryCategory::World,
    GeometryQ2::DynamicGeometryCategory::ViewerWeapon,
    GeometryQ2::DynamicGeometryCategory::ViewerModel,
};

uint32_t GetDynamicVisibilityMask(GeometryQ2::DynamicGeometryCategory category)
{
    switch (category)
    {
        case GeometryQ2::DynamicGeometryCategory::World:
            return AS_FLAG_OPAQUE;
        case GeometryQ2::DynamicGeometryCategory::ViewerWeapon:
            return AS_FLAG_VIEWER_WEAPON;
        case GeometryQ2::DynamicGeometryCategory::ViewerModel:
            return AS_FLAG_VIEWER_MODELS;
        case GeometryQ2::DynamicGeometryCategory::Count:
            break;
    }

    assert(false);
    return 0;
}

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
  blas(_device, static_cast<VertexCollectorFilterTypeFlags>(
                    VertexCollectorFilterTypeFlagBits::CF_STATIC_NON_MOVABLE |
                    VertexCollectorFilterTypeFlagBits::PT_OPAQUE)),
  transparentBlas(_device, static_cast<VertexCollectorFilterTypeFlags>(
                    VertexCollectorFilterTypeFlagBits::CF_STATIC_NON_MOVABLE |
                    VertexCollectorFilterTypeFlagBits::PT_OPAQUE)),
  tlasEffects(_device, "Q2RTX effects TLAS"),
  descPool(VK_NULL_HANDLE),
  descSetLayout(VK_NULL_HANDLE),
  descSets{},
  activeFrameIndex(0),
  fence(VK_NULL_HANDLE),
  worldPrimCount(0),
  transparentPrimCount(0),
  submitted(false)
{
    const uint32_t scratchAlignment = physDevice->GetASProperties().minAccelerationStructureScratchOffsetAlignment;
    staticScratchBuffer = std::make_shared<ScratchBuffer>(allocator, scratchAlignment);
    staticAsBuilder = std::make_shared<ASBuilder>(device, staticScratchBuffer);

    // Each frame slot owns its scratch allocator and builder. BeginFrame has
    // already waited the same slot's fence before SubmitDynamic resets it.
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        dynamicScratchBuffers[i] =
            std::make_shared<ScratchBuffer>(allocator, scratchAlignment);
        dynamicAsBuilders[i] =
            std::make_shared<ASBuilder>(device, dynamicScratchBuffers[i]);

        for (auto &categoryBlas : dynamicBlas[i])
        {
            categoryBlas = std::make_unique<BLASComponent>(
                device, static_cast<VertexCollectorFilterTypeFlags>(
                            VertexCollectorFilterTypeFlagBits::CF_DYNAMIC |
                            VertexCollectorFilterTypeFlagBits::PT_OPAQUE));
        }
        tlasGeometry[i] = std::make_unique<TLASComponent>(device, "Q2RTX geometry TLAS");
    }

    effectsInstanceBuffer.Init(allocator, sizeof(QvkGeometryInstance),
                        VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        "Q2RTX effects TLAS instance buffer");

    VkFenceCreateInfo fenceInfo = {};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkResult r = vkCreateFence(device, &fenceInfo, nullptr, &fence);
    VK_CHECKERROR(r);

    CreateDescSet();
}

ASManagerQ2::~ASManagerQ2()
{
    blas.Destroy();
    transparentBlas.Destroy();
    for (auto &tlas : tlasGeometry)
    {
        if (tlas)
        {
            tlas->Destroy();
        }
    }
    tlasEffects.Destroy();
    for (auto &frameBlas : dynamicBlas)
    {
        for (auto &b : frameBlas)
        {
            if (b)
            {
                b->Destroy();
            }
        }
    }

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

    for (auto &buf : instanceBuffer)
    {
        buf.Destroy();
    }
    effectsInstanceBuffer.Destroy();
    vkDestroyDescriptorPool(device, descPool, nullptr);
    vkDestroyDescriptorSetLayout(device, descSetLayout, nullptr);
    vkDestroyFence(device, fence, nullptr);
}

void ASManagerQ2::SubmitStatic()
{
    worldPrimCount = geometryQ2->GetWorldPrimitiveCount();
    transparentPrimCount = geometryQ2->GetTransparentPrimitiveCount();

    if (worldPrimCount == 0 || !geometryQ2->GetWorldBuffer())
    {
        return;
    }

    VkCommandBuffer cmd = cmdManager->StartGraphicsCmd();

    staticScratchBuffer->Reset();
    BuildBLAS(cmd);
    if (transparentPrimCount > 0)
    {
        BuildTransparentBLAS(cmd);
    }
    BuildTLAS(cmd);

    cmdManager->Submit(cmd, fence);
    Utils::WaitAndResetFence(device, fence);

    // Texel buffer placeholders and the effects TLAS entry are frame
    // invariant; write them into every ring slot's descriptor set once. The
    // geometry TLAS entry is filled per frame by SubmitDynamic below.
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        for (uint32_t b = 0; b < 4; b++)
        {
            VkWriteDescriptorSet write = {};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = descSets[i];
            write.dstBinding = b + 1;
            write.dstArrayElement = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
            write.pTexelBufferView = &texelViews[b];
            vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        }
    }

    submitted = true;
}

void ASManagerQ2::BuildStaticWorldBLAS(VkCommandBuffer cmd, BLASComponent &target,
                                       VkDeviceAddress positionAddress,
                                       uint32_t primCount)
{
    VkAccelerationStructureGeometryKHR geom = {};
    geom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geom.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    // Non-indexed: positions are 3 floats per vertex, one triangle = 3 verts.
    geom.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    geom.geometry.triangles.vertexData.deviceAddress = positionAddress;
    geom.geometry.triangles.vertexStride = 3 * sizeof(float);
    geom.geometry.triangles.maxVertex = primCount * 3 - 1;
    geom.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;

    VkAccelerationStructureBuildSizesInfoKHR buildSizes =
        staticAsBuilder->GetBottomBuildSizes(1, &geom, &primCount, true);

    target.RecreateIfNotValid(buildSizes, allocator);
    if (!target.IsValid(buildSizes))
    {
        return;
    }

    VkAccelerationStructureBuildRangeInfoKHR range = {};
    range.primitiveCount = primCount;
    range.firstVertex = 0;
    range.primitiveOffset = 0;
    range.transformOffset = 0;

    assert(staticAsBuilder->IsEmpty());
    staticAsBuilder->AddBLAS(target.GetAS(), 1, &geom, &range,
                             buildSizes, true, false, false);
    staticAsBuilder->BuildBottomLevel(cmd);
    Utils::ASBuildToBuildMemoryBarrier(cmd);
}

void ASManagerQ2::BuildBLAS(VkCommandBuffer cmd)
{
    BuildStaticWorldBLAS(
        cmd, blas,
        geometryQ2->GetWorldBufferAddress() + geometryQ2->GetWorldPositionOffset(),
        worldPrimCount);
}

void ASManagerQ2::BuildTransparentBLAS(VkCommandBuffer cmd)
{
    BuildStaticWorldBLAS(
        cmd, transparentBlas,
        geometryQ2->GetWorldBufferAddress() +
            geometryQ2->GetTransparentPositionOffset(),
        transparentPrimCount);
}

void ASManagerQ2::BuildDynamicBLAS(
    VkCommandBuffer cmd, uint32_t frameIndex,
    GeometryQ2::DynamicGeometryCategory category,
    const GeometryQ2::DynamicGeometryRange &range)
{
    VkAccelerationStructureGeometryKHR geom = {};
    geom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geom.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    geom.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    geom.geometry.triangles.vertexData.deviceAddress =
        geometryQ2->GetDynamicBufferAddress(frameIndex) + range.positionOffset;
    geom.geometry.triangles.vertexStride = 3 * sizeof(float);
    geom.geometry.triangles.maxVertex = range.primitiveCount * 3 - 1;
    geom.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;

    ASBuilder &builder = *dynamicAsBuilders[frameIndex];
    const uint32_t primCount = range.primitiveCount;
    VkAccelerationStructureBuildSizesInfoKHR buildSizes =
        builder.GetBottomBuildSizes(1, &geom, &primCount, true);

    BLASComponent &dynBlas =
        *dynamicBlas[frameIndex][static_cast<size_t>(category)];
    dynBlas.RecreateIfNotValid(buildSizes, allocator);
    if (!dynBlas.IsValid(buildSizes))
    {
        return;
    }

    VkAccelerationStructureBuildRangeInfoKHR buildRange = {};
    buildRange.primitiveCount = primCount;

    assert(builder.IsEmpty());
    builder.AddBLAS(dynBlas.GetAS(), 1, &geom, &buildRange,
                    buildSizes, true, false, false);
    builder.BuildBottomLevel(cmd);
}

void ASManagerQ2::BuildTLAS(VkCommandBuffer cmd)
{
    const VkDeviceAddress blasAddress = blas.GetASAddress();

    // Effects TLAS slot: same static BLAS, but zero mask so rays never hit
    // it. Built once here, at level load; never touched again.
    QvkGeometryInstance *effectInstances = static_cast<QvkGeometryInstance *>(effectsInstanceBuffer.Map());
    if (!effectInstances)
    {
        return;
    }

    QvkGeometryInstance &effectInst = effectInstances[0];
    memcpy(effectInst.transform, IDENTITY_12, sizeof(IDENTITY_12));
    effectInst.instance_id = 0;
    effectInst.mask = 0;
    effectInst.instance_offset = SBTO_OPAQUE;
    effectInst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    effectInst.acceleration_structure = blasAddress;

    effectsInstanceBuffer.Unmap();

    VkAccelerationStructureGeometryKHR topGeom = {};
    topGeom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    topGeom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    topGeom.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    topGeom.geometry.instances.arrayOfPointers = VK_FALSE;
    topGeom.geometry.instances.data.deviceAddress = effectsInstanceBuffer.GetAddress();

    const uint32_t instCount = 1;
    VkAccelerationStructureBuildSizesInfoKHR effectsSizes =
        staticAsBuilder->GetTopBuildSizes(&topGeom, instCount, false);

    tlasEffects.RecreateIfNotValid(effectsSizes, allocator);

    VkAccelerationStructureBuildRangeInfoKHR effectsRange = {};
    effectsRange.primitiveCount = instCount;
    effectsRange.firstVertex = 0;
    effectsRange.primitiveOffset = 0;
    effectsRange.transformOffset = 0;

    assert(staticAsBuilder->IsEmpty());
    staticAsBuilder->AddTLAS(tlasEffects.GetAS(), &topGeom, &effectsRange,
                             effectsSizes, false, false);
    staticAsBuilder->BuildTopLevel(cmd);
    Utils::ASBuildMemoryBarrier(cmd);
}

void ASManagerQ2::BuildCombinedTLAS(VkCommandBuffer cmd, uint32_t frameIndex)
{
    uint32_t instCount = 1;
    if (transparentPrimCount > 0)
    {
        instCount++;
    }
    for (GeometryQ2::DynamicGeometryCategory category : DYNAMIC_CATEGORIES)
    {
        if (geometryQ2->GetDynamicRange(frameIndex, category).primitiveCount > 0)
        {
            instCount++;
        }
    }

    Buffer &instBuf = instanceBuffer[frameIndex];
    constexpr VkDeviceSize maxInstanceBufferSize =
        (2 + GeometryQ2::DYNAMIC_GEOMETRY_CATEGORY_COUNT) *
        sizeof(QvkGeometryInstance);
    if (!instBuf.IsInitted() || instBuf.GetSize() < maxInstanceBufferSize)
    {
        instBuf.Destroy();
        instBuf.Init(
            allocator, maxInstanceBufferSize,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            "Q2RTX dynamic-frame TLAS instance buffer");
    }

    QvkGeometryInstance *instances = static_cast<QvkGeometryInstance *>(instBuf.Map());
    if (!instances)
    {
        return;
    }

    // Route both instances through the masked hit group: materials without
    // a mask accept immediately, alpha-tested albedo can discard cutouts.
    QvkGeometryInstance &worldInst = instances[0];
    memcpy(worldInst.transform, IDENTITY_12, sizeof(IDENTITY_12));
    worldInst.instance_id = VERTEX_BUFFER_WORLD;
    worldInst.mask = AS_FLAG_OPAQUE;
    worldInst.instance_offset = SBTO_MASKED;
    worldInst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    worldInst.acceleration_structure = blas.GetASAddress();

    uint32_t instanceIndex = 1;

    // Transparent (water/slime/glass) world instance: same VERTEX_BUFFER_WORLD
    // primitive buffer as the opaque world, but tlas_instance_prim_offsets
    // points past the opaque range, and AS_FLAG_TRANSPARENT keeps it out of
    // shadow and first-bounce reflection rays.
    if (transparentPrimCount > 0)
    {
        QvkGeometryInstance &transparentInst = instances[instanceIndex++];
        memcpy(transparentInst.transform, IDENTITY_12, sizeof(IDENTITY_12));
        transparentInst.instance_id = VERTEX_BUFFER_WORLD;
        transparentInst.mask = AS_FLAG_TRANSPARENT;
        transparentInst.instance_offset = SBTO_MASKED;
        transparentInst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        transparentInst.acceleration_structure = transparentBlas.GetASAddress();
    }

    for (GeometryQ2::DynamicGeometryCategory category : DYNAMIC_CATEGORIES)
    {
        const GeometryQ2::DynamicGeometryRange range =
            geometryQ2->GetDynamicRange(frameIndex, category);
        if (range.primitiveCount == 0)
        {
            continue;
        }

        QvkGeometryInstance &dynInst = instances[instanceIndex++];
        memcpy(dynInst.transform, IDENTITY_12, sizeof(IDENTITY_12));
        dynInst.instance_id = VERTEX_BUFFER_INSTANCED;
        dynInst.mask = GetDynamicVisibilityMask(category);
        dynInst.instance_offset = SBTO_MASKED;
        dynInst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        dynInst.acceleration_structure =
            dynamicBlas[frameIndex][static_cast<size_t>(category)]->GetASAddress();
    }
    assert(instanceIndex == instCount);

    instBuf.Unmap();

    DbgAS("ASManagerQ2::BuildCombinedTLAS frame=%u instCount=%u\n",
          frameIndex, instCount);

    VkAccelerationStructureGeometryKHR topGeom = {};
    topGeom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    topGeom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    topGeom.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    topGeom.geometry.instances.arrayOfPointers = VK_FALSE;
    topGeom.geometry.instances.data.deviceAddress = instBuf.GetAddress();

    ASBuilder &builder = *dynamicAsBuilders[frameIndex];
    VkAccelerationStructureBuildSizesInfoKHR buildSizes =
        builder.GetTopBuildSizes(&topGeom, instCount, false);

    TLASComponent &tlas = *tlasGeometry[frameIndex];
    tlas.RecreateIfNotValid(buildSizes, allocator);

    VkAccelerationStructureBuildRangeInfoKHR range = {};
    range.primitiveCount = instCount;
    range.firstVertex = 0;
    range.primitiveOffset = 0;
    range.transformOffset = 0;

    assert(builder.IsEmpty());
    builder.AddTLAS(tlas.GetAS(), &topGeom, &range, buildSizes, false, false);
    builder.BuildTopLevel(cmd);
    Utils::ASBuildMemoryBarrier(cmd);

    // Point this frame's descriptor set at the rebuilt geometry TLAS (the
    // effects TLAS entry never changes after SubmitStatic).
    const VkAccelerationStructureKHR tlasHandles[TLAS_COUNT] =
    {
        tlas.GetAS(),
        tlasEffects.GetAS(),
    };

    VkWriteDescriptorSetAccelerationStructureKHR accelWrite = {};
    accelWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    accelWrite.accelerationStructureCount = TLAS_COUNT;
    accelWrite.pAccelerationStructures = tlasHandles;

    VkWriteDescriptorSet write = {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = descSets[frameIndex];
    write.dstBinding = 0;
    write.dstArrayElement = 0;
    write.descriptorCount = TLAS_COUNT;
    write.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    write.pNext = &accelWrite;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
}

void ASManagerQ2::SubmitDynamic(VkCommandBuffer cmd, uint32_t frameIndex)
{
    if (frameIndex >= MAX_FRAMES_IN_FLIGHT || !submitted)
    {
        // No static world yet (no level loaded): nothing to combine, leave
        // HasTLAS() reporting false via the untouched tlasGeometry[frameIndex].
        return;
    }

    dynamicScratchBuffers[frameIndex]->Reset();
    assert(dynamicAsBuilders[frameIndex]->IsEmpty());

    for (GeometryQ2::DynamicGeometryCategory category : DYNAMIC_CATEGORIES)
    {
        const GeometryQ2::DynamicGeometryRange range =
            geometryQ2->GetDynamicRange(frameIndex, category);
        if (range.primitiveCount > 0)
        {
            DbgAS("ASManagerQ2::SubmitDynamic frame=%u cat=%u prims=%u posOff=%llu\n",
                  frameIndex, static_cast<uint32_t>(category), range.primitiveCount,
                  static_cast<unsigned long long>(range.positionOffset));
            BuildDynamicBLAS(cmd, frameIndex, category, range);
        }
    }

    // TLAS construction reads every referenced BLAS. The ray-tracing barrier
    // used after a completed TLAS does not cover this build-to-build hazard.
    Utils::ASBuildToBuildMemoryBarrier(cmd);
    BuildCombinedTLAS(cmd, frameIndex);
    FillInstanceBuffer(frameIndex);

    activeFrameIndex = frameIndex;
}

void ASManagerQ2::FillInstanceBuffer(uint32_t frameIndex)
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

    // Instance 0 is the static world. The transparent world instance (when
    // present) and dynamic instances follow in the same order
    // BuildCombinedTLAS uses, skipping empty ranges.
    inst.tlas_instance_prim_offsets[0] = 0;
    inst.tlas_instance_model_indices[0] = -1;

    uint32_t instanceIndex = 1;
    if (transparentPrimCount > 0)
    {
        // Same VERTEX_BUFFER_WORLD buffer, prim offset just past the opaque
        // range; -1 model index means the hit shader reads
        // tlas_instance_prim_offsets directly instead of model_instances.
        inst.tlas_instance_prim_offsets[instanceIndex] = worldPrimCount;
        inst.tlas_instance_model_indices[instanceIndex] = -1;
        instanceIndex++;
    }

    for (GeometryQ2::DynamicGeometryCategory category : DYNAMIC_CATEGORIES)
    {
        const GeometryQ2::DynamicGeometryRange range =
            geometryQ2->GetDynamicRange(frameIndex, category);
        if (range.primitiveCount == 0)
        {
            continue;
        }

        inst.tlas_instance_prim_offsets[instanceIndex] = range.primitiveOffset;
        inst.tlas_instance_model_indices[instanceIndex] = -1;
        instanceIndex++;
    }

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

    // Stage G1b: pool/allocation sized x MAX_FRAMES_IN_FLIGHT, one
    // descriptor set per ring slot (same pattern as VertexBufferQ2 /
    // FramebuffersQ2).
    VkDescriptorPoolSize poolSizes[2] = {};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    poolSizes[0].descriptorCount = TLAS_COUNT * MAX_FRAMES_IN_FLIGHT;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
    poolSizes[1].descriptorCount = 4 * MAX_FRAMES_IN_FLIGHT;

    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    poolInfo.maxSets = MAX_FRAMES_IN_FLIGHT;

    r = vkCreateDescriptorPool(device, &poolInfo, nullptr, &descPool);
    VK_CHECKERROR(r);

    VkDescriptorSetAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = descPool;
    VkDescriptorSetLayout layouts[MAX_FRAMES_IN_FLIGHT];
    for (VkDescriptorSetLayout &layout : layouts)
    {
        layout = descSetLayout;
    }
    allocInfo.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
    allocInfo.pSetLayouts = layouts;

    r = vkAllocateDescriptorSets(device, &allocInfo, descSets);
    VK_CHECKERROR(r);

    // Placeholder texel buffers: one 4-byte storage buffer per binding with
    // a matching buffer view. Filled with real particle/beam/sprite data in
    // a later stage. Shared across ring slots (frame invariant).
    VkBufferUsageFlags texelUsage = VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    for (uint32_t i = 0; i < 4; i++)
    {
        texelBuffers[i].Init(allocator, 4, texelUsage,
                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                             "Q2RTX texel buffer placeholder");

        VkBufferViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
        viewInfo.buffer = texelBuffers[i].GetBuffer();
        // The Q2RTX hit shaders sample these texel buffers as float
        // (texelFetch on float texelBuffer), so the view must be R32_SFLOAT.
        viewInfo.format = VK_FORMAT_R32_SFLOAT;
        viewInfo.offset = 0;
        viewInfo.range = 4;

        r = vkCreateBufferView(device, &viewInfo, nullptr, &texelViews[i]);
        VK_CHECKERROR(r);
    }

    // Every ring slot's TLAS binding starts out pointing at nothing valid
    // (VK_NULL_HANDLE acceleration structures); SubmitStatic/SubmitDynamic
    // fill it in for real before any Q2 dispatch reads it (guarded by
    // HasTLAS()).
}

bool ASManagerQ2::HasTLAS() const
{
    return submitted &&
          tlasGeometry[activeFrameIndex] &&
          tlasGeometry[activeFrameIndex]->GetAS() != VK_NULL_HANDLE;
}

VkDescriptorSet ASManagerQ2::GetDescSet() const
{
    return descSets[activeFrameIndex];
}

VkDescriptorSetLayout ASManagerQ2::GetDescSetLayout() const
{
    return descSetLayout;
}
