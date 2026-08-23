#include "GeometryQ2.h"

#include "CommandBufferManager.h"
#include "Generated/ShaderCommonC.h"
#include "TextureManager.h"
#include "Utils.h"
#include "VertexBufferQ2.h"

// Q2RTX binding contract: the vendored dual C/GLSL header defines
// VboPrimitive and the primitive/position buffer layout. constants.h must
// come first.
#include "../q2rtx-shaders/constants.h"
#include "../q2rtx-shaders/vertex_buffer.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

using namespace vkpt;

// Temporary diagnostics: append a line to a file next to the executable so the
// dynamic-geometry visibility bug can be traced without a console bridge.
static void DbgDynamic(const char *fmt, ...)
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

// Map the game's RT_MAT_KIND_* ordinal (Quake/rt_material.h) to the Q2RTX
// MATERIAL_KIND_* kind nibble (constants.h). These do NOT align numerically:
// rt_material.h has no EXPLOSION/TRANSPARENT entries, so SCREEN and CAMERA are
// shifted by two relative to the bit constants. Ordinal values are documented
// here so the mapping stays in sync with rt_material.h.
static uint32_t MapMaterialKind(int qmKind)
{
    switch (qmKind)
    {
    case 0:  return MATERIAL_KIND_INVALID;    // RT_MAT_KIND_INVALID
    case 1:  return MATERIAL_KIND_REGULAR;    // RT_MAT_KIND_REGULAR
    case 2:  return MATERIAL_KIND_CHROME;     // RT_MAT_KIND_CHROME
    case 3:  return MATERIAL_KIND_WATER;      // RT_MAT_KIND_WATER
    case 4:  return MATERIAL_KIND_LAVA;       // RT_MAT_KIND_LAVA
    case 5:  return MATERIAL_KIND_SLIME;      // RT_MAT_KIND_SLIME
    case 6:  return MATERIAL_KIND_GLASS;      // RT_MAT_KIND_GLASS
    case 7:  return MATERIAL_KIND_SKY;        // RT_MAT_KIND_SKY
    case 8:  return MATERIAL_KIND_INVISIBLE;  // RT_MAT_KIND_INVISIBLE
    case 9:  return MATERIAL_KIND_SCREEN;     // RT_MAT_KIND_SCREEN (shifted)
    case 10: return MATERIAL_KIND_CAMERA;     // RT_MAT_KIND_CAMERA (shifted)
    default: return MATERIAL_KIND_REGULAR;
    }
}

// Octahedral normal encoding, mirroring encode_normal() from utils.glsl so
// the CPU-produced normals decode to the same values the shaders expect.
static uint32_t EncodeNormal(float nx, float ny, float nz)
{
    const float invL1 = 1.0f / (std::abs(nx) + std::abs(ny) + std::abs(nz));
    float px = nx * invL1;
    float py = ny * invL1;

    if (nz < 0.0f)
    {
        const float sx = (px >= 0.0f) ? 1.0f : -1.0f;
        const float sy = (py >= 0.0f) ? 1.0f : -1.0f;
        const float ox = (1.0f - std::abs(py)) * sx;
        const float oy = (1.0f - std::abs(px)) * sy;
        px = ox;
        py = oy;
    }

    px = std::clamp(px * 0.5f + 0.5f, 0.0f, 1.0f);
    py = std::clamp(py * 0.5f + 0.5f, 0.0f, 1.0f);

    const uint32_t ux = static_cast<uint32_t>(px * 0xffffu);
    const uint32_t uy = static_cast<uint32_t>(py * 0xffffu);
    return ux | (uy << 16);
}

// Float to IEEE-754 half, used to pack (emissive, alpha) into one uint.
static uint16_t FloatToHalf(float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));

    const uint32_t sign = (bits >> 16) & 0x8000u;
    uint32_t exp = (bits >> 23) & 0xffu;
    uint32_t mant = bits & 0x7fffffu;

    if (exp == 0xffu)
    {
        // Inf / NaN
        return static_cast<uint16_t>(sign | 0x7c00u | (mant ? 0x200u : 0));
    }

    // Subnormal handling: values too small become half subnormals / zero.
    int32_t e = static_cast<int32_t>(exp) - 127 + 15;
    if (e >= 0x1f)
    {
        return static_cast<uint16_t>(sign | 0x7c00u); // overflow -> Inf
    }
    if (e <= 0)
    {
        if (e < -10)
        {
            return static_cast<uint16_t>(sign);
        }
        mant |= 0x800000u;
        const uint32_t shift = static_cast<uint32_t>(14 - e);
        uint32_t halfMant = mant >> shift;
        // round to nearest
        if ((mant >> (shift - 1)) & 1u)
        {
            halfMant++;
        }
        return static_cast<uint16_t>(sign | halfMant);
    }

    uint32_t halfMant = mant >> 13;
    // round to nearest
    if ((mant >> 12) & 1u)
    {
        halfMant++;
        if (halfMant == 0x400u)
        {
            halfMant = 0;
            e++;
        }
    }
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(e) << 10) | halfMant);
}

