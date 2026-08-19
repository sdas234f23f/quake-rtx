#include "FramebuffersQ2.h"

// Q2RTX binding contract: the vendored dual C/GLSL header. In C mode
// (VKPT_SHADER undefined) it defines the QVK_IMAGES enum and the image
// list macros (LIST_IMAGES / LIST_IMAGES_A_B) with the formats and sizes.
// MAX_RIMAGES is the host-side size of the global texture array; it must
// match NUM_GLOBAL_TEXTURES (checked inside the header).
#define MAX_RIMAGES 8192
#include "../q2rtx-shaders/global_textures.h"

#include <cstring>
#include <vector>

using namespace vkpt;

// The size macros in global_textures.h reference a `qvk` global (like Q2RTX
// does). This is the minimal stand-in needed to expand the IMG_DO size
// arguments; it is updated by FramebuffersQ2::Create.
struct
{
    VkExtent2D extent_screen_images;
    VkExtent2D extent_unscaled;
    VkExtent2D extent_taa_images;
    int        device_count;
} qvk = {};

static_assert(NUM_IMAGES == NUM_VKPT_IMAGES, "image count mismatch vs global_textures.h");

FramebuffersQ2::FramebuffersQ2(VkDevice _device,
                               std::shared_ptr<MemoryAllocator> _allocator,
                               std::shared_ptr<CommandBufferManager> _cmdManager)
:
    device(_device),
    allocator(std::move(_allocator)),
    cmdManager(std::move(_cmdManager)),
    width(0),
    height(0),
    deviceCount(1),
    images(nullptr),
    sampler(VK_NULL_HANDLE),
    whiteImage(VK_NULL_HANDLE),
    whiteImageView(VK_NULL_HANDLE),
    whiteMemory(VK_NULL_HANDLE),
    blueNoiseImageView(VK_NULL_HANDLE),
    descPool(VK_NULL_HANDLE),
    descSetLayout(VK_NULL_HANDLE),
    descSet(VK_NULL_HANDLE)
{
    // Value-initialize: DestroyImages() runs before the first Create() and
    // must not touch garbage handles.
    images = new ImageEntry[NUM_VKPT_IMAGES]();
    for (int i = 0; i < static_cast<int>(NUM_VKPT_IMAGES); i++)
    {
        images[i] = {};
    }

    VkSamplerCreateInfo samplerInfo = {};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 1.0f;

    VkResult r = vkCreateSampler(device, &samplerInfo, nullptr, &sampler);
    VK_CHECKERROR(r);

    CreateDescriptors();
}

FramebuffersQ2::~FramebuffersQ2()
{
    DestroyImages();

    if (whiteImageView)
    {
        vkDestroyImageView(device, whiteImageView, nullptr);
    }
    if (whiteImage)
    {
        allocator->DestroyTextureImage(whiteImage);
    }
    vkDestroySampler(device, sampler, nullptr);

    vkDestroyDescriptorPool(device, descPool, nullptr);
    vkDestroyDescriptorSetLayout(device, descSetLayout, nullptr);

    delete[] images;
}

void FramebuffersQ2::Create(uint32_t _width, uint32_t _height, uint32_t _deviceCount)
{
    if (_width == width && _height == height && _deviceCount == deviceCount)
    {
        return;
    }

    DestroyImages();
    DestroyWhiteTexture();

    width = _width;
    height = _height;
    deviceCount = _deviceCount;

    // Sizes for the header macros. Full precision (render) resolution is
    // used for all extents for now; refined when the shaders get bound.
    qvk.extent_screen_images = { width, height };
    qvk.extent_unscaled      = { width, height };
    qvk.extent_taa_images    = { width, height };
    qvk.device_count         = static_cast<int>(deviceCount);

    CreateImages();
    CreateWhiteTexture();

    UpdateDescriptors();
}

