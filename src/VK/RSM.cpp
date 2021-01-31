#include "RSM.h"

#include "GLTF/GltfHelpers.h"
#include "Base/ExtDebugUtils.h"

#define VERTEX_SHADER_FILENAME "RSM-vert.glsl"
#define FRAGMENT_SHADER_FILENAME "RSM-frag.glsl"

#define NUM_COLOR_RTS 3

bool RSM::getAttachmentDesc(std::vector<VkAttachmentDescription>& attachments)
{
    attachments.resize(NUM_COLOR_RTS + 1);
    int cnt = 0;

    // world coord
    ::AttachClearBeforeUse(
        VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_SAMPLE_COUNT_1_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        &attachments[cnt++]
    );

    // normal
    ::AttachClearBeforeUse(
        VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_SAMPLE_COUNT_1_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        &attachments[cnt++]
    );

    // flux
    ::AttachClearBeforeUse(
        VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_SAMPLE_COUNT_1_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        &attachments[cnt++]
    );

    // depth
    ::AttachClearBeforeUse(
        VK_FORMAT_D32_SFLOAT,
        VK_SAMPLE_COUNT_1_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        &attachments[cnt++]
    );

    assert(cnt == NUM_COLOR_RTS + 1);
    return true; // because it consists of depth att.
}

void RSM::OnCreate(
    Device* pDevice, 
    UploadHeap* pUploadHeap, 
    ResourceViewHeaps* pHeaps, 
    DynamicBufferRing* pDynamicBufferRing, 
    StaticBufferPool* pStaticBufferPool, 
    uint32_t mapWidth, uint32_t mapHeight)
{
    this->m_pDevice = pDevice;
    this->m_pResourceViewHeaps = pHeaps;
    this->m_pDynamicBufferRing = pDynamicBufferRing;
    this->m_pStaticBufferPool = pStaticBufferPool;

    this->m_Width = mapWidth * 2;
    this->m_Height = mapHeight * 2;

    //  define color clearing settings
    {
        this->m_clearValues.clear();
        VkClearValue clearValue; // this is a union
        clearValue.color = { 0.0f, 0.0f, 0.0f, 0.0f };
        for(int rt = 0; rt < NUM_COLOR_RTS; rt++)
            this->m_clearValues.push_back(clearValue);
        clearValue.depthStencil = { 1.0f, 0 };
        this->m_clearValues.push_back(clearValue);
    }

    // Create static sampler in case there is transparency
    //
    {
        VkSamplerCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        info.magFilter = VK_FILTER_LINEAR;
        info.minFilter = VK_FILTER_LINEAR;
        info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        info.minLod = 0;
        info.maxLod = 10000;
        info.maxAnisotropy = 1.0f;
        VkResult res = vkCreateSampler(pDevice->GetDevice(), &info, NULL, &m_defaultSampler);
        assert(res == VK_SUCCESS);
    }

    //  create render pass for RSM pipeline
    this->CreateRenderPass();

    //  init g-buffer ( = RSM itself )
    {
        this->m_WorldCoord.InitRenderTarget(
            this->m_pDevice,
            this->m_Width, this->m_Height,
            VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_SAMPLE_COUNT_1_BIT,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
            false,
            "RSM-WorldCoord"
        );
        this->m_WorldCoord.CreateSRV(&this->m_WorldCoordSRV);

        this->m_Normal.InitRenderTarget(
            this->m_pDevice,
            this->m_Width, this->m_Height,
            VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_SAMPLE_COUNT_1_BIT,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
            false,
            "RSM-Normal"
        );
        this->m_Normal.CreateSRV(&this->m_NormalSRV);

        this->m_Flux.InitRenderTarget(
            this->m_pDevice,
            this->m_Width, this->m_Height,
            VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_SAMPLE_COUNT_1_BIT,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
            false,
            "RSM-Flux"
        );
        this->m_Flux.CreateSRV(&this->m_FluxSRV);

        this->m_Depth.InitDepthStencil(
            this->m_pDevice,
            this->m_Width, this->m_Height,
            VK_FORMAT_D32_SFLOAT,
            VK_SAMPLE_COUNT_1_BIT,
            "RSM-Depth"
        );
        this->m_Depth.CreateDSV(&this->m_DepthDSV);
        this->m_Depth.CreateSRV(&this->m_DepthSRV);
    }

    //  create framebuffer
    std::vector<VkImageView> attachmentViews = { 
        this->m_WorldCoordSRV,
        this->m_NormalSRV,
        this->m_FluxSRV,
        this->m_DepthDSV 
    };

    this->m_framebuffer = CreateFrameBuffer(
        this->m_pDevice->GetDevice(),
        this->m_renderPass,
        &attachmentViews,
        this->m_Width, this->m_Height
    );
}