static uint32_t PackHalf2x16(float a, float b)
{
    return static_cast<uint32_t>(FloatToHalf(a)) |
           (static_cast<uint32_t>(FloatToHalf(b)) << 16);
}

GeometryQ2::GeometryQ2(VkDevice _device,
                       std::shared_ptr<MemoryAllocator> _allocator,
                       std::shared_ptr<CommandBufferManager> _cmdManager,
                       std::shared_ptr<TextureManager> _textureManager,
                       std::shared_ptr<VertexBufferQ2> _vertexBufferQ2)
: device(_device),
  allocator(std::move(_allocator)),
  cmdManager(std::move(_cmdManager)),
  textureManager(std::move(_textureManager)),
  vertexBufferQ2(std::move(_vertexBufferQ2)),
  uploadFence(VK_NULL_HANDLE),
  worldPrimCount(0),
  hasWorldData(false),
  materialCount(0),
  uploadedMaterialCount(0),
  dynamicPrimCount(0),
  dynamicUploadCallCount(0)
{
    VkFenceCreateInfo fenceInfo = {};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

    VkResult r = vkCreateFence(device, &fenceInfo, nullptr, &uploadFence);
    VK_CHECKERROR(r);

}

GeometryQ2::~GeometryQ2()
{
    worldBuffer.Destroy();
    for (auto &buf : dynamicBuffers)
    {
        buf.Destroy();
    }
    if (uploadFence != VK_NULL_HANDLE)
    {
        vkDestroyFence(device, uploadFence, nullptr);
    }
}

void GeometryQ2::BeginStaticUpload()
{
    world.primitives.clear();
    world.positions.clear();
    worldPrimCount = 0;
    hasWorldData = false;

    // Materials are collected per level load, entries uploaded in
    // SubmitStatic. Stale entries from the previous map are dropped.
    materialTable.clear();
    materialCount = 0;
    uploadedMaterialCount = 0;

    // Dynamic history is keyed by entity/surface uniqueID, which is only
    // meaningful within one map (RT_GetAliasModelUniqueId etc. reuse the
    // engine's per-entity index). Drop it so a new map doesn't inherit
    // stale motion vectors from the previous one.
    dynamicHistory.clear();
    dynamicSeenIds.clear();
}

void GeometryQ2::AddStaticGeometry(const RgGeometryUploadInfo &uploadInfo)
{
    if (uploadInfo.geomType != RG_GEOMETRY_TYPE_STATIC &&
        uploadInfo.geomType != RG_GEOMETRY_TYPE_STATIC_MOVABLE)
    {
        // Dynamic / instanced geometry is handled by AddDynamicGeometry.
        return;
    }

    const uint32_t triCount = AppendGeometry(uploadInfo, world, 0, nullptr, nullptr);
    worldPrimCount += triCount;
    hasWorldData = true;
}

void GeometryQ2::BeginDynamicUpload()
{
    for (WorldData &data : dynamicFrame)
    {
        data.primitives.clear();
        data.positions.clear();
    }
    dynamicPrimCount = 0;
    dynamicUploadCallCount = 0;
    dynamicSeenIds.clear();
}

