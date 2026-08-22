#include "FramebuffersQ2.h"

// Q2RTX binding contract: the vendored dual C/GLSL header. In C mode
// (VKPT_SHADER undefined) it defines the QVK_IMAGES enum and the image
// list macros (LIST_IMAGES / LIST_IMAGES_A_B) with the formats and sizes.
// MAX_RIMAGES is the host-side size of the global texture array; it must
// match NUM_GLOBAL_TEXTURES (checked inside the header).
#define MAX_RIMAGES 8192
#include "../q2rtx-shaders/global_textures.h"
#include "Generated/ShaderCommonC.h"

#include <algorithm>
#include <cstring>
#include <utility>
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
    placeholderCubeImage(VK_NULL_HANDLE),
    placeholderCubeView(VK_NULL_HANDLE),
    placeholder3DImage(VK_NULL_HANDLE),
    placeholder3DView(VK_NULL_HANDLE),
    placeholderStorageImage(VK_NULL_HANDLE),
    placeholderStorageView(VK_NULL_HANDLE),
    descPool(VK_NULL_HANDLE),
    descSetLayout(VK_NULL_HANDLE),
    descSets{},
    activeFrameIndex(0),
    needsImageTransition(true)
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
    CreatePlaceholders();
}

FramebuffersQ2::~FramebuffersQ2()
{
    DestroyImages();
    DestroyPlaceholders();

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
    needsImageTransition = true;

    // Sizes for the header macros. Full precision (render) resolution is
    // used for all extents for now; refined when the shaders get bound.
    qvk.extent_screen_images = { width, height };
    qvk.extent_unscaled      = { width, height };
    qvk.extent_taa_images    = { width, height };
    qvk.device_count         = static_cast<int>(deviceCount);

    CreateImages();
    CreateWhiteTexture();

    UpdateAllDescriptors();
}

bool FramebuffersQ2::TakeImageTransition()
{
    return std::exchange(needsImageTransition, false);
}

void FramebuffersQ2::TransitionImagesToGeneral(VkCommandBuffer cmd)
{
    if (!images)
    {
        return;
    }

    std::vector<VkImageMemoryBarrier> barriers;
    barriers.reserve(NUM_VKPT_IMAGES);
    for (int i = 0; i < static_cast<int>(NUM_VKPT_IMAGES); i++)
    {
        if (!images[i].image)
        {
            continue;
        }
        VkImageMemoryBarrier b = {};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.image = images[i].image;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcAccessMask = 0;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.baseMipLevel = 0;
        b.subresourceRange.levelCount = 1;
        b.subresourceRange.baseArrayLayer = 0;
        b.subresourceRange.layerCount = 1;
        barriers.push_back(b);
    }

    if (barriers.empty())
    {
        return;
    }

    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 0, nullptr, 0, nullptr,
                         static_cast<uint32_t>(barriers.size()), barriers.data());
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
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT; \
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

void FramebuffersQ2::CreatePlaceholders()
{
    // 1x1 cube for the samplerCube bindings (envmap, physical sky, terrain).
    {
        VkImageCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = VK_FORMAT_R8G8B8A8_UNORM;
        info.extent = { 1, 1, 1 };
        info.mipLevels = 1;
        info.arrayLayers = 6;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

        placeholderCubeImage = allocator->CreateDstTextureImage(&info, "q2rtx cube placeholder");

        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = placeholderCubeImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
        viewInfo.format = info.format;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 6;

        VkResult r = vkCreateImageView(device, &viewInfo, nullptr, &placeholderCubeView);
        VK_CHECKERROR(r);
    }

    // 1x1x1 3D image for the sampler3D bindings (sky scattering, sky clouds).
    {
        VkImageCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_3D;
        info.format = VK_FORMAT_R8G8B8A8_UNORM;
        info.extent = { 1, 1, 1 };
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

        placeholder3DImage = allocator->CreateDstTextureImage(&info, "q2rtx 3d placeholder");

        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = placeholder3DImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_3D;
        viewInfo.format = info.format;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        VkResult r = vkCreateImageView(device, &viewInfo, nullptr, &placeholder3DView);
        VK_CHECKERROR(r);
    }

    // 1x1 2D storage image for IMG_PHYSICAL_SKY (binding 140).
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
        info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

        placeholderStorageImage = allocator->CreateDstTextureImage(&info, "q2rtx physical sky placeholder");

        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = placeholderStorageImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = info.format;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        VkResult r = vkCreateImageView(device, &viewInfo, nullptr, &placeholderStorageView);
        VK_CHECKERROR(r);
    }
}

