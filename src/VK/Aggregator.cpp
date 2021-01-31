#include "Aggregator.h"

#define WG_SIZE_XY 32

void Aggregator::OnCreate(
	Device* pDevice, 
	ResourceViewHeaps* pResourceViewHeaps, 
	DynamicBufferRing* pDynamicBufferRing)
{
    this->pDevice = pDevice;
    this->pDynamicBufferRing = pDynamicBufferRing;
    this->pResourceViewHeaps = pResourceViewHeaps;

    //  create default sampler for i-light input
    {
        VkSamplerCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        info.magFilter = VK_FILTER_LINEAR;
        info.minFilter = VK_FILTER_LINEAR;
        info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.minLod = -1000;
        info.maxLod = 1000;
        info.maxAnisotropy = 1.0f;
        VkResult res = vkCreateSampler(pDevice->GetDevice(), &info, NULL, &this->sampler_default);
        assert(res == VK_SUCCESS);
    }

    //  create pipeline
    DefineList defines;
    this->createDescriptors(&defines);
    this->aggregator.OnCreate(
        this->pDevice, "Aggregator.glsl", "main", "", 
        this->descriptorSetLayout, 0, 0, 0, &defines);
}

void Aggregator::OnDestroy()
{
    vkDestroySampler(this->pDevice->GetDevice(), this->sampler_default, nullptr);

    this->aggregator.OnDestroy();

    this->pResourceViewHeaps->FreeDescriptor(this->descriptorSet);
    vkDestroyDescriptorSetLayout(this->pDevice->GetDevice(), this->descriptorSetLayout, NULL);

    pDevice = nullptr;
    pResourceViewHeaps = nullptr;
    pDynamicBufferRing = nullptr;
}

void Aggregator::setInputImages(VkImageView dLight, VkImageView iLight, uint32_t dLightWidth, uint32_t dLightHeight)
{
    this->outWidth = dLightWidth;
    this->outHeight = dLightHeight;

    VkDescriptorImageInfo imgInfos[2];
    VkWriteDescriptorSet writes[2];

    //  of d-light input
    imgInfos[0].sampler = VK_NULL_HANDLE;
    imgInfos[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    imgInfos[0].imageView = dLight;

    writes[0] = {};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].pNext = NULL;
    writes[0].dstSet = this->descriptorSet;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[0].pImageInfo = &imgInfos[0];
    writes[0].dstBinding = 1;
    writes[0].dstArrayElement = 0;

    //  of i-light input
    imgInfos[1].sampler = this->sampler_default;
    imgInfos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgInfos[1].imageView = iLight;

    writes[1] = writes[0];
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo = &imgInfos[1];
    writes[1].dstBinding = 2;

    vkUpdateDescriptorSets(this->pDevice->GetDevice(), 2, writes, 0, NULL);
}

void Aggregator::Draw(VkCommandBuffer cmdbuf, float weight)
{
    ::SetPerfMarkerBegin(cmdbuf, "Aggregator");

    //  update constants
    VkDescriptorBufferInfo descInfo_constants;
    {
        Aggregator::Constants* pAllocData;
        this->pDynamicBufferRing->AllocConstantBuffer(sizeof(Aggregator::Constants), (void**)&pAllocData, &descInfo_constants);
        pAllocData->weight = weight;
        pAllocData->imgWidth = this->outWidth;
        pAllocData->imgHeight = this->outHeight;
    }

    //  dispatch
    uint32_t numWG_x = (this->outWidth + WG_SIZE_XY - 1) / WG_SIZE_XY;
    uint32_t numWG_y = (this->outHeight + WG_SIZE_XY - 1) / WG_SIZE_XY;
    this->aggregator.Draw(cmdbuf, &descInfo_constants, this->descriptorSet, numWG_x, numWG_y, 1);

    ::SetPerfMarkerEnd(cmdbuf);
}

void Aggregator::createDescriptors(DefineList* pDefines)
{
    //  define bindings
    std::vector<VkDescriptorSetLayoutBinding> layoutBindings(3);
    layoutBindings[0].binding = 0;
    layoutBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    layoutBindings[0].descriptorCount = 1;
    layoutBindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    layoutBindings[0].pImmutableSamplers = NULL;
    (*pDefines)["ID_Params"] = std::to_string(layoutBindings[0].binding);

    layoutBindings[1].binding = 1;
    layoutBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    layoutBindings[1].descriptorCount = 1;
    layoutBindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    layoutBindings[1].pImmutableSamplers = NULL;
    (*pDefines)["ID_DLight"] = std::to_string(layoutBindings[1].binding);

    layoutBindings[2].binding = 2;
    layoutBindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    layoutBindings[2].descriptorCount = 1;
    layoutBindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    layoutBindings[2].pImmutableSamplers = NULL;
    (*pDefines)["ID_ILight"] = std::to_string(layoutBindings[2].binding);

    //  allocate descriptor
    this->pResourceViewHeaps->CreateDescriptorSetLayoutAndAllocDescriptorSet(
        &layoutBindings,
        &this->descriptorSetLayout,
        &this->descriptorSet);

    //  update binding 0 (dynamic buffer)
    this->pDynamicBufferRing->SetDescriptorSet(0, sizeof(Aggregator::Constants), this->descriptorSet);
}