void GeometryQ2::AddDynamicGeometry(const RgGeometryUploadInfo &uploadInfo)
{
    if (uploadInfo.geomType != RG_GEOMETRY_TYPE_DYNAMIC)
    {
        return;
    }

    dynamicUploadCallCount++;

    // Sky needs its own material and AS_FLAG_SKY visibility path. Keep it out
    // of the solid dynamic stage until that path is ported; treating it as
    // opaque world geometry would make it block primary and shadow rays.
    if (uploadInfo.visibilityType == RG_GEOMETRY_VISIBILITY_TYPE_SKY)
    {
        return;
    }

    // Reliable signal instead of game-side gating: RT_GetSpriteModelUniqueId
    // (Quake/gl_rmisc.c) tags every sprite uniqueID with kind 3 in the top 4
    // bits (RT_GetAliasModelUniqueId = 2, RT_GetBrushSurfUniqueId = 1). This
    // stage explicitly defers sprites/particles/beams, so drop sprite
    // uploads here; particles and beams never call rgUploadGeometry.
    constexpr uint64_t kUniqueIdKindMask = 0xFull << 60;
    constexpr uint64_t kUniqueIdKindSprite = 3ull << 60;
    if ((uploadInfo.uniqueID & kUniqueIdKindMask) == kUniqueIdKindSprite)
    {
        return;
    }

    DynamicGeometryCategory category = DynamicGeometryCategory::World;
    uint32_t extraMaterialFlags = 0;
    if (uploadInfo.visibilityType == RG_GEOMETRY_VISIBILITY_TYPE_FIRST_PERSON)
    {
        category = DynamicGeometryCategory::ViewerWeapon;
        extraMaterialFlags |= MATERIAL_FLAG_WEAPON;
    }
    else if (uploadInfo.visibilityType ==
             RG_GEOMETRY_VISIBILITY_TYPE_FIRST_PERSON_VIEWER)
    {
        category = DynamicGeometryCategory::ViewerModel;
    }

    const auto historyIt = dynamicHistory.find(uploadInfo.uniqueID);
    const std::vector<float> *prevPositions =
        (historyIt != dynamicHistory.end()) ? &historyIt->second : nullptr;

    std::vector<float> newHistory;
    WorldData &categoryData = dynamicFrame[static_cast<size_t>(category)];
    const uint32_t triCount =
        AppendGeometry(uploadInfo, categoryData, extraMaterialFlags,
                       prevPositions, &newHistory);

    dynamicPrimCount += triCount;
    dynamicSeenIds.insert(uploadInfo.uniqueID);
    dynamicHistory[uploadInfo.uniqueID] = std::move(newHistory);
}

void GeometryQ2::SubmitDynamic(uint32_t frameIndex)
{
    if (frameIndex >= MAX_FRAMES_IN_FLIGHT)
    {
        return;
    }

    // Drop history for uniqueIDs that stopped uploading this frame (freed
    // pickups, dead monsters, closed doors that finished moving, etc.) so a
    // future entity reusing the same uniqueID slot never inherits a stale
    // delta or a topology mismatch.
    for (auto it = dynamicHistory.begin(); it != dynamicHistory.end();)
    {
        if (dynamicSeenIds.find(it->first) == dynamicSeenIds.end())
        {
            it = dynamicHistory.erase(it);
        }
        else
        {
            ++it;
        }
    }

    // Existing entries may still be read by another in-flight frame. Material
    // entries are immutable after insertion, so append only the new tail:
    // older frames cannot reference those new indices.
    if (materialCount > uploadedMaterialCount)
    {
        const uint32_t newCount = materialCount - uploadedMaterialCount;
        vertexBufferQ2->SetQ2Materials(
            materialTable.data() + uploadedMaterialCount * MATERIAL_UINTS,
            uploadedMaterialCount, newCount);
        uploadedMaterialCount = materialCount;
    }

    dynamicRanges[frameIndex] = {};

    DbgDynamic("GeometryQ2::SubmitDynamic frame=%u calls=%u totalTris=%u "
               "world=%u viewerWeapon=%u viewerModel=%u\n",
               frameIndex, dynamicUploadCallCount, dynamicPrimCount,
               static_cast<uint32_t>(
                   dynamicFrame[static_cast<size_t>(DynamicGeometryCategory::World)].primitives.size() /
                   sizeof(VboPrimitive)),
               static_cast<uint32_t>(
                   dynamicFrame[static_cast<size_t>(DynamicGeometryCategory::ViewerWeapon)].primitives.size() /
                   sizeof(VboPrimitive)),
               static_cast<uint32_t>(
                   dynamicFrame[static_cast<size_t>(DynamicGeometryCategory::ViewerModel)].primitives.size() /
                   sizeof(VboPrimitive)));

    if (dynamicPrimCount == 0)
    {
        // Nothing dynamic this frame; leave the ring slot's previous buffer
        // untouched; ASManagerQ2 skips the dynamic instance when the count
        // is zero, so its stale contents are never read.
        return;
    }

    VkDeviceSize primSize = 0;
    VkDeviceSize posSize = 0;
    for (const WorldData &data : dynamicFrame)
    {
        primSize += data.primitives.size();
        posSize += data.positions.size();
    }
    const VkDeviceSize totalSize = primSize + posSize;

    Buffer &buf = dynamicBuffers[frameIndex];
    if (!buf.IsInitted() || buf.GetSize() < totalSize)
    {
        buf.Destroy();
        buf.Init(allocator, totalSize,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                    VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                "Q2RTX dynamic geometry buffer");
    }

    // Host-visible coherent memory: the write below happens-before this
    // frame's command buffer is submitted (SubmitDynamic runs synchronously
    // on the CPU before VulkanDevice::DrawFrame records the ray tracing
    // dispatches), so no explicit barrier is required for the device to see
    // it, matching how ASManagerQ2 already treats its TLAS instance buffer.
    uint8_t *mapped = static_cast<uint8_t *>(buf.Map());
    VkDeviceSize primitiveByteOffset = 0;
    VkDeviceSize positionByteOffset = primSize;

    for (size_t i = 0; i < dynamicFrame.size(); i++)
    {
        const WorldData &data = dynamicFrame[i];
        DynamicGeometryRange &range = dynamicRanges[frameIndex][i];

        range.primitiveOffset =
            static_cast<uint32_t>(primitiveByteOffset / sizeof(VboPrimitive));
        range.primitiveCount =
            static_cast<uint32_t>(data.primitives.size() / sizeof(VboPrimitive));
        range.positionOffset = positionByteOffset;

        if (!data.primitives.empty())
        {
            std::memcpy(mapped + primitiveByteOffset, data.primitives.data(),
                        data.primitives.size());
            primitiveByteOffset += data.primitives.size();
        }
        if (!data.positions.empty())
        {
            std::memcpy(mapped + positionByteOffset, data.positions.data(),
                        data.positions.size());
            positionByteOffset += data.positions.size();
        }
    }
    buf.Unmap();

    const VkDescriptorBufferInfo primInfo =
    {
        .buffer = buf.GetBuffer(),
        .offset = 0,
        .range = primSize,
    };
    vertexBufferQ2->SetDynamicBufferInfo(frameIndex, primInfo);
}