void RSM::OnDestroy()
{
    vkDestroyFramebuffer(this->m_pDevice->GetDevice(), this->m_framebuffer, nullptr);

    //  destroy G-Buffer(RSM) and its image view
    {
        vkDestroyImageView(this->m_pDevice->GetDevice(), this->m_WorldCoordSRV, nullptr);
        this->m_WorldCoord.OnDestroy();

        vkDestroyImageView(this->m_pDevice->GetDevice(), this->m_NormalSRV, nullptr);
        this->m_Normal.OnDestroy();

        vkDestroyImageView(this->m_pDevice->GetDevice(), this->m_FluxSRV, nullptr);
        this->m_Flux.OnDestroy();

        vkDestroyImageView(this->m_pDevice->GetDevice(), this->m_DepthDSV, nullptr);
        vkDestroyImageView(this->m_pDevice->GetDevice(), this->m_DepthSRV, nullptr);
        this->m_Depth.OnDestroy();
    }

    vkDestroyRenderPass(m_pDevice->GetDevice(), m_renderPass, nullptr);
    vkDestroySampler(m_pDevice->GetDevice(), m_defaultSampler, nullptr);

    this->m_Width = 0;
    this->m_Height = 0;

    this->m_pDevice = nullptr;
    this->m_pResourceViewHeaps = nullptr;
    this->m_pDynamicBufferRing = nullptr;
    this->m_pStaticBufferPool = nullptr;
}

