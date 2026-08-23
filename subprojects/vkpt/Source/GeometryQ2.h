// GeometryQ2: converts legacy vkQuake geometry into the Q2RTX VboPrimitive
// format and uploads it to the Q2RTX world buffer (stage G1 of the porting
// plan, see PORTING.md), plus the per-frame dynamic aggregate buffer added
// in stage G1b.
//
// Part of the Q2RTX binding layer (stage S2b / Block 2): the legacy renderer
// keeps rendering as before, while this module builds a parallel Q2RTX-
// compatible vertex buffer (primitive array + BLAS source positions) from the
// RgGeometryUploadInfo that already flows through VulkanDevice::UploadGeometry.
//
// Stage G1b adds a second, per-frame path: every RG_GEOMETRY_TYPE_DYNAMIC
// upload (alias models - pickups/enemies/weapons/view weapon - and moving
// brush surfaces) is converted with the exact same per-triangle conversion
// used for the static world (AppendGeometry). World, view-weapon, and viewer
// geometry occupy contiguous ranges in one world-space aggregate buffer so
// ASManagerQ2 can give each range its own BLAS/TLAS visibility mask. The
// aggregate buffer is ring-buffered MAX_FRAMES_IN_FLIGHT deep so rebuilding
// one frame cannot disturb another frame still in flight. Sky and sprites are
// skipped: sky needs its deferred dedicated path, while
// RT_GetSpriteModelUniqueId (Quake/gl_rmisc.c) tags every sprite uniqueID with
// kind 3 in its top 4 bits. Particles and beams never call rgUploadGeometry.

#pragma once

#include "Buffer.h"
#include "Common.h"
#include "vkpt/vkpt.h"

