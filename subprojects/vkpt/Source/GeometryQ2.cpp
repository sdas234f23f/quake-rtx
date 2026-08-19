#include "GeometryQ2.h"

#include "CommandBufferManager.h"
#include "Utils.h"
#include "VertexBufferQ2.h"

// Q2RTX binding contract: the vendored dual C/GLSL header defines
// VboPrimitive and the primitive/position buffer layout. constants.h must
// come first.
#include "../q2rtx-shaders/constants.h"
#include "../q2rtx-shaders/vertex_buffer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace vkpt;

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
                       std::shared_ptr<VertexBufferQ2> _vertexBufferQ2)
: device(_device),
  allocator(std::move(_allocator)),
  cmdManager(std::move(_cmdManager)),
  vertexBufferQ2(std::move(_vertexBufferQ2)),
  uploadFence(VK_NULL_HANDLE),
  worldPrimCount(0),
  hasWorldData(false)
{
    VkFenceCreateInfo fenceInfo = {};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

    VkResult r = vkCreateFence(device, &fenceInfo, nullptr, &uploadFence);
    VK_CHECKERROR(r);
}

GeometryQ2::~GeometryQ2()
{
    worldBuffer.Destroy();
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
}

void GeometryQ2::AddStaticGeometry(const RgGeometryUploadInfo &uploadInfo)
{
    if (uploadInfo.geomType != RG_GEOMETRY_TYPE_STATIC &&
        uploadInfo.geomType != RG_GEOMETRY_TYPE_STATIC_MOVABLE)
    {
        // Dynamic / instanced geometry is handled in stage G1b.
        return;
    }

    const uint32_t triCount = uploadInfo.indexCount ? uploadInfo.indexCount / 3
                                                    : uploadInfo.vertexCount / 3;

    worldPrimCount += triCount;
    world.primitives.reserve(world.primitives.size() + triCount * sizeof(VboPrimitive));
    world.positions.reserve(world.positions.size() + triCount * 9 * sizeof(float));

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

    for (uint32_t t = 0; t < triCount; t++)
    {
        const uint32_t idx[3] =
        {
            uploadInfo.pIndices ? uploadInfo.pIndices[t * 3 + 0] : t * 3 + 0,
            uploadInfo.pIndices ? uploadInfo.pIndices[t * 3 + 1] : t * 3 + 1,
            uploadInfo.pIndices ? uploadInfo.pIndices[t * 3 + 2] : t * 3 + 2,
        };

        VboPrimitive prim = {};
        for (int v = 0; v < 3; v++)
        {
            const RgVertex &vert = uploadInfo.pVertices[idx[v]];

            float worldPos[3];
            TransformPoint(vert.position, worldPos);

            float worldNormal[3];
            TransformNormal(vert.normal, worldNormal);

            float *posOut = (v == 0) ? prim.pos0 : (v == 1) ? prim.pos1 : prim.pos2;
            posOut[0] = worldPos[0];
            posOut[1] = worldPos[1];
            posOut[2] = worldPos[2];

            uint32_t *normalOut = (v == 0) ? &prim.normals[0]
                                           : (v == 1) ? &prim.normals[1] : &prim.normals[2];
            *normalOut = EncodeNormal(worldNormal[0], worldNormal[1], worldNormal[2]);
        }

        // uv / material / cluster / shell. Material id is 0 for now (stage G6
        // fills the real material table); alpha comes from the first layer.
        prim.uv0[0] = uploadInfo.pVertices[idx[0]].texCoord[0];
        prim.uv0[1] = uploadInfo.pVertices[idx[0]].texCoord[1];
        prim.uv1[0] = uploadInfo.pVertices[idx[0]].texCoordLayer1[0];
        prim.uv1[1] = uploadInfo.pVertices[idx[0]].texCoordLayer1[1];
        prim.uv2[0] = uploadInfo.pVertices[idx[0]].texCoordLayer2[0];
        prim.uv2[1] = uploadInfo.pVertices[idx[0]].texCoordLayer2[1];

        prim.cluster = static_cast<int32_t>(uploadInfo.pVertices[idx[0]].cluster);

        const float alpha = uploadInfo.layerColors[0].data[3];
        const float emissive = uploadInfo.defaultEmission;
        prim.emissive_and_alpha = PackHalf2x16(emissive, alpha);

        const size_t primOffset = world.primitives.size();
        world.primitives.resize(primOffset + sizeof(VboPrimitive));
        std::memcpy(world.primitives.data() + primOffset, &prim, sizeof(VboPrimitive));

        // Append the three world-space positions for the BLAS source buffer.
        for (int v = 0; v < 3; v++)
        {
            const RgVertex &vert = uploadInfo.pVertices[idx[v]];
            float worldPos[3];
            TransformPoint(vert.position, worldPos);

            const size_t posOffset = world.positions.size();
            world.positions.resize(posOffset + 3 * sizeof(float));
            std::memcpy(world.positions.data() + posOffset, worldPos, sizeof(worldPos));
        }
    }

    hasWorldData = true;
}

void GeometryQ2::SubmitStatic()
{
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
