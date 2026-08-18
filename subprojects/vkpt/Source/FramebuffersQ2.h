// Q2RTX-convention framebuffers + textures descriptor set (PORTING.md, S2b-2).
//
// Creates the images declared by q2rtx-shaders/global_textures.h
// (LIST_IMAGES / LIST_IMAGES_A_B, 68 entries) and a descriptor set that
// matches the Q2RTX GLOBAL_TEXTURES_DESC_SET_IDX layout (global texture
// array at binding 0, framebuffer images from BINDING_OFFSET_IMAGES,
// framebuffer textures from BINDING_OFFSET_TEXTURES). It is NOT bound to any
// pipeline yet; the images are allocated so Q2RTX shaders can be swapped in
// one module at a time.

#pragma once

#include "Common.h"
#include "MemoryAllocator.h"
#include "CommandBufferManager.h"

namespace vkpt
{

class FramebuffersQ2
{
public:
    FramebuffersQ2(VkDevice device,
                   std::shared_ptr<MemoryAllocator> allocator,
                   std::shared_ptr<CommandBufferManager> cmdManager);
    ~FramebuffersQ2();

    FramebuffersQ2(const FramebuffersQ2 &other) = delete;
    FramebuffersQ2(FramebuffersQ2 &&other) noexcept = delete;
    FramebuffersQ2 &operator=(const FramebuffersQ2 &other) = delete;
    FramebuffersQ2 &operator=(FramebuffersQ2 &&other) noexcept = delete;

    // (Re)creates the framebuffer images when the render size changes.
    void Create(uint32_t renderWidth, uint32_t renderHeight, uint32_t deviceCount);

    VkDescriptorSet GetDescSet() const;
    VkDescriptorSetLayout GetDescSetLayout() const;
    VkImage GetImage(int index) const;

private:
    struct ImageEntry
    {
        VkImage       image;
        VkImageView   storageView;
        VkImageView   sampledView;
        VkDeviceMemory memory;
    };

    void DestroyImages();
    void CreateImages();
    void CreateImageEntry(int index, const VkImageCreateInfo &info, const char *name);
    void CreateWhiteTexture();
    void DestroyWhiteTexture();
    void CreateDescriptors();
    void UpdateDescriptors();

private:
    VkDevice device;
    std::shared_ptr<MemoryAllocator> allocator;
    std::shared_ptr<CommandBufferManager> cmdManager;

    uint32_t width;
    uint32_t height;
    uint32_t deviceCount;

    ImageEntry *images;   // indexed by the QVK_IMAGES enum from global_textures.h

    VkSampler    sampler;
    VkImage      whiteImage;
    VkImageView  whiteImageView;
    VkDeviceMemory whiteMemory;

    VkDescriptorPool      descPool;
    VkDescriptorSetLayout descSetLayout;
    VkDescriptorSet       descSet;
};

}