VkBuffer GeometryQ2::GetDynamicBuffer(uint32_t frameIndex) const
{
    if (frameIndex >= MAX_FRAMES_IN_FLIGHT || !dynamicBuffers[frameIndex].IsInitted())
    {
        return VK_NULL_HANDLE;
    }
    return dynamicBuffers[frameIndex].GetBuffer();
}

VkDeviceAddress GeometryQ2::GetDynamicBufferAddress(uint32_t frameIndex) const
{
    if (frameIndex >= MAX_FRAMES_IN_FLIGHT || !dynamicBuffers[frameIndex].IsInitted())
    {
        return 0;
    }
    return dynamicBuffers[frameIndex].GetAddress();
}

GeometryQ2::DynamicGeometryRange GeometryQ2::GetDynamicRange(
    uint32_t frameIndex, DynamicGeometryCategory category) const
{
    const size_t categoryIndex = static_cast<size_t>(category);
    if (frameIndex >= MAX_FRAMES_IN_FLIGHT ||
        categoryIndex >= DYNAMIC_GEOMETRY_CATEGORY_COUNT)
    {
        return {};
    }
    return dynamicRanges[frameIndex][categoryIndex];
}

uint32_t GeometryQ2::AppendGeometry(const RgGeometryUploadInfo &uploadInfo,
                                    WorldData &out,
                                    uint32_t extraMaterialFlags,
                                    const std::vector<float> *prevPositions,
                                    std::vector<float> *positionHistoryOut)
{
    const uint32_t triCount = uploadInfo.indexCount ? uploadInfo.indexCount / 3
                                                    : uploadInfo.vertexCount / 3;

    out.primitives.reserve(out.primitives.size() + triCount * sizeof(VboPrimitive));
    out.positions.reserve(out.positions.size() + triCount * 9 * sizeof(float));

    // Only use the caller-supplied previous-frame history if it matches this
    // call's topology exactly (same triangle count); otherwise the geometry
    // is new or changed shape, so every custom0/1/2 delta stays zero.
    const bool havePrevPositions = prevPositions != nullptr &&
                                   prevPositions->size() == static_cast<size_t>(triCount) * 9;

    if (positionHistoryOut)
    {
        positionHistoryOut->clear();
        positionHistoryOut->reserve(static_cast<size_t>(triCount) * 9);
    }

    const float (&m)[3][4] = uploadInfo.transform.matrix;

    auto TransformPoint = [&m](const float p[3], float out[3])
    {
        for (int k = 0; k < 3; k++)
        {
            out[k] = m[k][0] * p[0] + m[k][1] * p[1] + m[k][2] * p[2] + m[k][3];
        }
    };

    // Normal transform: the upper 3x3 part of the matrix, renormalized.
    auto TransformNormal = [&m](const float n[3], float out[3])
    {
        for (int k = 0; k < 3; k++)
        {
            out[k] = m[k][0] * n[0] + m[k][1] * n[1] + m[k][2] * n[2];
        }
        const float len = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
        if (len > 1.0e-6f)
        {
            out[0] /= len;
            out[1] /= len;
            out[2] /= len;
        }
    };

    // Stage G6: build the per-surface Q2 material table entry. The game
    // resolves the Q2RTX-style .mat data through uploadInfo.pQ2Material
    // (filled by r_world.c); when no material is defined, fall back to the
    // cvar defaults from the upload info so the Q2 table always mirrors
    // rt_brush_rough / rt_brush_metal. The entry format is the 6-uint
    // layout of get_material_info in vertex_buffer.h; entries are
    // de-duplicated. Index 0 is empty, index 1 is the startup default.
    // The legacy material owns albedo, packed roughness/metal/emission (RME),
    // and normal textures. The Q2 shader adapter interprets the emissive slot
    // as packed RME while both renderers share this material system.
    const RgQ2Material *qm = uploadInfo.pQ2Material;
    const MaterialTextures textures =
        textureManager->GetMaterialTextures(uploadInfo.geomMaterial.layerMaterials[0]);
    const uint32_t baseTexture = textures.indices[MATERIAL_ALBEDO_ALPHA_INDEX];
    const uint32_t rmeTexture =
        textures.indices[MATERIAL_ROUGHNESS_METALLIC_EMISSION_INDEX];
    const uint32_t normalTexture = textures.indices[MATERIAL_NORMAL_INDEX];
    const bool hasBaseTexture = baseTexture != EMPTY_TEXTURE_INDEX;
    const bool hasRmeTexture = rmeTexture != EMPTY_TEXTURE_INDEX;

    uint32_t entry[MATERIAL_UINTS];
    entry[0] = baseTexture | (normalTexture << 16);
    entry[1] = rmeTexture | (baseTexture << 16);
    entry[4] = 1; // num_frames | next_frame << 16

    // TexMgr_ApplyMaterialFromMat already bakes the .mat base, bump,
    // roughness, metalness, and emissive factors into these textures.
    const float roughness = hasRmeTexture
                                ? -1.0f
                                : (qm && qm->roughness_override > 0.0f)
                                      ? qm->roughness_override
                                      : uploadInfo.defaultRoughness;
    const float metalness = hasRmeTexture
                                ? 1.0f
                                : qm ? qm->metalness_factor : uploadInfo.defaultMetallicity;
    const float emissive = rmeTexture != EMPTY_TEXTURE_INDEX
                               ? 1.0f
                               : (qm && qm->is_light) ? qm->emissive_factor : 0.0f;
    const float specular = (qm && qm->specular_factor > 0.0f) ? qm->specular_factor : 0.5f;
    const float baseFactor =
        hasBaseTexture ? 1.0f
                       : (qm && qm->base_factor > 0.0f) ? qm->base_factor : 1.0f;
    const float bump = normalTexture != EMPTY_TEXTURE_INDEX ? 1.0f
                                                            : qm ? qm->bump_scale : 0.0f;

    entry[2] = PackHalf2x16(std::clamp(bump, 0.0f, 1.0f),
                            std::clamp(roughness, -1.0f, 1.0f));
    entry[3] = PackHalf2x16(std::clamp(metalness, 0.0f, 1.0f),
                            std::clamp(emissive, 0.0f, 1.0f));
    entry[5] = PackHalf2x16(std::clamp(specular, 0.0f, 1.0f),
                            std::clamp(baseFactor, 0.0f, 1.0f));

    uint32_t materialId = 1;
    uint32_t index = UINT32_MAX;
    for (uint32_t i = 0; i < materialCount; i++)
    {
        if (std::memcmp(materialTable.data() + i * MATERIAL_UINTS, entry,
                        sizeof(entry)) == 0)
        {
            index = i;
            break;
        }
    }
    if (index == UINT32_MAX)
    {
        if (materialCount + 2 >= MAX_PBR_MATERIALS)
        {
            // Overflow: fall back to the default material.
            materialId = 1;
        }
        else
        {
            index = materialCount;
            materialCount++;
            materialTable.insert(materialTable.end(), entry, entry + MATERIAL_UINTS);
            materialId = 2 + index;
        }
    }
    else
    {
        materialId = 2 + index;
    }

    if (materialId != 1)
    {
        // Map the game's RT_MAT_KIND_* ordinal to the Q2RTX MATERIAL_KIND_*
        // nibble. Quake 1 water/slime/lava surfaces reach this through the
        // surface-flag override in r_world.c (is_water -> WATER, is_acid ->
        // SLIME); explicit .mat kinds (glass, sky, ...) map directly. Unknown
        // or unset kinds fall back to REGULAR.
        materialId |= MapMaterialKind(qm ? qm->kind : /*RT_MAT_KIND_REGULAR*/ 1);
        if (qm && qm->is_light)
        {
            materialId |= MATERIAL_FLAG_LIGHT;
        }
    }
    // Stage G1b: MATERIAL_FLAG_WEAPON for first-person view weapon
    // triangles, independent of which material index they resolved to.
    materialId |= extraMaterialFlags;

    for (uint32_t t = 0; t < triCount; t++)
    {
        const uint32_t idx[3] =
        {
            uploadInfo.pIndices ? uploadInfo.pIndices[t * 3 + 0] : t * 3 + 0,
            uploadInfo.pIndices ? uploadInfo.pIndices[t * 3 + 1] : t * 3 + 1,
            uploadInfo.pIndices ? uploadInfo.pIndices[t * 3 + 2] : t * 3 + 2,
        };

        VboPrimitive prim = {};

        // World-space triangle positions.
        float worldPos[3][3];
        for (int v = 0; v < 3; v++)
        {
            const RgVertex &vert = uploadInfo.pVertices[idx[v]];
            TransformPoint(vert.position, worldPos[v]);
        }

        // G5 fix: world geometry uploads with RG_GEOMETRY_UPLOAD_GENERATE_
        // NORMALS_BIT leave RgVertex.normal zero (RTGL1 generates normals in
        // a compute shader instead, which we do not run). Encoding a zero
        // normal here would produce NaN/garbage, so derive the flat normal
        // from the triangle winding instead — exactly like primary_rays.rgen
        // computes flat_normal (cross(p1-p0, p2-p1)). This keeps the stored
        // normal consistent with the backface-flip logic in primary_rays.
        uint32_t normalEnc;
        if (uploadInfo.flags & RG_GEOMETRY_UPLOAD_GENERATE_NORMALS_BIT)
        {
            const float e1[3] = {worldPos[1][0] - worldPos[0][0],
                                 worldPos[1][1] - worldPos[0][1],
                                 worldPos[1][2] - worldPos[0][2]};
            const float e2[3] = {worldPos[2][0] - worldPos[1][0],
                                 worldPos[2][1] - worldPos[1][1],
                                 worldPos[2][2] - worldPos[1][2]};
            float flat[3] = {e1[1] * e2[2] - e1[2] * e2[1],
                             e1[2] * e2[0] - e1[0] * e2[2],
                             e1[0] * e2[1] - e1[1] * e2[0]};
            const float len = std::sqrt(flat[0] * flat[0] + flat[1] * flat[1] + flat[2] * flat[2]);
            if (len > 1.0e-6f)
            {
                flat[0] /= len;
                flat[1] /= len;
                flat[2] /= len;
            }
            normalEnc = EncodeNormal(flat[0], flat[1], flat[2]);
        }
        else
        {
            // Explicitly supplied normals (e.g. alias models).
            const RgVertex &v0 = uploadInfo.pVertices[idx[0]];
            float worldNormal[3];
            TransformNormal(v0.normal, worldNormal);
            normalEnc = EncodeNormal(worldNormal[0], worldNormal[1], worldNormal[2]);
        }

        uint32_t tangentEnc[3] = {};
        bool flipBitangent = false;
        {
            const float *uv0 = uploadInfo.pVertices[idx[0]].texCoord;
            const float *uv1 = uploadInfo.pVertices[idx[1]].texCoord;
            const float *uv2 = uploadInfo.pVertices[idx[2]].texCoord;
            const float du1 = uv1[0] - uv0[0];
            const float dv1 = uv1[1] - uv0[1];
            const float du2 = uv2[0] - uv0[0];
            const float dv2 = uv2[1] - uv0[1];
            const float determinant = du1 * dv2 - dv1 * du2;

            if (std::abs(determinant) > 1.0e-8f)
            {
                const float invDet = 1.0f / determinant;
                float tangent[3];
                float bitangent[3];
                for (int k = 0; k < 3; k++)
                {
                    const float edge1 = worldPos[1][k] - worldPos[0][k];
                    const float edge2 = worldPos[2][k] - worldPos[0][k];
                    tangent[k] = (edge1 * dv2 - edge2 * dv1) * invDet;
                    bitangent[k] = (edge2 * du1 - edge1 * du2) * invDet;
                }

                const float length = std::sqrt(tangent[0] * tangent[0] +
                                               tangent[1] * tangent[1] +
                                               tangent[2] * tangent[2]);
                if (length > 1.0e-6f)
                {
                    tangent[0] /= length;
                    tangent[1] /= length;
                    tangent[2] /= length;
                    const uint32_t encoded =
                        EncodeNormal(tangent[0], tangent[1], tangent[2]);
                    tangentEnc[0] = tangentEnc[1] = tangentEnc[2] = encoded;

                    const float edge1[3] =
                    {
                        worldPos[1][0] - worldPos[0][0],
                        worldPos[1][1] - worldPos[0][1],
                        worldPos[1][2] - worldPos[0][2],
                    };
                    const float edge2[3] =
                    {
                        worldPos[2][0] - worldPos[0][0],
                        worldPos[2][1] - worldPos[0][1],
                        worldPos[2][2] - worldPos[0][2],
                    };
                    const float faceNormal[3] =
                    {
                        edge1[1] * edge2[2] - edge1[2] * edge2[1],
                        edge1[2] * edge2[0] - edge1[0] * edge2[2],
                        edge1[0] * edge2[1] - edge1[1] * edge2[0],
                    };
                    const float crossNormalTangent[3] =
                    {
                        faceNormal[1] * tangent[2] - faceNormal[2] * tangent[1],
                        faceNormal[2] * tangent[0] - faceNormal[0] * tangent[2],
                        faceNormal[0] * tangent[1] - faceNormal[1] * tangent[0],
                    };
                    flipBitangent =
                        crossNormalTangent[0] * bitangent[0] +
                        crossNormalTangent[1] * bitangent[1] +
                        crossNormalTangent[2] * bitangent[2] < 0.0f;
                }
            }
        }

        for (int v = 0; v < 3; v++)
        {
            float *posOut = (v == 0) ? prim.pos0 : (v == 1) ? prim.pos1 : prim.pos2;
            posOut[0] = worldPos[v][0];
            posOut[1] = worldPos[v][1];
            posOut[2] = worldPos[v][2];

            uint32_t *normalOut = (v == 0) ? &prim.normals[0]
                                           : (v == 1) ? &prim.normals[1] : &prim.normals[2];
            *normalOut = normalEnc;
            prim.tangents[v] = tangentEnc[v];
        }

        // Stage G1b: previous-current motion vector, packed like
        // pack_motion_vector's packHalf4x16(prev - current, 0) in
        // vertex_buffer.h. Zero (the default-initialized value) for
        // static geometry (prevPositions is always null there) and for new
        // or topology-changed dynamic geometry.
        if (havePrevPositions)
        {
            uint32_t *const customOut[3] = {prim.custom0, prim.custom1, prim.custom2};
            for (int v = 0; v < 3; v++)
            {
                const size_t base = (static_cast<size_t>(t) * 3 + v) * 3;
                const float dx = (*prevPositions)[base + 0] - worldPos[v][0];
                const float dy = (*prevPositions)[base + 1] - worldPos[v][1];
                const float dz = (*prevPositions)[base + 2] - worldPos[v][2];
                customOut[v][0] = PackHalf2x16(dx, dy);
                customOut[v][1] = PackHalf2x16(dz, 0.0f);
            }
        }

        // uv / material / cluster / shell. The material id was resolved from
        // the per-upload Q2 material above (index + MATERIAL_KIND_REGULAR +
        // optional MATERIAL_FLAG_LIGHT). Alpha comes from the first layer.
        prim.material_id =
            materialId | (flipBitangent ? MATERIAL_FLAG_HANDEDNESS : 0);
        prim.uv0[0] = uploadInfo.pVertices[idx[0]].texCoord[0];
        prim.uv0[1] = uploadInfo.pVertices[idx[0]].texCoord[1];
        prim.uv1[0] = uploadInfo.pVertices[idx[1]].texCoord[0];
        prim.uv1[1] = uploadInfo.pVertices[idx[1]].texCoord[1];
        prim.uv2[0] = uploadInfo.pVertices[idx[2]].texCoord[0];
        prim.uv2[1] = uploadInfo.pVertices[idx[2]].texCoord[1];

        prim.cluster = static_cast<int32_t>(uploadInfo.pVertices[idx[0]].cluster);

        const float alpha = uploadInfo.layerColors[0].data[3];
        const float primitiveEmissive =
            rmeTexture != EMPTY_TEXTURE_INDEX ? 1.0f : uploadInfo.defaultEmission;
        prim.emissive_and_alpha = PackHalf2x16(primitiveEmissive, alpha);

        const size_t primOffset = out.primitives.size();
        out.primitives.resize(primOffset + sizeof(VboPrimitive));
        std::memcpy(out.primitives.data() + primOffset, &prim, sizeof(VboPrimitive));

        // Append the three world-space positions for the BLAS source buffer
        // (and, for the dynamic path, this frame's history for next frame's
        // motion vectors - same layout, 9 floats per triangle).
        for (int v = 0; v < 3; v++)
        {
            const size_t posOffset = out.positions.size();
            out.positions.resize(posOffset + 3 * sizeof(float));
            std::memcpy(out.positions.data() + posOffset, worldPos[v], sizeof(worldPos[v]));

            if (positionHistoryOut)
            {
                positionHistoryOut->push_back(worldPos[v][0]);
                positionHistoryOut->push_back(worldPos[v][1]);
                positionHistoryOut->push_back(worldPos[v][2]);
            }
        }
    }

    return triCount;
}

