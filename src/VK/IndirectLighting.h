#pragma once

#define ILIGHT_NUM_DESCRIPTOR_SETS 3

struct ILightInput
{
    struct CameraGBuffer
    {
        static const uint32_t numImageViews = 2;

        VkImageView worldCoord,
            normal;
    };

    struct LightGBuffer
    {
        static const uint32_t numImageViews = 3;

        VkImageView worldCoord,
            normal,
            flux;
    };
};

class IndirectLighting
{
public:

    //  get attachment infos. of this pass
    //  return true if it consists of depth attachment (at the end of the list)
    static bool getAttachmentDesc(
        std::vector<VkAttachmentDescription>& attachments);

    //  ToDo : edit uniform per_frame here!
    struct per_frame
    {
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

    void OnCreateWindowSizeDependentResources(uint32_t Width, uint32_t Height);
    void OnDestroyWindowSizeDependentResources();

    void setCameraGBuffer(ILightInput::CameraGBuffer* pCamSRVs);
    void setLightGBuffer(ILightInput::LightGBuffer* pLightSRVs);

    IndirectLighting::per_frame* SetPerFrameConstants();

    void Draw(VkCommandBuffer commandBuffer, VkRect2D* renderArea);

    //  final result
    Texture                         output;
    VkImageView                     srv_output = VK_NULL_HANDLE;

protected:

    Device* pDevice = nullptr;
    ResourceViewHeaps* pResourceViewHeaps = nullptr;
    DynamicBufferRing* pDynamicBufferRing = nullptr;
    StaticBufferPool* pStaticBufferPool = nullptr;

    IndirectLighting::per_frame perFrame;
    VkDescriptorBufferInfo descInfo_perFrame, descInfo_sampleOffsets;
    VkSampler sampler_default;

    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;

    //  set 0: general, set 1: RSM, set 2: GeomBuffer
    VkDescriptorSet descriptorSets[ILIGHT_NUM_DESCRIPTOR_SETS];
    VkDescriptorSetLayout descriptorSetLayouts[ILIGHT_NUM_DESCRIPTOR_SETS];

    bool useExternalRenderPass = false;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    std::vector<VkClearValue> clearValues;

    void createRenderPass();
    void createDescriptors(DefineList* pAttributeDefines);
    void createPipeline(const DefineList* defines);

    void generateSamplingOffsets();
};