#include <array>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vkpt
{

class CommandBufferManager;
class TextureManager;
class VertexBufferQ2;

class GeometryQ2
{
public:
    enum class DynamicGeometryCategory : uint32_t
    {
        World,
        ViewerWeapon,
        ViewerModel,
        Count,
    };

    static constexpr size_t DYNAMIC_GEOMETRY_CATEGORY_COUNT =
        static_cast<size_t>(DynamicGeometryCategory::Count);

    struct DynamicGeometryRange
    {
        uint32_t primitiveOffset = 0;
        uint32_t primitiveCount = 0;
        VkDeviceSize positionOffset = 0;
    };

    GeometryQ2(VkDevice device,
               std::shared_ptr<MemoryAllocator> allocator,
               std::shared_ptr<CommandBufferManager> cmdManager,
               std::shared_ptr<TextureManager> textureManager,
               std::shared_ptr<VertexBufferQ2> vertexBufferQ2);
    ~GeometryQ2();

    GeometryQ2(const GeometryQ2 &other) = delete;
    GeometryQ2(GeometryQ2 &&other) noexcept = delete;
    GeometryQ2 &operator=(const GeometryQ2 &other) = delete;
    GeometryQ2 &operator=(GeometryQ2 &&other) noexcept = delete;

    // Called once per level load (from Scene::StartNewStatic / VulkanDevice).
    void BeginStaticUpload();

    // Called for every static geometry upload. Converts the indexed triangle
    // list into Q2RTX VboPrimitive records + BLAS source positions, all in
    // world space. Dynamic geometry is handled by AddDynamicGeometry instead.
    void AddStaticGeometry(const RgGeometryUploadInfo &uploadInfo);

    // Called after the legacy static scene is submitted. Copies the gathered
    // CPU data into the Q2RTX world buffer and updates the descriptors.
    void SubmitStatic();

    uint32_t GetWorldPrimitiveCount() const;
    uint32_t GetTransparentPrimitiveCount() const;
    VkBuffer GetWorldBuffer() const;
    VkDeviceAddress GetWorldBufferAddress() const;
    // Offset of the opaque BLAS source positions inside the world buffer.
    VkDeviceSize GetWorldPositionOffset() const;
    // Offset of the transparent (water/slime/glass) BLAS source positions.
    VkDeviceSize GetTransparentPositionOffset() const;

    // Stage G1b: dynamic aggregate geometry, rebuilt every frame.

    // Called once per frame (VulkanDevice::BeginFrame) before any
    // AddDynamicGeometry calls for that frame arrive from the game.
    void BeginDynamicUpload();

    // Called for every RG_GEOMETRY_TYPE_DYNAMIC upload. Reuses the same
    // per-triangle conversion as the static path and additionally packs a
    // previous-current position delta into custom0/1/2 (motion vectors),
    // looked up by uploadInfo.uniqueID from the previous frame's history.
    void AddDynamicGeometry(const RgGeometryUploadInfo &uploadInfo);

    // Called once per frame (VulkanDevice::DrawFrame) after all of this
    // frame's AddDynamicGeometry calls. Uploads three contiguous visibility
    // ranges into the aggregate buffer for ring slot frameIndex (host-visible,
    // no staging/fence - safe because BeginFrame already fence-waited this
    // same slot) and prunes history entries for uniqueIDs that stopped
    // uploading this frame.
    void SubmitDynamic(uint32_t frameIndex);

    VkBuffer GetDynamicBuffer(uint32_t frameIndex) const;
    VkDeviceAddress GetDynamicBufferAddress(uint32_t frameIndex) const;
    DynamicGeometryRange GetDynamicRange(uint32_t frameIndex,
                                         DynamicGeometryCategory category) const;

private:
    struct WorldData
    {
        std::vector<uint8_t> primitives;
        std::vector<uint8_t> positions;
    };

    void UploadToDevice(WorldData &&opaque, WorldData &&transparent);

    // Shared per-triangle conversion used by both the static and dynamic
    // paths: transforms uploadInfo's triangles to world space, resolves /
    // dedupes the Q2 material table entry, encodes normals/tangents, and
    // appends VboPrimitive + BLAS source position records to "out". When
    // prevPositions is non-null and has exactly triCount*9 floats (same
    // topology as this call), the previous-current world-space delta is
    // packed into custom0/1/2 (packHalf4x16 semantics); otherwise the delta
    // is left zero. When positionHistoryOut is non-null, this call's
    // current world-space positions (triCount*9 floats) are written there
    // for use as next frame's prevPositions. Returns the triangle count.
    uint32_t AppendGeometry(const RgGeometryUploadInfo &uploadInfo,
                            WorldData &out,
                            uint32_t extraMaterialFlags,
                            const std::vector<float> *prevPositions,
                            std::vector<float> *positionHistoryOut);

    VkDevice device;
    std::shared_ptr<MemoryAllocator> allocator;
    std::shared_ptr<CommandBufferManager> cmdManager;
    std::shared_ptr<TextureManager> textureManager;
    std::shared_ptr<VertexBufferQ2> vertexBufferQ2;

    WorldData world;
    WorldData worldTransparent;
    Buffer worldBuffer;
    VkFence uploadFence;
    uint32_t worldPrimCount;
    uint32_t transparentPrimCount;
    bool hasWorldData;

    // Q2 material table entries collected from RgGeometryUploadInfo::
    // pQ2Material during static AND dynamic uploads: MATERIAL_UINTS uints
    // per entry, packed like Q2RTX material_table (vertex_buffer.h).
    // Material index 0 is empty, 1 is the default white material. Persists
    // across frames (cleared only at level load) so dynamic materials seen
    // once stay deduplicated; re-uploaded to the LightBuffer whenever it
    // grows (SubmitStatic at level load, SubmitDynamic every frame after).
    std::vector<uint32_t> materialTable;
    uint32_t materialCount;
    uint32_t uploadedMaterialCount;

    // Stage G1b: per-frame dynamic geometry, collected into visibility
    // categories between BeginDynamicUpload and SubmitDynamic, then copied
    // into contiguous ranges in the ring slot for the current frameIndex.
    std::array<WorldData, DYNAMIC_GEOMETRY_CATEGORY_COUNT> dynamicFrame;
    uint32_t dynamicPrimCount;
    // Temporary diagnostics (invisibility root-cause): how many dynamic upload
    // calls reached AddDynamicGeometry this frame (before any kind filtering).
    uint32_t dynamicUploadCallCount;

    // Previous frame's world-space triangle positions (9 floats/triangle,
    // same order AppendGeometry emits them) keyed by RgGeometryUploadInfo::
    // uniqueID, used to reconstruct motion vectors. RT_GetAliasModelUniqueId
    // / RT_GetBrushSurfUniqueId (Quake/gl_rmisc.c) keep this stable across
    // frames for the same entity/surface.
    std::unordered_map<uint64_t, std::vector<float>> dynamicHistory;
    // uniqueIDs seen during the current frame; anything missing after
    // SubmitDynamic is dropped from dynamicHistory (entity freed/despawned).
    std::unordered_set<uint64_t> dynamicSeenIds;

    // Ring-buffered GPU copies of the dynamic aggregate (primitives then
    // BLAS source positions, like the static world buffer), one per frame
    // in flight. Safe to rewrite in place: BeginFrame already fence-waits
    // this same slot's prior use before any AddDynamicGeometry call.
    std::array<Buffer, MAX_FRAMES_IN_FLIGHT> dynamicBuffers;
    std::array<
        std::array<DynamicGeometryRange, DYNAMIC_GEOMETRY_CATEGORY_COUNT>,
        MAX_FRAMES_IN_FLIGHT>
        dynamicRanges;
};

}