void FramebuffersQ2::DestroyPlaceholders()
{
    if (placeholderStorageView)
    {
        vkDestroyImageView(device, placeholderStorageView, nullptr);
        placeholderStorageView = VK_NULL_HANDLE;
    }
    if (placeholderStorageImage)
    {
        allocator->DestroyTextureImage(placeholderStorageImage);
        placeholderStorageImage = VK_NULL_HANDLE;
    }
    if (placeholder3DView)
    {
        vkDestroyImageView(device, placeholder3DView, nullptr);
        placeholder3DView = VK_NULL_HANDLE;
    }
    if (placeholder3DImage)
    {
        allocator->DestroyTextureImage(placeholder3DImage);
        placeholder3DImage = VK_NULL_HANDLE;
    }
    if (placeholderCubeView)
    {
        vkDestroyImageView(device, placeholderCubeView, nullptr);
        placeholderCubeView = VK_NULL_HANDLE;
    }
    if (placeholderCubeImage)
    {
        allocator->DestroyTextureImage(placeholderCubeImage);
        placeholderCubeImage = VK_NULL_HANDLE;
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

    // Sky / terrain bindings (138..148), required by the path tracer shaders
    // (the compute shaders do not use them, but the ray tracing ones do).
    {
        const struct { uint32_t binding; VkDescriptorType type; } extra[] =
        {
            { BINDING_OFFSET_ENVMAP,             VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER },
            { BINDING_OFFSET_PHYSICAL_SKY,       VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER },
            { BINDING_OFFSET_PHYSICAL_SKY_IMG,   VK_DESCRIPTOR_TYPE_STORAGE_IMAGE },
            { BINDING_OFFSET_SKY_TRANSMITTANCE,  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER },
            { BINDING_OFFSET_SKY_SCATTERING,     VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER },
            { BINDING_OFFSET_SKY_IRRADIANCE,     VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER },
            { BINDING_OFFSET_SKY_CLOUDS,         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER },
            { BINDING_OFFSET_TERRAIN_ALBEDO,     VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER },
            { BINDING_OFFSET_TERRAIN_NORMALS,    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER },
            { BINDING_OFFSET_TERRAIN_DEPTH,      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER },
            { BINDING_OFFSET_TERRAIN_SHADOWMAP,  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER },
        };
        for (const auto &b : extra)
        {
            VkDescriptorSetLayoutBinding binding = {};
            binding.binding = b.binding;
            binding.descriptorType = b.type;
            binding.descriptorCount = 1;
            binding.stageFlags = VK_SHADER_STAGE_ALL;
            bindings.push_back(binding);
        }
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = bindings.size();
    layoutInfo.pBindings = bindings.data();

    VkResult r = vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &descSetLayout);
    VK_CHECKERROR(r);

    VkDescriptorPoolSize poolSizes[2] = {};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount =
        (NUM_GLOBAL_TEXTURES + NUM_IMAGES + 1 + 10) * MAX_FRAMES_IN_FLIGHT;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[1].descriptorCount = (NUM_IMAGES + 1) * MAX_FRAMES_IN_FLIGHT;

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
}

void FramebuffersQ2::UpdateDescriptors(VkDescriptorSet targetSet)
{
    std::vector<VkWriteDescriptorSet> writes;
    // One VkDescriptorImageInfo per written descriptor. The vector is
    // reserved so pointers into it stay valid across push_back; the previous
    // code took the address of stack temporaries, which is undefined
    // behaviour once the loop scope ends.
    std::vector<VkDescriptorImageInfo> imageInfos;
    imageInfos.reserve(NUM_GLOBAL_TEXTURES + 2 * NUM_IMAGES + 12);

    auto AddWrite = [&](uint32_t binding, VkDescriptorType type, const VkDescriptorImageInfo &info)
    {
        imageInfos.push_back(info);

        VkWriteDescriptorSet write = {};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = targetSet;
        write.dstBinding = binding;
        write.dstArrayElement = 0;
        write.descriptorCount = 1;
        write.descriptorType = type;
        write.pImageInfo = &imageInfos.back();
        writes.push_back(write);
    };

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
        write.dstSet = targetSet;
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

        AddWrite(BINDING_OFFSET_IMAGES + i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, storage);

        VkDescriptorImageInfo sampled = {};
        sampled.sampler = sampler;
        sampled.imageView = images[i].sampledView;
        // Q2RTX keeps every framebuffer image in GENERAL for the whole frame
        // (imageLoad/imageStore and texelFetch/textureLod both work from it),
        // so the sampled descriptors must declare GENERAL as well.
        sampled.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        AddWrite(BINDING_OFFSET_TEXTURES + i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sampled);
    }

    // blue noise array
    {
        VkDescriptorImageInfo blueNoise = {};
        blueNoise.sampler = sampler;
        blueNoise.imageView = blueNoiseImageView;
        blueNoise.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        AddWrite(BINDING_OFFSET_BLUE_NOISE, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, blueNoise);
    }

    // Sky / terrain bindings (138..148), used by the path tracer shaders.
    // Point them at 1x1 placeholders for now; real sky, envmap and terrain
    // data comes with later port stages.
    {
        VkDescriptorImageInfo cube = {};
        cube.sampler = sampler;
        cube.imageView = placeholderCubeView;
        cube.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        const uint32_t cubeBindings[] =
        {
            BINDING_OFFSET_ENVMAP,
            BINDING_OFFSET_PHYSICAL_SKY,
            BINDING_OFFSET_TERRAIN_ALBEDO,
            BINDING_OFFSET_TERRAIN_NORMALS,
            BINDING_OFFSET_TERRAIN_DEPTH,
        };
        for (uint32_t binding : cubeBindings)
        {
            AddWrite(binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, cube);
        }

        VkDescriptorImageInfo volume = {};
        volume.sampler = sampler;
        volume.imageView = placeholder3DView;
        volume.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        const uint32_t volumeBindings[] =
        {
            BINDING_OFFSET_SKY_SCATTERING,
            BINDING_OFFSET_SKY_CLOUDS,
        };
        for (uint32_t binding : volumeBindings)
        {
            AddWrite(binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, volume);
        }

        VkDescriptorImageInfo flat = {};
        flat.sampler = sampler;
        flat.imageView = whiteImageView;
        flat.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        const uint32_t flatBindings[] =
        {
            BINDING_OFFSET_SKY_TRANSMITTANCE,
            BINDING_OFFSET_SKY_IRRADIANCE,
            BINDING_OFFSET_TERRAIN_SHADOWMAP,
        };
        for (uint32_t binding : flatBindings)
        {
            AddWrite(binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, flat);
        }

        VkDescriptorImageInfo storage = {};
        storage.imageView = placeholderStorageView;
        storage.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        AddWrite(BINDING_OFFSET_PHYSICAL_SKY_IMG, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, storage);
    }

    vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

void FramebuffersQ2::UpdateAllDescriptors()
{
    for (VkDescriptorSet set : descSets)
    {
        UpdateDescriptors(set);
    }
}

void FramebuffersQ2::SetBlueNoiseImageView(VkImageView view)
{
    blueNoiseImageView = view;
    // Images and the white fallback are created on the first sized Create().
    if (whiteImageView != VK_NULL_HANDLE)
    {
        UpdateAllDescriptors();
    }
}

void FramebuffersQ2::PrepareForFrame(uint32_t frameIndex, VkDescriptorSet textureDescSet,
                                     uint32_t textureDescriptorCount)
{
    if (frameIndex >= MAX_FRAMES_IN_FLIGHT || textureDescSet == VK_NULL_HANDLE)
    {
        return;
    }

    activeFrameIndex = frameIndex;
    const uint32_t copyCount = std::min(textureDescriptorCount,
                                        static_cast<uint32_t>(NUM_GLOBAL_TEXTURES));
    if (copyCount == 0)
    {
        return;
    }

    VkCopyDescriptorSet copy = {};
    copy.sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET;
    copy.srcSet = textureDescSet;
    copy.srcBinding = BINDING_TEXTURES;
    copy.srcArrayElement = 0;
    copy.dstSet = descSets[frameIndex];
    copy.dstBinding = GLOBAL_TEXTURES_TEX_ARR_BINDING_IDX;
    copy.dstArrayElement = 0;
    copy.descriptorCount = copyCount;

    vkUpdateDescriptorSets(device, 0, nullptr, 1, &copy);
}

VkDescriptorSet FramebuffersQ2::GetDescSet() const
{
    return descSets[activeFrameIndex];
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
