#include "LightManagerQ2.h"

#include "VertexBufferQ2.h"

// Q2RTX binding contract: LightPolygon / LightBuffer sizes and the light
// list limits. constants.h must come first.
#include "../q2rtx-shaders/constants.h"
#include "../q2rtx-shaders/vertex_buffer.h"
// DynLightData + MAX_LIGHT_SOURCES for the point-light path.
#include "../q2rtx-shaders/global_ubo.h"

#include <cstring>

using namespace vkpt;

// Floats per LightPolygon entry in the light_polys array.
static constexpr uint32_t LIGHT_POLY_FLOATS = LIGHT_POLY_VEC4S * 4;

LightManagerQ2::LightManagerQ2(std::shared_ptr<VertexBufferQ2> _vertexBufferQ2)
:
    vertexBufferQ2(std::move(_vertexBufferQ2)),
    lightPolyCount(0),
    dynLightCount(0),
    uploadedClusterCount(0),
    uploadedTotalCount(0)
{
    lightPolys.reserve(static_cast<size_t>(MAX_LIGHT_POLYS) * LIGHT_POLY_FLOATS);
}

LightManagerQ2::~LightManagerQ2() = default;

void LightManagerQ2::PrepareForFrame()
{
    lightPolys.clear();
    lightPolyCount = 0;
    dynLights.clear();
    dynLightCount = 0;
    idToIndex.clear();

    uploadedClusterCount = 0;
    uploadedTotalCount = 0;
    uploadedOffsets.clear();
    uploadedLightIds.clear();
}

void LightManagerQ2::AddPolygonalLight(const RgPolygonalLightUploadInfo &info)
{
    if (lightPolyCount >= MAX_LIGHT_POLYS)
    {
        return;
    }

    // Q2RTX get_light_polygon() unpacks four vec4:
    //   p0 = (positions[0], color.r)
    //   p1 = (positions[1], color.g)
    //   p2 = (positions[2], color.b)
    //   p3 = (light_style_scale, prev_style_scale, unused, unused)
    // Quake has no Q2 light styles on these surfaces, so both scales are 1.
    const float entry[LIGHT_POLY_FLOATS] =
    {
        info.positions[0].data[0], info.positions[0].data[1], info.positions[0].data[2], info.color.data[0],
        info.positions[1].data[0], info.positions[1].data[1], info.positions[1].data[2], info.color.data[1],
        info.positions[2].data[0], info.positions[2].data[1], info.positions[2].data[2], info.color.data[2],
        1.0f, 1.0f, 0.0f, 0.0f,
    };

    // A unique ID can be uploaded more than once per frame (the game emits
    // the same surface from several paths); keep the first entry so the
    // cluster lists resolve to a stable index.
    const auto found = idToIndex.find(info.uniqueID);
    if (found != idToIndex.end())
    {
        return;
    }

    idToIndex[info.uniqueID] = lightPolyCount;
    lightPolys.insert(lightPolys.end(), entry, entry + LIGHT_POLY_FLOATS);
    lightPolyCount++;
}

void LightManagerQ2::AddSphericalLight(const RgSphericalLightUploadInfo &info)
{
    if (dynLightCount >= MAX_LIGHT_SOURCES)
    {
        return;
    }

    DynLightData light = {};
    light.center[0] = info.position.data[0];
    light.center[1] = info.position.data[1];
    light.center[2] = info.position.data[2];
    light.radius = info.radius;
    light.color[0] = info.color.data[0];
    light.color[1] = info.color.data[1];
    light.color[2] = info.color.data[2];

    // Always a sphere for now. RgSphericalLightUploadInfo.normal marks a
    // one-sided emitter, but DYNLIGHT_SPOT needs real cone angles packed into
    // spot_data and we have none to give - occlusion is handled by the shadow
    // ray either way, so a full sphere is the honest mapping.
    light.type = DYNLIGHT_SPHERE;

    const size_t offset = dynLights.size();
    dynLights.resize(offset + sizeof(DynLightData));
    std::memcpy(dynLights.data() + offset, &light, sizeof(DynLightData));
    dynLightCount++;
}

const void *LightManagerQ2::GetDynLightData() const
{
    return dynLights.data();
}

uint32_t LightManagerQ2::GetDynLightCount() const
{
    return dynLightCount;
}

void LightManagerQ2::SetClusterLightLists(uint32_t numClusters, const uint32_t *offsets,
                                          const uint64_t *lightUniqueIds, uint32_t totalCount)
{
    if (!offsets || !lightUniqueIds || numClusters == 0)
    {
        uploadedClusterCount = 0;
        uploadedTotalCount = 0;
        return;
    }

    uploadedClusterCount = numClusters;
    uploadedTotalCount = totalCount;

    uploadedOffsets.assign(offsets, offsets + numClusters + 1);
    uploadedLightIds.assign(lightUniqueIds, lightUniqueIds + totalCount);
}

void LightManagerQ2::Submit(uint32_t frameId)
{
    vertexBufferQ2->SetLightPolys(lightPolys.data(), lightPolyCount);

    if (uploadedClusterCount == 0 || lightPolyCount == 0)
    {
        // No lights this frame: every cluster must still see an empty range,
        // otherwise the shader reads stale offsets from the previous map.
        resolvedOffsets.assign(2, 0);
        resolvedCounts.assign(1, 0);
        vertexBufferQ2->SetClusterLightLists(1, resolvedOffsets.data(), nullptr, 0);
        vertexBufferQ2->SetLightCounts(frameId % LIGHT_COUNT_HISTORY,
                                       resolvedCounts.data(), 1);
        return;
    }

    // Resolve unique IDs to light-poly indices, dropping ids that never
    // arrived as a polygonal light (dlights and sphere lights share the
    // cluster registration but do not live in light_polys).
    resolvedOffsets.clear();
    resolvedIndices.clear();
    resolvedCounts.clear();
    resolvedOffsets.reserve(uploadedClusterCount + 1);
    resolvedIndices.reserve(uploadedTotalCount);
    resolvedCounts.reserve(uploadedClusterCount);

    uint32_t total = 0;
    for (uint32_t c = 0; c < uploadedClusterCount; c++)
    {
        resolvedOffsets.push_back(total);

        const uint32_t begin = uploadedOffsets[c];
        const uint32_t end = uploadedOffsets[c + 1];
        uint32_t written = 0;

        for (uint32_t i = begin; i < end && i < uploadedLightIds.size(); i++)
        {
            const auto found = idToIndex.find(uploadedLightIds[i]);
            if (found == idToIndex.end())
            {
                continue;
            }
            if (total + written >= MAX_LIGHT_LIST_NODES)
            {
                break;
            }

            resolvedIndices.push_back(found->second);
            written++;
        }

        resolvedCounts.push_back(written);
        total += written;
    }
    resolvedOffsets.push_back(total);

    vertexBufferQ2->SetClusterLightLists(uploadedClusterCount, resolvedOffsets.data(),
                                         resolvedIndices.data(), total);

    // sample_polygonal_lights takes the per-cluster light count from this
    // buffer, not from the offsets, and picks the slot by the frame number
    // recorded in the RNG seed (Q2RTX copy_bsp_lights does the same).
    vertexBufferQ2->SetLightCounts(frameId % LIGHT_COUNT_HISTORY,
                                   resolvedCounts.data(), uploadedClusterCount);
}

uint32_t LightManagerQ2::GetLightPolyCount() const
{
    return lightPolyCount;
}