void RSM::bindResources(GLTFTexturesAndBuffers* pGLTFTexturesAndBuffers, AsyncPool* pAsyncPool)
{
    this->m_pGLTFTexturesAndBuffers = pGLTFTexturesAndBuffers;

    // Create default material, this material will be used if none is assigned
    //
    {
        SetDefaultMaterialParamters(&m_defaultMaterial.m_pbrMaterialParameters);
    }

    // Load PBR 2.0 Materials
    //  
    const json& j3 = pGLTFTexturesAndBuffers->m_pGLTFCommon->j3;
    if (j3.find("materials") != j3.end())
    {
        const json& materials = j3["materials"];
        m_materialsData.resize(materials.size());

        for (uint32_t i = 0; i < materials.size(); i++)
        {
            PBRMaterial* tfmat = &m_materialsData[i];

            // Get PBR material parameters and texture IDs
            std::map<std::string, int> textureIds;
            ProcessMaterials(materials[i], &tfmat->m_pbrMaterialParameters, textureIds);

            // translate texture IDs into textureViews
            std::map<std::string, VkImageView> texturesBase;
            for (auto const& value : textureIds)
                texturesBase[value.first] = m_pGLTFTexturesAndBuffers->GetTextureViewByID(value.second);

            CreateDescriptorTableForMaterialTextures(tfmat, texturesBase);
        }
    }

    // Load Meshes
    if (j3.find("meshes") != j3.end())
    {
        const json& meshes = j3["meshes"];
        m_meshes.resize(meshes.size());

        for (uint32_t i = 0; i < meshes.size(); i++)
        {
            const json& primitives = meshes[i]["primitives"];

            // Loop through all the primitives (sets of triangles with a same material) and 
            // 1) create an input layout for the geometry
            // 2) then take its material and create a Root descriptor
            // 3) With all the above, create a pipeline
            //
            PBRMesh* tfmesh = &m_meshes[i];
            tfmesh->m_pPrimitives.resize(primitives.size());

            for (uint32_t p = 0; p < primitives.size(); p++)
            {
                const json& primitive = primitives[p];
                PBRPrimitives* pPrimitive = &tfmesh->m_pPrimitives[p];

                ExecAsyncIfThereIsAPool(pAsyncPool, [this, i, &primitive, pPrimitive]()
                {
                    // Sets primitive's material, or set a default material if none was specified in the GLTF
                    //
                    auto mat = primitive.find("material");
                    pPrimitive->m_pMaterial = (mat != primitive.end()) ? &m_materialsData[mat.value()] : &m_defaultMaterial;

                    // holds all the #defines from materials, geometry and texture IDs, the VS & PS shaders need this to get the bindings and code paths
                    //
                    DefineList defines = pPrimitive->m_pMaterial->m_pbrMaterialParameters.m_defines;

                    // make a list of all the attribute names our pass requires, in the case of PBR we need them all
                    //
                    std::vector<std::string> requiredAttributes;
                    for (auto const& it : primitive["attributes"].items())
                    {
                        const std::string semanticName = it.key();
                        if (
                            (semanticName == "POSITION") ||
                            (semanticName.substr(0, 5) == "COLOR") ||
                            (semanticName == "NORMAL") ||
                            (semanticName == "TANGENT") ||
                            //(semanticName.substr(0, 7) == "WEIGHTS") || // for skinning
                            //(semanticName.substr(0, 6) == "JOINTS") || // for skinning
                            (DoesMaterialUseSemantic(defines, semanticName) == true) // if there is transparency this will make sure we use the texture coordinates of that texture
                            )
                        {
                            requiredAttributes.push_back(semanticName);
                        }
                    }

                    //  append defines from render targets
                    //defines += rtDefines;

                    // create an input layout from the required attributes
                    // shader's can tell the slots from the #defines
                    //
                    std::vector<VkVertexInputAttributeDescription> inputLayout;
                    m_pGLTFTexturesAndBuffers->CreateGeometry(primitive, requiredAttributes, inputLayout, defines, &pPrimitive->m_geometry);

                    // Create descriptors and pipelines
                    //
                    CreateDescriptors(&defines, pPrimitive);
                    CreatePipeline(inputLayout, defines, pPrimitive);
                });
            }
        }
    }
}

void RSM::freeResources()
{
    if (this->m_pGLTFTexturesAndBuffers == nullptr)
        return;

    for (uint32_t m = 0; m < m_meshes.size(); m++)
    {
        PBRMesh* pMesh = &m_meshes[m];
        for (uint32_t p = 0; p < pMesh->m_pPrimitives.size(); p++)
        {
            PBRPrimitives* pPrimitive = &pMesh->m_pPrimitives[p];
            vkDestroyPipeline(m_pDevice->GetDevice(), pPrimitive->m_pipeline, nullptr);
            pPrimitive->m_pipeline = VK_NULL_HANDLE;
            vkDestroyPipelineLayout(m_pDevice->GetDevice(), pPrimitive->m_pipelineLayout, nullptr);
            vkDestroyDescriptorSetLayout(m_pDevice->GetDevice(), pPrimitive->m_uniformsDescriptorSetLayout, NULL);
            m_pResourceViewHeaps->FreeDescriptor(pPrimitive->m_uniformsDescriptorSet);
        }
    }

    for (int i = 0; i < m_materialsData.size(); i++)
    {
        vkDestroyDescriptorSetLayout(m_pDevice->GetDevice(), m_materialsData[i].m_texturesDescriptorSetLayout, NULL);
        m_pResourceViewHeaps->FreeDescriptor(m_materialsData[i].m_texturesDescriptorSet);
    }

    //destroy default material
    vkDestroyDescriptorSetLayout(m_pDevice->GetDevice(), m_defaultMaterial.m_texturesDescriptorSetLayout, NULL);
    m_pResourceViewHeaps->FreeDescriptor(m_defaultMaterial.m_texturesDescriptorSet);

    this->m_pGLTFTexturesAndBuffers = nullptr;
}

RSM::per_frame* RSM::SetPerFrameConstants()
{
    per_frame* cbPerFrame;
    m_pDynamicBufferRing->AllocConstantBuffer(sizeof(per_frame), (void**)&cbPerFrame, &m_perFrameDesc);

    return cbPerFrame;
}