void FramebuffersQ2::DestroyImages()
{
    if (!images)
    {
        return;
    }
    for (int i = 0; i < NUM_VKPT_IMAGES; i++)
    {
        if (images[i].sampledView)
        {
            vkDestroyImageView(device, images[i].sampledView, nullptr);
        }
        if (images[i].storageView)
        {
            vkDestroyImageView(device, images[i].storageView, nullptr);
        }
        if (images[i].image)
        {
            allocator->DestroyTextureImage(images[i].image);
        }
        images[i] = {};
    }
}

void FramebuffersQ2::CreateImages()
{
#define IMG_DO(_name, _binding, _vkformat, _glslformat, _w, _h)               \
    {                                                                         \
        VkImageCreateInfo info = {};                                          \
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;                     \
        info.imageType = VK_IMAGE_TYPE_2D;                                    \
        info.format = VK_FORMAT_##_vkformat;                                  \
        info.extent = { static_cast<uint32_t>(_w), static_cast<uint32_t>(_h), 1 }; \
        info.mipLevels = 1;                                                   \
        info.arrayLayers = 1;                                                 \
        info.samples = VK_SAMPLE_COUNT_1_BIT;                                 \
        info.tiling = VK_IMAGE_TILING_OPTIMAL;                                \
        info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | \
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT;                         \
        CreateImageEntry(VKPT_IMG_##_name, info, #_name);                     \
    }
    LIST_IMAGES
    LIST_IMAGES_A_B
#undef IMG_DO
}

void FramebuffersQ2::CreateImageEntry(int index, const VkImageCreateInfo &info, const char *name)
{
    ImageEntry &entry = images[index];

    entry.image = allocator->CreateDstTextureImage(&info, name);

    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = entry.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = info.format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    VkResult r = vkCreateImageView(device, &viewInfo, nullptr, &entry.storageView);
    VK_CHECKERROR(r);
    r = vkCreateImageView(device, &viewInfo, nullptr, &entry.sampledView);
    VK_CHECKERROR(r);
}

void FramebuffersQ2::CreateWhiteTexture()
{
    VkImageCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = { 1, 1, 1 };
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    whiteImage = allocator->CreateDstTextureImage(&info, "q2rtx white texture");

    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = whiteImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = info.format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;

    VkResult r = vkCreateImageView(device, &viewInfo, nullptr, &whiteImageView);
    VK_CHECKERROR(r);
}

void FramebuffersQ2::DestroyWhiteTexture()
{
    if (whiteImageView)
    {
        vkDestroyImageView(device, whiteImageView, nullptr);
        whiteImageView = VK_NULL_HANDLE;
    }
    if (whiteImage)
    {
        allocator->DestroyTextureImage(whiteImage);
        whiteImage = VK_NULL_HANDLE;
    }
}

void FramebuffersQ2::CreateDescriptors()
{
    // binding 0 (global texture array) + IMG_* + TEX_* + BLUE_NOISE.
    const uint32_t bindingCount = 1 + NUM_IMAGES + NUM_IMAGES + 1;

    std::vector<VkDescriptorSetLayoutBinding> bindings(bindingCount);

    // binding 0: global texture array
    bindings[0].binding = GLOBAL_TEXTURES_TEX_ARR_BINDING_IDX;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = NUM_GLOBAL_TEXTURES;
    bindings[0].stageFlags = VK_SHADER_STAGE_ALL;

    // framebuffer images (IMG_*) and textures (TEX_*)
    for (int i = 0; i < static_cast<int>(NUM_IMAGES); i++)
    {
        bindings[1 + i].binding = BINDING_OFFSET_IMAGES + i;
        bindings[1 + i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[1 + i].descriptorCount = 1;
        bindings[1 + i].stageFlags = VK_SHADER_STAGE_ALL;

        bindings[1 + NUM_IMAGES + i].binding = BINDING_OFFSET_TEXTURES + i;
        bindings[1 + NUM_IMAGES + i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[1 + NUM_IMAGES + i].descriptorCount = 1;
        bindings[1 + NUM_IMAGES + i].stageFlags = VK_SHADER_STAGE_ALL;
    }

    // blue noise array (TEX_BLUE_NOISE)
    bindings[1 + NUM_IMAGES + NUM_IMAGES].binding = BINDING_OFFSET_BLUE_NOISE;
    bindings[1 + NUM_IMAGES + NUM_IMAGES].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1 + NUM_IMAGES + NUM_IMAGES].descriptorCount = 1;
    bindings[1 + NUM_IMAGES + NUM_IMAGES].stageFlags = VK_SHADER_STAGE_ALL;

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = bindings.size();
    layoutInfo.pBindings = bindings.data();

    VkResult r = vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &descSetLayout);
    VK_CHECKERROR(r);

    VkDescriptorPoolSize poolSizes[2] = {};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = NUM_GLOBAL_TEXTURES + NUM_IMAGES + 1; // + blue noise
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[1].descriptorCount = NUM_IMAGES;

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
}

void FramebuffersQ2::UpdateDescriptors()
{
    std::vector<VkWriteDescriptorSet> writes;
    std::vector<VkDescriptorImageInfo> imageInfos;

    // binding 0: the whole global texture array points at the white texture
    // for now (real textures come with the texture-manager port).
    {
        VkDescriptorImageInfo white = {};
        white.sampler = sampler;
        white.imageView = whiteImageView;
        white.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        for (int i = 0; i < static_cast<int>(NUM_GLOBAL_TEXTURES); i++)
        {
            imageInfos.push_back(white);
        }

        VkWriteDescriptorSet write = {};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = descSet;
        write.dstBinding = GLOBAL_TEXTURES_TEX_ARR_BINDING_IDX;
        write.dstArrayElement = 0;
        write.descriptorCount = NUM_GLOBAL_TEXTURES;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = imageInfos.data();
        writes.push_back(write);
    }

    // framebuffer images and textures
    for (int i = 0; i < static_cast<int>(NUM_IMAGES); i++)
    {
        VkDescriptorImageInfo storage = {};
        storage.imageView = images[i].storageView;
        storage.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writeStorage = {};
        writeStorage.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writeStorage.dstSet = descSet;
        writeStorage.dstBinding = BINDING_OFFSET_IMAGES + i;
        writeStorage.descriptorCount = 1;
        writeStorage.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writeStorage.pImageInfo = &storage;
        writes.push_back(writeStorage);

        VkDescriptorImageInfo sampled = {};
        sampled.sampler = sampler;
        sampled.imageView = images[i].sampledView;
        // Q2RTX keeps every framebuffer image in GENERAL for the whole frame
        // (imageLoad/imageStore and texelFetch/textureLod both work from it),
        // so the sampled descriptors must declare GENERAL as well.
        sampled.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writeSampled = {};
        writeSampled.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writeSampled.dstSet = descSet;
        writeSampled.dstBinding = BINDING_OFFSET_TEXTURES + i;
        writeSampled.descriptorCount = 1;
        writeSampled.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writeSampled.pImageInfo = &sampled;
        writes.push_back(writeSampled);
    }

    // blue noise array
    {
        VkDescriptorImageInfo blueNoise = {};
        blueNoise.sampler = sampler;
        blueNoise.imageView = blueNoiseImageView;
        blueNoise.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write = {};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = descSet;
        write.dstBinding = BINDING_OFFSET_BLUE_NOISE;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &blueNoise;
        writes.push_back(write);
    }

    vkUpdateDescriptorSets(device, writes.size(), writes.data(), 0, nullptr);
}

void FramebuffersQ2::SetBlueNoiseImageView(VkImageView view)
{
    blueNoiseImageView = view;
    // The descriptor set already exists; re-write the blue noise binding.
    UpdateDescriptors();
}

VkDescriptorSet FramebuffersQ2::GetDescSet() const
{
    return descSet;
}

VkDescriptorSetLayout FramebuffersQ2::GetDescSetLayout() const
{
    return descSetLayout;
}

VkImage FramebuffersQ2::GetImage(int index) const
{
    if (index < 0 || index >= static_cast<int>(NUM_VKPT_IMAGES))
    {
        return VK_NULL_HANDLE;
    }
    return images[index].image;
}
