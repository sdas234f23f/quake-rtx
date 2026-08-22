// Q2RTX-convention light data (LightManagerQ2).
//
// Part of the Q2RTX geometry/lighting port (see PORTING.md, stage G6c).
// Collects the polygonal lights the game uploads each frame into the Q2RTX
// LightPolygon layout, and resolves the game's per-BSP-cluster light lists
// (which carry light unique IDs) into indices in that array. The result is
// written into the LightBuffer owned by VertexBufferQ2.
//
// Q2RTX builds this once per map from the BSP (bsp_mesh.c collect_light_polys
// / collect_cluster_lights); we rebuild it per frame from the uploads because
// the game already produces the lists that way, PVS included.

#pragma once

#include "vkpt/vkpt.h"
#include "Common.h"
#include "Containers.h"

#include <memory>
#include <vector>

namespace vkpt
{

class VertexBufferQ2;

class LightManagerQ2
{
public:
    explicit LightManagerQ2(std::shared_ptr<VertexBufferQ2> vertexBufferQ2);
    ~LightManagerQ2();

    LightManagerQ2(const LightManagerQ2 &other) = delete;
    LightManagerQ2(LightManagerQ2 &&other) noexcept = delete;
    LightManagerQ2 &operator=(const LightManagerQ2 &other) = delete;
    LightManagerQ2 &operator=(LightManagerQ2 &&other) noexcept = delete;

    // Drop the previous frame's lights. Called at the start of each frame.
    void PrepareForFrame();

    // One emissive triangle. Q2RTX samples these directly as area lights,
    // so the game's poly -> sphere merge is bypassed for this path.
    void AddPolygonalLight(const RgPolygonalLightUploadInfo &info);

    // One point light (dlights, entity lights, world_custom_lights.txt).
    // These go into the UBO's dyn_light_data array, which light_lists.h
    // samples separately from the per-cluster polygon lists, so they need no
    // cluster registration. Capped at MAX_LIGHT_SOURCES like Q2RTX.
    void AddSphericalLight(const RgSphericalLightUploadInfo &info);

    // Packed DynLightData entries for the UBO, and how many are valid.
    const void *GetDynLightData() const;
    uint32_t GetDynLightCount() const;

    // The game's per-cluster lists, keyed by light unique ID. Stashed here
    // and resolved in Submit(), because the lists can arrive before every
    // light has been uploaded.
    void SetClusterLightLists(uint32_t numClusters, const uint32_t *offsets,
                              const uint64_t *lightUniqueIds, uint32_t totalCount);

    // Resolve the unique IDs to light-poly indices and write everything into
    // the LightBuffer. frameId selects the light-counts history slot the way
    // the shader does.
    void Submit(uint32_t frameId);

    // Number of light polys written by the last Submit(); goes into
    // ubo.num_static_lights.
    uint32_t GetLightPolyCount() const;

private:
    std::shared_ptr<VertexBufferQ2> vertexBufferQ2;

    // LightPolygon entries, LIGHT_POLY_VEC4S * 4 floats each.
    std::vector<float> lightPolys;
    uint32_t lightPolyCount;

    // DynLightData entries as raw bytes (the struct comes from global_ubo.h,
    // which only the .cpp needs to see).
    std::vector<uint8_t> dynLights;
    uint32_t dynLightCount;

    // Unique ID -> index into lightPolys.
    rgl::unordered_map<uint64_t, uint32_t> idToIndex;

    // Cluster lists exactly as the game uploaded them.
    uint32_t uploadedClusterCount;
    uint32_t uploadedTotalCount;
    std::vector<uint32_t> uploadedOffsets;
    std::vector<uint64_t> uploadedLightIds;

    // Scratch for the resolved lists (kept across frames to avoid realloc).
    std::vector<uint32_t> resolvedOffsets;
    std::vector<uint32_t> resolvedIndices;
    std::vector<uint32_t> resolvedCounts;
};

}