void RSM::Draw(VkCommandBuffer commandBuffer, Light* lights[4])
{
    // prepare batch first
    //
    std::vector<RSM::BatchList> batches;
    this->BuildBatchList(&batches);

    //  begin render pass
    {
        VkRenderPassBeginInfo rp_begin;
        rp_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp_begin.pNext = NULL;
        rp_begin.renderPass = this->m_renderPass;
        rp_begin.framebuffer = this->m_framebuffer;
        rp_begin.renderArea.offset.x = 0;
        rp_begin.renderArea.offset.y = 0;
        rp_begin.renderArea.extent.width = this->m_Width;
        rp_begin.renderArea.extent.height = this->m_Height;
        rp_begin.pClearValues = m_clearValues.data();
        rp_begin.clearValueCount = (uint32_t)m_clearValues.size();
        vkCmdBeginRenderPass(commandBuffer, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);
    }

    //  render to RSM (each quarter)
    {
        uint32_t viewportOffsetsX[4] = { 0, 1, 0, 1 };
        uint32_t viewportOffsetsY[4] = { 0, 0, 1, 1 };
        uint32_t viewportWidth = this->m_Width / 2;
        uint32_t viewportHeight = this->m_Height / 2;

        for (int quarter = 0; quarter < 4; quarter++)
        {
            if (!lights[quarter])
                break;

            // Set per Frame constants
            //
            per_frame* cbPerFrame = this->SetPerFrameConstants();
            cbPerFrame->light = *(lights[quarter]);

            SetViewportAndScissor(commandBuffer,
                viewportOffsetsX[quarter] * viewportWidth,
                viewportOffsetsY[quarter] * viewportHeight,
                viewportWidth, viewportHeight);

            this->DrawBatchList(commandBuffer, &batches);
        }
    }

    //  end render pass
    vkCmdEndRenderPass(commandBuffer);
}

void RSM::BuildBatchList(std::vector<RSM::BatchList>* pBatchList)
{
    // loop through nodes
    //
    std::vector<tfNode>* pNodes = &m_pGLTFTexturesAndBuffers->m_pGLTFCommon->m_nodes;
    Matrix2* pNodesMatrices = m_pGLTFTexturesAndBuffers->m_pGLTFCommon->m_worldSpaceMats.data();

    for (uint32_t i = 0; i < pNodes->size(); i++)
    {
        tfNode* pNode = &pNodes->at(i);
        if ((pNode == NULL) || (pNode->meshIndex < 0))
            continue;

        XMMATRIX mModelViewProj = pNodesMatrices[i].GetCurrent() * m_pGLTFTexturesAndBuffers->m_pGLTFCommon->m_perFrameData.mCameraCurrViewProj;

        PBRMesh* pMesh = &m_meshes[pNode->meshIndex];
        for (int p = 0; p < pMesh->m_pPrimitives.size(); p++)
        {
            PBRPrimitives* pPrimitive = &pMesh->m_pPrimitives[p];

            if (pPrimitive->m_pipeline == VK_NULL_HANDLE)
                continue;

            // do frustrum culling
            //
            tfPrimitives boundingBox = m_pGLTFTexturesAndBuffers->m_pGLTFCommon->m_meshes[pNode->meshIndex].m_pPrimitives[p];
            if (CameraFrustumToBoxCollision(mModelViewProj, boundingBox.m_center, boundingBox.m_radius))
                continue;

            // Set per Object constants
            //
            per_object* cbPerObject;
            VkDescriptorBufferInfo perObjectDesc;
            m_pDynamicBufferRing->AllocConstantBuffer(sizeof(per_object), (void**)&cbPerObject, &perObjectDesc);
            cbPerObject->mWorld = pNodesMatrices[i].GetCurrent();
            cbPerObject->m_pbrParams = pPrimitive->m_pMaterial->m_pbrMaterialParameters.m_params;

            // Build batch
            //
            BatchList t;
            t.m_pPrimitive = pPrimitive;
            t.m_perObjectDesc = perObjectDesc;
            t.m_pPerSkeleton = nullptr; // m_pGLTFTexturesAndBuffers->GetSkinningMatricesBuffer(pNode->skinIndex);

            // append primitive to list 
            //
            pBatchList->push_back(t);
        }
    }
}