void GeometryQ2::SubmitStatic()
{
    // Upload the per-surface material table collected during the static
    // uploads, even if there is no geometry (the table can outlive it).
    if (materialCount > 0)
    {
        vertexBufferQ2->SetQ2Materials(materialTable.data(), 0, materialCount);
        uploadedMaterialCount = materialCount;
    }

    if (!hasWorldData || world.primitives.empty())
    {
        return;
    }

    UploadToDevice(std::move(world));
    hasWorldData = false;
}

uint32_t GeometryQ2::GetWorldPrimitiveCount() const
{
    return worldPrimCount;
}

VkBuffer GeometryQ2::GetWorldBuffer() const
{
    return worldBuffer.IsInitted() ? worldBuffer.GetBuffer() : VK_NULL_HANDLE;
}

VkDeviceAddress GeometryQ2::GetWorldBufferAddress() const
{
    return worldBuffer.IsInitted() ? worldBuffer.GetAddress() : 0;
}

VkDeviceSize GeometryQ2::GetWorldPositionOffset() const
{
    return worldPrimCount * sizeof(VboPrimitive);
}

void GeometryQ2::UploadToDevice(WorldData &&data)
{
    const VkDeviceSize primSize = data.primitives.size();
    const VkDeviceSize posSize = data.positions.size();

    if (primSize == 0)
    {
        return;
    }

    // One buffer holds the primitive array followed by the BLAS source
    // positions, exactly like Q2RTX's world buffer (buf_world).
    worldBuffer.Destroy();
    worldBuffer.Init(allocator, primSize + posSize,
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                         VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                         VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                     "Q2RTX world geometry buffer");

    // Host-visible staging copy.
    Buffer staging;
    staging.Init(allocator, primSize + posSize,
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 "Q2RTX world geometry staging");

    void *mapped = staging.Map();
    std::memcpy(mapped, data.primitives.data(), primSize);
    std::memcpy(static_cast<uint8_t *>(mapped) + primSize, data.positions.data(), posSize);
    staging.Unmap();

    VkCommandBuffer cmd = cmdManager->StartGraphicsCmd();

    VkBufferCopy copyInfo = {};
    copyInfo.size = primSize + posSize;
    vkCmdCopyBuffer(cmd, staging.GetBuffer(), worldBuffer.GetBuffer(), 1, &copyInfo);

    VkBufferMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                            VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    barrier.buffer = worldBuffer.GetBuffer();
    barrier.offset = 0;
    barrier.size = primSize + posSize;

    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                             VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         0, 0, nullptr, 1, &barrier, 0, nullptr);

    cmdManager->Submit(cmd, uploadFence);
    Utils::WaitAndResetFence(device, uploadFence);

    staging.Destroy();

    // Point the descriptor set at the real world buffer: binding 0 element
    // VERTEX_BUFFER_WORLD = the primitive array, binding 1 = the positions.
    const VkDescriptorBufferInfo primInfo =
    {
        .buffer = worldBuffer.GetBuffer(),
        .offset = 0,
        .range = primSize,
    };
    const VkDescriptorBufferInfo posInfo =
    {
        .buffer = worldBuffer.GetBuffer(),
        .offset = primSize,
        .range = posSize,
    };

    vertexBufferQ2->SetWorldBufferInfo(primInfo, posInfo);
}
