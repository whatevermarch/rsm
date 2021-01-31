#pragma once

class Aggregator
{
public:
    struct Constants
    {
        float weight;

        uint32_t imgWidth, imgHeight;
        float padding;
    };

    void OnCreate(
        Device* pDevice, 
        ResourceViewHeaps* pResourceViewHeaps, 
        DynamicBufferRing* pDynamicBufferRing);
    void OnDestroy();

    void setInputImages(VkImageView dLight, VkImageView iLight, 
        uint32_t dLightWidth, uint32_t dLightHeight);

    void Draw(VkCommandBuffer cmdbuf, float weight);

private:
    Device* pDevice = nullptr;
    ResourceViewHeaps* pResourceViewHeaps = nullptr;
    DynamicBufferRing* pDynamicBufferRing = nullptr;

    PostProcCS aggregator;

    uint32_t outWidth = 0, outHeight = 0;
    VkSampler sampler_default;

    VkDescriptorSet       descriptorSet;
    VkDescriptorSetLayout descriptorSetLayout;

    void createDescriptors(DefineList* pDefines);
};