void RSM::DrawBatchList(VkCommandBuffer commandBuffer, std::vector<RSM::BatchList>* pBatchList)
{
    SetPerfMarkerBegin(commandBuffer, "RSM");

    for (auto& t : *pBatchList)
    {
        t.m_pPrimitive->DrawPrimitive(
            commandBuffer, 
            m_perFrameDesc,
            t.m_perObjectDesc, 
            t.m_pPerSkeleton
        );
    }

    SetPerfMarkerEnd(commandBuffer);
}

void RSM::CreateRenderPass()
{
    std::vector<VkAttachmentDescription> attachments;
    bool hasDepth = RSM::getAttachmentDesc(attachments);
    uint32_t numColorAtt = hasDepth ? attachments.size() - 1 : attachments.size();

    this->m_renderPass = CreateRenderPassOptimal(
        this->m_pDevice->GetDevice(),
        numColorAtt,
        attachments.data(),
        hasDepth ? &attachments[numColorAtt] : NULL
    );
    SetResourceName(m_pDevice->GetDevice(), VK_OBJECT_TYPE_RENDER_PASS, (uint64_t)m_renderPass, "RSM Renderpass");
}

void RSM::CreateDescriptorTableForMaterialTextures(PBRMaterial* tfmat, std::map<std::string, VkImageView>& texturesBase)
{
    // count the number of textures to init bindings and descriptor
    tfmat->m_textureCount = (int)texturesBase.size();

    // Alloc a descriptor layout and init the descriptor set for the PBR textures 
    // for each entry we create a #define with that texture name that hold the id of the texture. That way the PS knows in what slot is each texture.      

    // allocate descriptor table for the textures
    m_pResourceViewHeaps->AllocDescriptor(tfmat->m_textureCount, NULL, &tfmat->m_texturesDescriptorSetLayout, &tfmat->m_texturesDescriptorSet);

    uint32_t cnt = 0;

    // create SRV for the PBR materials
    //
    for (auto const& it : texturesBase)
    {
        tfmat->m_pbrMaterialParameters.m_defines[std::string("ID_") + it.first] = std::to_string(cnt);
        SetDescriptorSet(m_pDevice->GetDevice(), cnt, it.second, &m_defaultSampler, tfmat->m_texturesDescriptorSet);
        cnt++;
    }
}

void RSM::CreateDescriptors(DefineList* pAttributeDefines, PBRPrimitives* pPrimitive)
{
    std::vector<VkDescriptorSetLayoutBinding> layout_bindings(2);

    // Constant buffer 'per frame'
    layout_bindings[0].binding = 0;
    layout_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    layout_bindings[0].descriptorCount = 1;
    layout_bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    layout_bindings[0].pImmutableSamplers = NULL;
    (*pAttributeDefines)["ID_PER_FRAME"] = std::to_string(layout_bindings[0].binding);

    // Constant buffer 'per object'
    layout_bindings[1].binding = 1;
    layout_bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    layout_bindings[1].descriptorCount = 1;
    layout_bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    layout_bindings[1].pImmutableSamplers = NULL;
    (*pAttributeDefines)["ID_PER_OBJECT"] = std::to_string(layout_bindings[1].binding);

    m_pResourceViewHeaps->CreateDescriptorSetLayoutAndAllocDescriptorSet(&layout_bindings, &pPrimitive->m_uniformsDescriptorSetLayout, &pPrimitive->m_uniformsDescriptorSet);

    // set descriptors entries
    m_pDynamicBufferRing->SetDescriptorSet(0, sizeof(per_frame), pPrimitive->m_uniformsDescriptorSet);
    m_pDynamicBufferRing->SetDescriptorSet(1, sizeof(per_object), pPrimitive->m_uniformsDescriptorSet);

    // Create the pipeline layout
    // this means in shader => set 0: general, set 1: texture
    //
    std::vector<VkDescriptorSetLayout> descriptorSetLayout = { pPrimitive->m_uniformsDescriptorSetLayout };
    if (pPrimitive->m_pMaterial->m_texturesDescriptorSetLayout != VK_NULL_HANDLE)
        descriptorSetLayout.push_back(pPrimitive->m_pMaterial->m_texturesDescriptorSetLayout);

    /////////////////////////////////////////////
    // Create a PSO description

    VkPipelineLayoutCreateInfo pPipelineLayoutCreateInfo = {};
    pPipelineLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pPipelineLayoutCreateInfo.pNext = NULL;
    pPipelineLayoutCreateInfo.pushConstantRangeCount = 0;
    pPipelineLayoutCreateInfo.pPushConstantRanges = NULL;
    pPipelineLayoutCreateInfo.setLayoutCount = (uint32_t)descriptorSetLayout.size();
    pPipelineLayoutCreateInfo.pSetLayouts = descriptorSetLayout.data();

    VkResult res = vkCreatePipelineLayout(m_pDevice->GetDevice(), &pPipelineLayoutCreateInfo, NULL, &pPrimitive->m_pipelineLayout);
    assert(res == VK_SUCCESS);
    SetResourceName(m_pDevice->GetDevice(), VK_OBJECT_TYPE_PIPELINE_LAYOUT, (uint64_t)pPrimitive->m_pipelineLayout, "RSM PipLayout");
}

