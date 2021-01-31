#pragma once


class RSM
{
public:

    //  get attachment infos. of this pass
    //  return true if it consists of depth attachment (at the end of the list)
    static bool getAttachmentDesc(
        std::vector<VkAttachmentDescription>& attachments);

    //using per_frame = GltfDepthPass::per_frame;
    struct per_frame
    {
        Light light;
    };

    //using per_object = GltfDepthPass::per_object;
    struct per_object
    {
        XMMATRIX mWorld;
        PBRMaterialParametersConstantBuffer m_pbrParams;
    };

    struct BatchList
    {
        PBRPrimitives* m_pPrimitive;
        VkDescriptorBufferInfo m_perObjectDesc;
        VkDescriptorBufferInfo* m_pPerSkeleton;
    };

    void OnCreate(
        Device* pDevice,
        UploadHeap* pUploadHeap,
        ResourceViewHeaps* pHeaps,
        DynamicBufferRing* pDynamicBufferRing,
        StaticBufferPool* pStaticBufferPool,
        uint32_t mapWidth,
        uint32_t mapHeight);

    void OnDestroy();

    bool resourcesBound() { return this->m_pGLTFTexturesAndBuffers != nullptr; }
    void bindResources(GLTFTexturesAndBuffers* pGLTFTexturesAndBuffers, AsyncPool* pAsyncPool = NULL);
    void freeResources();

    RSM::per_frame* SetPerFrameConstants();

    void Draw(VkCommandBuffer commandBuffer, Light* lights[4]);
    void BuildBatchList(std::vector<BatchList>* pBatchList);
    void DrawBatchList(VkCommandBuffer commandBuffer, std::vector<BatchList>* pBatchList);

    //  shader resources
    //  NOTE : these g-buffer are divided into 4 section (half vert/hori), 
    //          supporting maximum 4 light src.
    //  depth
    Texture                         m_Depth;
    VkImageView                     m_DepthDSV = VK_NULL_HANDLE;
    VkImageView                     m_DepthSRV = VK_NULL_HANDLE;

    //  world coord
    Texture                         m_WorldCoord;
    VkImageView                     m_WorldCoordSRV = VK_NULL_HANDLE;

    //  normal
    Texture                         m_Normal;
    VkImageView                     m_NormalSRV = VK_NULL_HANDLE;

    //  flux
    Texture                         m_Flux;
    VkImageView                     m_FluxSRV = VK_NULL_HANDLE;

protected:

    uint32_t m_Width = 0, m_Height = 0;

    Device* m_pDevice = nullptr;
    ResourceViewHeaps* m_pResourceViewHeaps = nullptr;
    DynamicBufferRing* m_pDynamicBufferRing = nullptr;
    StaticBufferPool* m_pStaticBufferPool = nullptr;

    GLTFTexturesAndBuffers* m_pGLTFTexturesAndBuffers = nullptr;

    std::vector<PBRMesh> m_meshes;
    std::vector<PBRMaterial> m_materialsData;
    PBRMaterial m_defaultMaterial;
    VkSampler m_defaultSampler = VK_NULL_HANDLE;

    RSM::per_frame m_cbPerFrame;
    VkDescriptorBufferInfo m_perFrameDesc;

    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    VkFramebuffer m_framebuffer = VK_NULL_HANDLE;
    std::vector<VkClearValue> m_clearValues;

    void CreateRenderPass();
    void CreateDescriptorTableForMaterialTextures(PBRMaterial* tfmat, std::map<std::string, VkImageView>& texturesBase);
    void CreateDescriptors(DefineList* pAttributeDefines, PBRPrimitives* pPrimitive);
    void CreatePipeline(std::vector<VkVertexInputAttributeDescription> layout, const DefineList& defines, PBRPrimitives* pPrimitive);
};
