#include "LightManagerQ2.h"

#include "Utils.h"
#include "VertexBufferQ2.h"

// Q2RTX binding contract: LightPolygon / LightBuffer sizes and the light
// list limits. constants.h must come first.
#include "../q2rtx-shaders/constants.h"
#include "../q2rtx-shaders/vertex_buffer.h"
// DynLightData + MAX_LIGHT_SOURCES for the point-light path.
#include "../q2rtx-shaders/global_ubo.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>

using namespace vkpt;

// Floats per LightPolygon entry in the light_polys array.
static constexpr uint32_t LIGHT_POLY_FLOATS = LIGHT_POLY_VEC4S * 4;

namespace
{

// Q2RTX light colors are radiance, not flux. Two independent corrections are
// applied to the uploaded flux:
//  1. Flux -> radiance: divide by the emitter area exactly like
//     LightManager::EncodeAsSphereLight / EncodeAsTriangleLight.
//  2. Counteract the legacy vkquake-rt over-amplification. RT_FIXUP_LIGHT_INTENSITY
//     (glquake.h) multiplies every light by rt_globallight_mult (5) *
//     RT_QUAKE_LIGHT_AREA_INTENSITY_FIX (1600) * rt_brightness (1.0) = 8000
//     before upload (all call sites pass witharea=true). Q2RTX's physical HDR
//     pipeline (FP16 SH x1024, final composite x128) expects radiance ~1-10;
//     without this correction the ~8000x flux overflows FP16 and renders as the
//     green pixel shift the user sees on brightly lit surfaces.
//  The 1/3000 (rather than 1/8000) factor deliberately leaves the lights ~2.67x
//  brighter than the physically-neutral normalization so the scene is not too
//  dark; it still stays well inside the FP16 HDR range.
constexpr double RG_PI = 3.1415926535897932384626433;
constexpr float MIN_SPHERE_RADIUS = 0.005f;
constexpr float Q2_LIGHT_SCALE = 1.0f / 3000.0f;

double GetSphericalLightContribution(const RgSphericalLightUploadInfo &light,
                                     const float cameraPosition[3])
{
    const double intensity =
        0.299 * std::max(static_cast<double>(light.color.data[0]), 0.0) +
        0.587 * std::max(static_cast<double>(light.color.data[1]), 0.0) +
        0.114 * std::max(static_cast<double>(light.color.data[2]), 0.0);

    const double dx = static_cast<double>(light.position.data[0]) - cameraPosition[0];
    const double dy = static_cast<double>(light.position.data[1]) - cameraPosition[1];
    const double dz = static_cast<double>(light.position.data[2]) - cameraPosition[2];
    const double distanceSquared = dx * dx + dy * dy + dz * dz;

    if (!std::isfinite(intensity) || !std::isfinite(distanceSquared))
    {
        return 0.0;
    }

    return intensity / std::max(distanceSquared, 1.0);
}

void AppendSphericalLight(std::vector<uint8_t> &dst,
                          const RgSphericalLightUploadInfo &info)
{
    DynLightData light = {};
    light.center[0] = info.position.data[0];
    light.center[1] = info.position.data[1];
    light.center[2] = info.position.data[2];
    light.radius = info.radius;

    // Flux -> radiance: match LightManager::EncodeAsSphereLight, then
    // counteract the legacy 8000x over-amplification (see Q2_LIGHT_SCALE).
    const float radius = std::max(MIN_SPHERE_RADIUS, info.radius);
    const float invArea = 1.0f / (static_cast<float>(RG_PI) * radius * radius);
    light.color[0] = info.color.data[0] * Q2_LIGHT_SCALE * invArea;
    light.color[1] = info.color.data[1] * Q2_LIGHT_SCALE * invArea;
    light.color[2] = info.color.data[2] * Q2_LIGHT_SCALE * invArea;

    // RgSphericalLightUploadInfo.normal marks a one-sided emitter, but
    // DYNLIGHT_SPOT needs real cone angles packed into spot_data.
    light.type = DYNLIGHT_SPHERE;

    const size_t offset = dst.size();
    dst.resize(offset + sizeof(DynLightData));
    std::memcpy(dst.data() + offset, &light, sizeof(DynLightData));
}

}