void RSM::CreatePipeline(std::vector<VkVertexInputAttributeDescription> layout, const DefineList& defines, PBRPrimitives* pPrimitive)
{
    /////////////////////////////////////////////
    // Compile and create shaders

    VkPipelineShaderStageCreateInfo vertexShader, fragmentShader = {};
    {
        VkResult res_compile_shader;
        res_compile_shader = VKCompileFromFile(m_pDevice->GetDevice(), VK_SHADER_STAGE_VERTEX_BIT, VERTEX_SHADER_FILENAME, "main", "", &defines, &vertexShader);
        assert(res_compile_shader == VK_SUCCESS);
        res_compile_shader = VKCompileFromFile(m_pDevice->GetDevice(), VK_SHADER_STAGE_FRAGMENT_BIT, FRAGMENT_SHADER_FILENAME, "main", "", &defines, &fragmentShader);
        assert(res_compile_shader == VK_SUCCESS);
    }
    std::vector<VkPipelineShaderStageCreateInfo> shaderStages = { vertexShader, fragmentShader };

    /////////////////////////////////////////////
    // Create a Pipeline 

    // vertex input state

    std::vector<VkVertexInputBindingDescription> vi_binding(layout.size());
    for (int i = 0; i < layout.size(); i++)
    {
        vi_binding[i].binding = layout[i].binding;
        vi_binding[i].stride = SizeOfFormat(layout[i].format);
        vi_binding[i].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    }

    VkPipelineVertexInputStateCreateInfo vi = {};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.pNext = NULL;
    vi.flags = 0;
    vi.vertexBindingDescriptionCount = (uint32_t)vi_binding.size();
    vi.pVertexBindingDescriptions = vi_binding.data();
    vi.vertexAttributeDescriptionCount = (uint32_t)layout.size();
    vi.pVertexAttributeDescriptions = layout.data();

    // input assembly state

    VkPipelineInputAssemblyStateCreateInfo ia;
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.pNext = NULL;
    ia.flags = 0;
    ia.primitiveRestartEnable = VK_FALSE;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // rasterizer state

    VkPipelineRasterizationStateCreateInfo rs;
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.pNext = NULL;
    rs.flags = 0;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = pPrimitive->m_pMaterial->m_pbrMaterialParameters.m_doubleSided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.depthClampEnable = VK_FALSE;
    rs.rasterizerDiscardEnable = VK_FALSE;
    rs.depthBiasEnable = VK_FALSE;
    rs.depthBiasConstantFactor = 0;
    rs.depthBiasClamp = 0;
    rs.depthBiasSlopeFactor = 0;
    rs.lineWidth = 1.0f;

    // Color blend state

    std::vector<VkPipelineColorBlendAttachmentState> att_states;
    VkPipelineColorBlendAttachmentState att_state = {};
    att_state.colorWriteMask = 0xf;
    att_state.blendEnable = VK_FALSE; // set to TRUE when transparency
    att_state.alphaBlendOp = VK_BLEND_OP_ADD;
    att_state.colorBlendOp = VK_BLEND_OP_ADD;
    att_state.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    att_state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    att_state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    att_state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    for (int rt = 0; rt < NUM_COLOR_RTS; rt++)
        att_states.push_back(att_state);

    VkPipelineColorBlendStateCreateInfo cb;
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.flags = 0;
    cb.pNext = NULL;
    cb.attachmentCount = static_cast<uint32_t>(att_states.size());;
    cb.pAttachments = att_states.data();
    cb.logicOpEnable = VK_FALSE;
    cb.logicOp = VK_LOGIC_OP_NO_OP;
    cb.blendConstants[0] = 1.0f;
    cb.blendConstants[1] = 1.0f;
    cb.blendConstants[2] = 1.0f;
    cb.blendConstants[3] = 1.0f;

    std::vector<VkDynamicState> dynamicStateEnables = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };
    VkPipelineDynamicStateCreateInfo dynamicState = {};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.pNext = NULL;
    dynamicState.pDynamicStates = dynamicStateEnables.data();
    dynamicState.dynamicStateCount = (uint32_t)dynamicStateEnables.size();

    // view port state

    VkPipelineViewportStateCreateInfo vp = {};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.pNext = NULL;
    vp.flags = 0;
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    vp.pScissors = NULL;
    vp.pViewports = NULL;

    // depth stencil state

    VkPipelineDepthStencilStateCreateInfo ds;
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.pNext = NULL;
    ds.flags = 0;
    ds.depthTestEnable = true;
    ds.depthWriteEnable = true;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    ds.back.failOp = VK_STENCIL_OP_KEEP;
    ds.back.passOp = VK_STENCIL_OP_KEEP;
    ds.back.compareOp = VK_COMPARE_OP_ALWAYS;
    ds.back.compareMask = 0;
    ds.back.reference = 0;
    ds.back.depthFailOp = VK_STENCIL_OP_KEEP;
    ds.back.writeMask = 0;
    ds.depthBoundsTestEnable = VK_FALSE;
    ds.minDepthBounds = 0;
    ds.maxDepthBounds = 0;
    ds.stencilTestEnable = VK_FALSE;
    ds.front = ds.back;

    // multi sample state

    VkPipelineMultisampleStateCreateInfo ms;
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.pNext = NULL;
    ms.flags = 0;
    ms.pSampleMask = NULL;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    ms.sampleShadingEnable = VK_FALSE;
    ms.alphaToCoverageEnable = VK_FALSE;
    ms.alphaToOneEnable = VK_FALSE;
    ms.minSampleShading = 0.0;

    // create pipeline 

    VkGraphicsPipelineCreateInfo pipeline = {};
    pipeline.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline.pNext = NULL;
    pipeline.layout = pPrimitive->m_pipelineLayout;
    pipeline.basePipelineHandle = VK_NULL_HANDLE;
    pipeline.basePipelineIndex = 0;
    pipeline.flags = 0;
    pipeline.pVertexInputState = &vi;
    pipeline.pInputAssemblyState = &ia;
    pipeline.pRasterizationState = &rs;
    pipeline.pColorBlendState = &cb;
    pipeline.pTessellationState = NULL;
    pipeline.pMultisampleState = &ms;
    pipeline.pDynamicState = &dynamicState;
    pipeline.pViewportState = &vp;
    pipeline.pDepthStencilState = &ds;
    pipeline.pStages = shaderStages.data();
    pipeline.stageCount = (uint32_t)shaderStages.size();
    pipeline.renderPass = m_renderPass;
    pipeline.subpass = 0;

    VkResult res = vkCreateGraphicsPipelines(m_pDevice->GetDevice(), m_pDevice->GetPipelineCache(), 1, &pipeline, NULL, &pPrimitive->m_pipeline);
    assert(res == VK_SUCCESS);
    SetResourceName(m_pDevice->GetDevice(), VK_OBJECT_TYPE_PIPELINE, (uint64_t)pPrimitive->m_pipeline, "RSM Pipeline");
}
