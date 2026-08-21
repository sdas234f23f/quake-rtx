// GeometryQ2: converts legacy vkQuake geometry into the Q2RTX VboPrimitive
// format and uploads it to the Q2RTX world buffer (stage G1 of the porting
// plan, see PORTING.md).
//
// Part of the Q2RTX binding layer (stage S2b / Block 2): the legacy renderer
// keeps rendering as before, while this module builds a parallel Q2RTX-
// compatible vertex buffer (primitive array + BLAS source positions) from the
// RgGeometryUploadInfo that already flows through VulkanDevice::UploadGeometry.
// Nothing is visible yet - the buffer is only filled and bound into the
// VertexBufferQ2 descriptor set, which no Q2RTX shader consumes so far.

#pragma once

#include "Buffer.h"
#include "Common.h"
#include "vkpt/vkpt.h"

#include <vector>

namespace vkpt
{

class CommandBufferManager;
class VertexBufferQ2;

class GeometryQ2
{
public:
    GeometryQ2(VkDevice device,
               std::shared_ptr<MemoryAllocator> allocator,
               std::shared_ptr<CommandBufferManager> cmdManager,
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
    // world space. Dynamic geometry is ignored for now (stage G1b).
    void AddStaticGeometry(const RgGeometryUploadInfo &uploadInfo);

    // Called after the legacy static scene is submitted. Copies the gathered
    // CPU data into the Q2RTX world buffer and updates the descriptors.
    void SubmitStatic();

    uint32_t GetWorldPrimitiveCount() const;
    VkBuffer GetWorldBuffer() const;
    VkDeviceAddress GetWorldBufferAddress() const;
    // Offset of the BLAS source positions inside the world buffer.
    VkDeviceSize GetWorldPositionOffset() const;

private:
    struct WorldData
    {
        std::vector<uint8_t> primitives;
        std::vector<uint8_t> positions;
    };

    void UploadToDevice(WorldData &&data);

    VkDevice device;
    std::shared_ptr<MemoryAllocator> allocator;
    std::shared_ptr<CommandBufferManager> cmdManager;
    std::shared_ptr<VertexBufferQ2> vertexBufferQ2;

    WorldData world;
    Buffer worldBuffer;
    VkFence uploadFence;
    uint32_t worldPrimCount;
    bool hasWorldData;

    // Q2 material table entries collected from RgGeometryUploadInfo::
    // pQ2Material during static uploads: MATERIAL_UINTS uints per entry,
    // packed like Q2RTX material_table (vertex_buffer.h). Material index 0
    // is empty, 1 is the default white material. Written to the LightBuffer
    // in SubmitStatic (stage G6).
    std::vector<uint32_t> materialTable;
    uint32_t materialCount;
};

}
