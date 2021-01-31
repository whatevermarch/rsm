#pragma once

#define DLIGHT_NUM_DESCRIPTOR_SETS 3

struct DLightInput
{
    struct CameraGBuffer
    {
        static const uint32_t numImageViews = 4;

        VkImageView worldCoord,
            normal,
            diffuse,
            specular;
    };

    struct LightGBuffer
    {
        static const uint32_t numImageViews = 1;

        VkImageView depth;
    };
};

class DirectLighting
{
public:

    //  get attachment infos. of this pass
    //  return true if it consists of depth attachment (at the end of the list)
    static bool getAttachmentDesc(
        std::vector<VkAttachmentDescription>& attachments);

    struct per_frame
    {
        XMVECTOR cameraPos;
        Light light;
    };

    void OnCreate(
        Device* pDevice,
        UploadHeap* pUploadHeap,
        ResourceViewHeaps* pHeaps,
        DynamicBufferRing* pDynamicBufferRing,
        StaticBufferPool* pStaticBufferPool,
        VkRenderPass renderPass = VK_NULL_HANDLE);
    void OnDestroy();

    void OnCreateWindowSizeDependentResources(uint32_t Width, uint32_t Height, GBuffer* pGBuffer);
    void OnDestroyWindowSizeDependentResources();

    void setCameraGBuffer(DLightInput::CameraGBuffer* pCamSRVs);
    void setLightGBuffer(DLightInput::LightGBuffer* pLightSRVs);

    DirectLighting::per_frame* SetPerFrameConstants();

    void Draw(VkCommandBuffer commandBuffer, VkRect2D* renderArea);

protected:

    uint32_t hdrWidth = 0, hdrHeight = 0;

    Device* pDevice = nullptr;
    ResourceViewHeaps* pResourceViewHeaps = nullptr;
    DynamicBufferRing* pDynamicBufferRing = nullptr;
    StaticBufferPool* pStaticBufferPool = nullptr;

    DirectLighting::per_frame perFrame;
    VkDescriptorBufferInfo descInfo_perFrame;
    VkSampler sampler_shadow;

    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;

    //  set 0: general, set 1: RSM, set 2: GeomBuffer
    VkDescriptorSet descriptorSets[DLIGHT_NUM_DESCRIPTOR_SETS];
    VkDescriptorSetLayout descriptorSetLayouts[DLIGHT_NUM_DESCRIPTOR_SETS];

    bool useExternalRenderPass = false;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    std::vector<VkClearValue> clearValues;

    void createRenderPass();
    void createDescriptors(DefineList* pAttributeDefines);
    void createPipeline(const DefineList* defines);
};