LightManagerQ2::LightManagerQ2(std::shared_ptr<VertexBufferQ2> _vertexBufferQ2)
:
    vertexBufferQ2(std::move(_vertexBufferQ2)),
    lightPolyCount(0),
    dynLightCount(0),
    uploadedClusterCount(0),
    uploadedTotalCount(0)
{
    lightPolys.reserve(static_cast<size_t>(MAX_LIGHT_POLYS) * LIGHT_POLY_FLOATS);
    sphericalLights.reserve(MAX_LIGHT_SOURCES);
    sphericalLightContributions.reserve(MAX_LIGHT_SOURCES);
    sphericalLightOrder.reserve(MAX_LIGHT_SOURCES);
    dynLights.reserve(static_cast<size_t>(MAX_LIGHT_SOURCES) * sizeof(DynLightData));
}

LightManagerQ2::~LightManagerQ2() = default;

void LightManagerQ2::PrepareForFrame()
{
    lightPolys.clear();
    lightPolyCount = 0;
    sphericalLights.clear();
    sphericalLightContributions.clear();
    sphericalLightOrder.clear();
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
    //
    // Flux -> radiance: divide by the triangle area exactly like
    // LightManager::EncodeAsTriangleLight (area = 0.5 * |cross(p1-p0, p2-p0)|),
    // then counteract the legacy 8000x over-amplification (see Q2_LIGHT_SCALE).
    const RgFloat3D unnormalizedNormal = Utils::GetUnnormalizedNormal(info.positions);
    const float area = Utils::Length(unnormalizedNormal.data) * 0.5f;
    const float invArea = (area > 0.0f) ? (1.0f / area) : 0.0f;
    const float scale = Q2_LIGHT_SCALE * invArea;

    const float entry[LIGHT_POLY_FLOATS] =
    {
        info.positions[0].data[0], info.positions[0].data[1], info.positions[0].data[2], info.color.data[0] * scale,
        info.positions[1].data[0], info.positions[1].data[1], info.positions[1].data[2], info.color.data[1] * scale,
        info.positions[2].data[0], info.positions[2].data[1], info.positions[2].data[2], info.color.data[2] * scale,
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
    sphericalLights.push_back(info);
}

void LightManagerQ2::SelectSphericalLights(const float cameraPosition[3])
{
    dynLights.clear();
    dynLightCount = 0;

    const size_t selectedCount =
        std::min(sphericalLights.size(), static_cast<size_t>(MAX_LIGHT_SOURCES));

    if (sphericalLights.size() == selectedCount)
    {
        for (const RgSphericalLightUploadInfo &light : sphericalLights)
        {
            AppendSphericalLight(dynLights, light);
        }
    }
    else
    {
        sphericalLightContributions.resize(sphericalLights.size());
        sphericalLightOrder.resize(sphericalLights.size());
        std::iota(sphericalLightOrder.begin(), sphericalLightOrder.end(),
                  static_cast<size_t>(0));

        for (size_t i = 0; i < sphericalLights.size(); i++)
        {
            sphericalLightContributions[i] =
                GetSphericalLightContribution(sphericalLights[i], cameraPosition);
        }

        const auto moreImportant = [this](size_t a, size_t b)
        {
            if (sphericalLightContributions[a] != sphericalLightContributions[b])
            {
                return sphericalLightContributions[a] > sphericalLightContributions[b];
            }

            if (sphericalLights[a].uniqueID != sphericalLights[b].uniqueID)
            {
                return sphericalLights[a].uniqueID < sphericalLights[b].uniqueID;
            }

            return a < b;
        };

        std::partial_sort(sphericalLightOrder.begin(),
                          sphericalLightOrder.begin() + selectedCount,
                          sphericalLightOrder.end(),
                          moreImportant);
        sphericalLightOrder.resize(selectedCount);

        // Upload order is stable across frames. Keep it for the selected set
        // so stochastic light indices do not churn whenever two scores swap.
        std::sort(sphericalLightOrder.begin(), sphericalLightOrder.end());

        for (size_t index : sphericalLightOrder)
        {
            AppendSphericalLight(dynLights, sphericalLights[index]);
        }
    }

    dynLightCount = static_cast<uint32_t>(selectedCount);
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

void LightManagerQ2::Submit(uint32_t frameId, const float cameraPosition[3])
{
    SelectSphericalLights(cameraPosition);
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
