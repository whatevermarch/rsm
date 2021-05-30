#include "IndirectLighting.h"

#include <random>

#define VERTEX_SHADER_FILENAME "Bypass-vert.glsl"

#define FRAGMENT_SHADER_FILENAME "IndirectLighting-frag.glsl"

#define NUM_RSM_SAMPLES 100

bool IndirectLighting::getAttachmentDesc(std::vector<VkAttachmentDescription>& attachments)
{
    attachments.resize(1);
    int cnt = 0;

    //  output
    ::AttachClearBeforeUse(
        VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_SAMPLE_COUNT_1_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        &attachments[cnt++]
    );

    assert(cnt == 1);
    return false; // because it doesn't consist of depth att.
}

void IndirectLighting::OnCreate(
    Device* pDevice,
    UploadHeap* pUploadHeap,
    ResourceViewHeaps* pHeaps,
    DynamicBufferRing* pDynamicBufferRing,
    StaticBufferPool* pStaticBufferPool,
    VkRenderPass renderPass)
{
    this->pDevice = pDevice;
    this->pResourceViewHeaps = pHeaps;
    this->pDynamicBufferRing = pDynamicBufferRing;
    this->pStaticBufferPool = pStaticBufferPool;

    //  define color clearing settings
    {
        this->clearValues.clear();
        VkClearValue cv; // this is a union
        cv.color = { 0.0f, 0.0f, 0.0f, 0.0f };
        this->clearValues.push_back(cv);
    }

    //  create sampler for sampling g-buffer (RSM and GBuffer)
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

    //  create sampler for sampling rotation noise texture
    //  the texture is repeatable in 4 patches per dimension in RSM
    {
        VkSamplerCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        info.magFilter = VK_FILTER_LINEAR;
        info.minFilter = VK_FILTER_LINEAR;
        info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        info.minLod = -1000;
        info.maxLod = 1000;
        info.maxAnisotropy = 1.0f;
        VkResult res = vkCreateSampler(pDevice->GetDevice(), &info, NULL, &this->sampler_kernelRotation);
        assert(res == VK_SUCCESS);
    }

    //  generate sample offsets used for sampling around a particular pixel
    this->generateSamplingOffsets();

    //  generate noise texture to determine each pixel's random rotation
    this->generateSamplingKernelRotation(*pUploadHeap);

    //  create (or derive) render pass
    if (renderPass != VK_NULL_HANDLE)
    {
        this->renderPass = renderPass;
        this->useExternalRenderPass = true;
    }
    else
    {
        this->createRenderPass();
        this->useExternalRenderPass = false;
    }

    //  create descriptor sets and pipeline object for this pass
    DefineList defines;
    this->createDescriptors(&defines);
    this->createPipeline(&defines);
}

void IndirectLighting::OnDestroy()
{
    //  destroy pipeline
    vkDestroyPipeline(this->pDevice->GetDevice(), this->pipeline, nullptr);
    vkDestroyPipelineLayout(this->pDevice->GetDevice(), this->pipelineLayout, nullptr);

    //  destroy descriptor sets
    for (int i = 0; i < ILIGHT_NUM_DESCRIPTOR_SETS; i++)
    {
        vkDestroyDescriptorSetLayout(this->pDevice->GetDevice(), this->descriptorSetLayouts[i], NULL);
        this->pResourceViewHeaps->FreeDescriptor(this->descriptorSets[i]);
    }

    //  destroy render pass
    if (!this->useExternalRenderPass)
    {
        vkDestroyRenderPass(this->pDevice->GetDevice(), this->renderPass, nullptr);
    }
    this->renderPass = VK_NULL_HANDLE;

    //  destroy default sampler
    vkDestroySampler(this->pDevice->GetDevice(), this->sampler_default, nullptr);

    //  destroy kernel rotation sampler
    vkDestroySampler(this->pDevice->GetDevice(), this->sampler_kernelRotation, nullptr);

    //  destroy kernel rotation noise
    vkDestroyImageView(this->pDevice->GetDevice(), this->srv_kernelRotation, nullptr);
    this->srv_output = VK_NULL_HANDLE;
    this->kernelRotation.OnDestroy();

    this->pDevice = nullptr;
    this->pResourceViewHeaps = nullptr;
    this->pDynamicBufferRing = nullptr;
    this->pStaticBufferPool = nullptr;
}

void IndirectLighting::OnCreateWindowSizeDependentResources(uint32_t Width, uint32_t Height)
{
    //  initialize render target 
    {
        this->output.InitRenderTarget(
            this->pDevice,
            Width, Height,
            VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_SAMPLE_COUNT_1_BIT,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT,
            false,
            "I-Light Output"
        );
        this->output.CreateSRV(&this->srv_output);
    }

    //  create frame buffer
    {
        std::vector<VkImageView> attachments = {
            this->srv_output
        };

        this->framebuffer = CreateFrameBuffer(
            this->pDevice->GetDevice(),
            this->renderPass,
            &attachments,
            Width, Height
        );
    }
}

void IndirectLighting::OnDestroyWindowSizeDependentResources()
{
    //  destroy frame buffer
    vkDestroyFramebuffer(this->pDevice->GetDevice(), this->framebuffer, nullptr);
    this->framebuffer = VK_NULL_HANDLE;

    //  destroy texture and its image view
    vkDestroyImageView(this->pDevice->GetDevice(), this->srv_output, nullptr);
    this->srv_output = VK_NULL_HANDLE;
    this->output.OnDestroy();
}

void IndirectLighting::setCameraGBuffer(ILightInput::CameraGBuffer* pCamSRVs)
{
    //  define input image view descriptions
    uint32_t numImages = ILightInput::CameraGBuffer::numImageViews;
    std::vector<VkDescriptorImageInfo> desc_image(numImages);
    desc_image[0].sampler = this->sampler_default;
    desc_image[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    for (int i = 1; i < numImages; i++)
        desc_image[i] = desc_image[0];

    desc_image[0].imageView = pCamSRVs->worldCoord;
    desc_image[1].imageView = pCamSRVs->normal;

    //  update decriptor
    std::vector<VkWriteDescriptorSet> write(numImages);
    for (unsigned int att = 0; att < write.size(); att++)
    {
        write[att] = {};
        write[att].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write[att].pNext = NULL;
        write[att].dstSet = this->descriptorSets[2]; // set 2: GeomBuffer
        write[att].descriptorCount = 1;
        write[att].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write[att].pImageInfo = &desc_image[att];
        write[att].dstBinding = (uint32_t)att;
        write[att].dstArrayElement = 0;
    }

    vkUpdateDescriptorSets(this->pDevice->GetDevice(), write.size(), write.data(), 0, NULL);
}

void IndirectLighting::setLightGBuffer(ILightInput::LightGBuffer* pLightSRVs)
{
    //  define input image view descriptions
    uint32_t numImages = ILightInput::LightGBuffer::numImageViews;
    std::vector<VkDescriptorImageInfo> desc_image(numImages);
    desc_image[0].sampler = this->sampler_default;
    desc_image[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    for (int i = 1; i < numImages; i++)
        desc_image[i] = desc_image[0];

    desc_image[0].imageView = pLightSRVs->worldCoord;
    desc_image[1].imageView = pLightSRVs->normal;
    desc_image[2].imageView = pLightSRVs->flux;

    //  update decriptor
    std::vector<VkWriteDescriptorSet> write(numImages);
    for (unsigned int att = 0; att < write.size(); att++)
    {
        write[att] = {};
        write[att].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write[att].pNext = NULL;
        write[att].dstSet = this->descriptorSets[1]; // set 1: RSM
        write[att].descriptorCount = 1;
        write[att].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write[att].pImageInfo = &desc_image[att];
        write[att].dstBinding = (uint32_t)att;
        write[att].dstArrayElement = 0;
    }

    vkUpdateDescriptorSets(this->pDevice->GetDevice(), write.size(), write.data(), 0, NULL);
}

IndirectLighting::per_frame* IndirectLighting::SetPerFrameConstants()
{
    //  allocate constant buffer from dynamic buffer ring
    //  and get the pointer to that buffer
    per_frame* pPerFrame;
    this->pDynamicBufferRing->AllocConstantBuffer(sizeof(per_frame), (void**)&pPerFrame, &this->descInfo_perFrame);

    return pPerFrame;
}

void IndirectLighting::Draw(VkCommandBuffer commandBuffer, VkRect2D* renderArea)
{
    //  begin render pass
    {
        VkRenderPassBeginInfo rp_begin;
        rp_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp_begin.pNext = NULL;
        rp_begin.renderPass = this->renderPass;
        rp_begin.framebuffer = this->framebuffer;
        rp_begin.renderArea.offset.x = 0;
        rp_begin.renderArea.offset.y = 0;
        rp_begin.renderArea.extent.width = this->output.GetWidth();
        rp_begin.renderArea.extent.height = this->output.GetHeight();
        rp_begin.pClearValues = this->clearValues.data();
        rp_begin.clearValueCount = (uint32_t)this->clearValues.size();
        vkCmdBeginRenderPass(commandBuffer, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);
    }

    SetPerfMarkerBegin(commandBuffer, "I-Light");

    //  set viewport to be rendered
    //  NOTE : remember that we use just a quarter of the original resolution
    SetViewportAndScissor(commandBuffer,
        0, 0,
        this->output.GetWidth(), this->output.GetHeight());

    //  bind descriptor sets
    uint32_t numUniformOffsets = 1;
    uint32_t uniformOffset = this->descInfo_perFrame.offset;
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, this->pipelineLayout, 0, ILIGHT_NUM_DESCRIPTOR_SETS, this->descriptorSets, numUniformOffsets, &uniformOffset);

    //  bind pipeline
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, this->pipeline);

    //  draw
    //  ref : https://www.saschawillems.de/blog/2016/08/13/vulkan-tutorial-on-rendering-a-fullscreen-quad-without-buffers/
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);

    SetPerfMarkerEnd(commandBuffer);

    //  end render pass
    vkCmdEndRenderPass(commandBuffer);
}

void IndirectLighting::createRenderPass()
{
    //  get attachment(s) description for this pass
    std::vector<VkAttachmentDescription> attachments;
    IndirectLighting::getAttachmentDesc(attachments);

    //  setup attachment refs
    //  NOTE : ref no MUST match the index specified upon framebuffer creation
    VkAttachmentReference color_reference = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };

    //  define (single) subpass
    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.flags = 0;
    subpass.inputAttachmentCount = 0;
    subpass.pInputAttachments = NULL;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_reference;
    subpass.pResolveAttachments = NULL;
    subpass.pDepthStencilAttachment = NULL;
    subpass.preserveAttachmentCount = 0;
    subpass.pPreserveAttachments = NULL;

    //  define dependencies between subpasses
    VkSubpassDependency dep = {};
    //  from g-buffer+RSM to i-light
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dep.dependencyFlags = 0; // VK_DEPENDENCY_BY_REGION_BIT; // should try if already modify g-buf and rsm render pass dep.
    
    //  create render pass object
    VkRenderPassCreateInfo rp_info = {};
    rp_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp_info.pNext = NULL;
    rp_info.attachmentCount = (uint32_t)attachments.size();
    rp_info.pAttachments = attachments.data();
    rp_info.subpassCount = 1;
    rp_info.pSubpasses = &subpass;
    rp_info.dependencyCount = 1;
    rp_info.pDependencies = &dep;

    VkResult res = vkCreateRenderPass(this->pDevice->GetDevice(), &rp_info, NULL, &this->renderPass);
    assert(res == VK_SUCCESS);
    SetResourceName(this->pDevice->GetDevice(), VK_OBJECT_TYPE_RENDER_PASS, (uint64_t)this->renderPass, "I-Light Renderpass");
}

void IndirectLighting::createDescriptors(DefineList* pAttributeDefines)
{
    std::vector<VkDescriptorSetLayoutBinding> layout_bindings;

    //  set 0 (general)
    {
        const int numBindings = 3;

        //  define bindings
        layout_bindings.resize(numBindings);
        layout_bindings[0].binding = 0;
        layout_bindings[0].descriptorCount = 1;
        layout_bindings[0].pImmutableSamplers = NULL;
        layout_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        layout_bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        (*pAttributeDefines)["ID_PER_FRAME"] = std::to_string(layout_bindings[0].binding);

        layout_bindings[1].binding = 1;
        layout_bindings[1].descriptorCount = 1;
        layout_bindings[1].pImmutableSamplers = NULL;
        layout_bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        layout_bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        (*pAttributeDefines)["ID_sampleOffsets"] = std::to_string(layout_bindings[1].binding);

        layout_bindings[2].binding = 2;
        layout_bindings[2].descriptorCount = 1;
        layout_bindings[2].pImmutableSamplers = NULL;
        layout_bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        layout_bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        (*pAttributeDefines)["ID_kernelRotations"] = std::to_string(layout_bindings[2].binding);

        //  create desc. set
        this->pResourceViewHeaps->CreateDescriptorSetLayoutAndAllocDescriptorSet(
            &layout_bindings,
            &this->descriptorSetLayouts[0],
            &this->descriptorSets[0]);

        //  update perFrame
        this->pDynamicBufferRing->SetDescriptorSet(0, sizeof(per_frame), this->descriptorSets[0]);
        
        //  update sampleOffsets
        VkWriteDescriptorSet write;
        write = {};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.pNext = NULL;
        write.dstSet = this->descriptorSets[0];
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo = &this->descInfo_sampleOffsets;
        write.dstArrayElement = 0;
        write.dstBinding = layout_bindings[1].binding;

        vkUpdateDescriptorSets(this->pDevice->GetDevice(), 1, &write, 0, NULL);

        //  update kernel rotations
        SetDescriptorSet(this->pDevice->GetDevice(), 2, this->srv_kernelRotation, 
            &this->sampler_kernelRotation, this->descriptorSets[0]);
    }

    //  set 1 (RSM)
    {
        uint32_t numRSMViews = ILightInput::LightGBuffer::numImageViews;
        this->pResourceViewHeaps->AllocDescriptor(
            numRSMViews,
            nullptr,
            &this->descriptorSetLayouts[1],
            &this->descriptorSets[1]);
    }

    //  set 2 (GeomBuffer)
    {
        uint32_t numGeomBufferViews = ILightInput::CameraGBuffer::numImageViews;
        this->pResourceViewHeaps->AllocDescriptor(
            numGeomBufferViews,
            nullptr,
            &this->descriptorSetLayouts[2],
            &this->descriptorSets[2]);
    }
}

void IndirectLighting::createPipeline(const DefineList* defines)
{
    //  create the pipeline layout
    {
        VkPipelineLayoutCreateInfo pPipelineLayoutCreateInfo = {};
        pPipelineLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pPipelineLayoutCreateInfo.pNext = NULL;
        pPipelineLayoutCreateInfo.pushConstantRangeCount = 0;
        pPipelineLayoutCreateInfo.pPushConstantRanges = NULL;
        pPipelineLayoutCreateInfo.setLayoutCount = ILIGHT_NUM_DESCRIPTOR_SETS;
        pPipelineLayoutCreateInfo.pSetLayouts = this->descriptorSetLayouts;

        VkResult res = vkCreatePipelineLayout(this->pDevice->GetDevice(), &pPipelineLayoutCreateInfo, NULL, &this->pipelineLayout);
        assert(res == VK_SUCCESS);
        SetResourceName(this->pDevice->GetDevice(), VK_OBJECT_TYPE_PIPELINE_LAYOUT, (uint64_t)this->pipelineLayout, "I-Light PipLayout");
    }

    //  define pipeline

    //  compile and create shaders
    VkPipelineShaderStageCreateInfo vertexShader = {}, fragmentShader = {};
    {
        VkResult res_compile_shader;
        res_compile_shader = VKCompileFromFile(this->pDevice->GetDevice(), VK_SHADER_STAGE_VERTEX_BIT, VERTEX_SHADER_FILENAME, "main", "", defines, &vertexShader);
        assert(res_compile_shader == VK_SUCCESS);
        res_compile_shader = VKCompileFromFile(this->pDevice->GetDevice(), VK_SHADER_STAGE_FRAGMENT_BIT, FRAGMENT_SHADER_FILENAME, "main", "", defines, &fragmentShader);
        assert(res_compile_shader == VK_SUCCESS);
    }
    std::vector<VkPipelineShaderStageCreateInfo> shaderStages = { vertexShader, fragmentShader };

    // vertex input state

    VkPipelineVertexInputStateCreateInfo vi = {};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.pNext = NULL;
    vi.flags = 0;
    vi.vertexBindingDescriptionCount = 0;
    vi.pVertexBindingDescriptions = NULL;
    vi.vertexAttributeDescriptionCount = 0;
    vi.pVertexAttributeDescriptions = NULL;

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
    rs.cullMode = VK_CULL_MODE_BACK_BIT; // due to EXT_MAINTAINANCE1
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.depthClampEnable = VK_FALSE;
    rs.rasterizerDiscardEnable = VK_FALSE;
    rs.depthBiasEnable = VK_FALSE;
    rs.depthBiasConstantFactor = 0;
    rs.depthBiasClamp = 0;
    rs.depthBiasSlopeFactor = 0;
    rs.lineWidth = 1.0f;

    // Color blend state

    VkPipelineColorBlendAttachmentState att_state = {};
    att_state.colorWriteMask = 0xf;
    att_state.blendEnable = VK_FALSE;
    att_state.alphaBlendOp = VK_BLEND_OP_ADD;
    att_state.colorBlendOp = VK_BLEND_OP_ADD;
    att_state.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    att_state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    att_state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    att_state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;

    VkPipelineColorBlendStateCreateInfo cb;
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.flags = 0;
    cb.pNext = NULL;
    cb.attachmentCount = 1;
    cb.pAttachments = &att_state;
    cb.logicOpEnable = VK_FALSE;
    cb.logicOp = VK_LOGIC_OP_NO_OP;
    cb.blendConstants[0] = 1.0f;
    cb.blendConstants[1] = 1.0f;
    cb.blendConstants[2] = 1.0f;
    cb.blendConstants[3] = 1.0f;

    // dynamic state

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
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_FALSE;
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

    //  create pipeline 
    VkGraphicsPipelineCreateInfo pipeline = {};
    pipeline.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline.pNext = NULL;
    pipeline.layout = this->pipelineLayout;
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
    pipeline.renderPass = this->renderPass;
    pipeline.subpass = 0;

    VkResult res = vkCreateGraphicsPipelines(this->pDevice->GetDevice(), this->pDevice->GetPipelineCache(), 1, &pipeline, NULL, &this->pipeline);
    assert(res == VK_SUCCESS);
    SetResourceName(this->pDevice->GetDevice(), VK_OBJECT_TYPE_PIPELINE, (uint64_t)this->pipeline, "I-Light Pipeline");
}

void IndirectLighting::generateSamplingOffsets()
{
    //  initialize randomizer using normal dist. with the following mean, S.D.
    //  NOTE : S.D. is approximated by chebyshev's inequality
    std::default_random_engine generator;
    std::normal_distribution<float> distribution(0.0f, 0.36f);

    //  define random function
    auto sample_normal = [&distribution, &generator]() {
        float x;
        do {
            x = distribution(generator);
        } while (x < -1.f || x > 1.f);
        return x;
    };

    //  allocate memory in static buffer pool
    std::pair<float, float>* pOffset = nullptr;
    bool allocRes = this->pStaticBufferPool->AllocBuffer(
        NUM_RSM_SAMPLES, sizeof(float) * 2,
        (void**)&pOffset,
        &this->descInfo_sampleOffsets
    );
    assert(allocRes);

    //  generate each sample an offset (x,y)
    for (int i = 0; i < NUM_RSM_SAMPLES; i++)
    {
        pOffset[i].first = sample_normal();
        pOffset[i].second = sample_normal();
    }
}

void IndirectLighting::generateSamplingKernelRotation(UploadHeap& uploadHeap)
{
    //  initialize randomizer using uniform dist. w/ elem in [0,1]
    std::default_random_engine generator;
    std::uniform_real_distribution<float> distribution;
    
    //  generate noise texture in host
    const int noiseDim = 64;
    std::vector<float> rotations(noiseDim * noiseDim);
    for (float& rotation : rotations)
        rotation = distribution(generator);

    //  init noise texture object
    {
        IMG_INFO texInfo;
        texInfo.width = noiseDim;
        texInfo.height = noiseDim;
        texInfo.depth = 1;
        texInfo.mipMapCount = 1;
        texInfo.arraySize = 1;
        texInfo.format = DXGI_FORMAT_R32_FLOAT;
        texInfo.bitCount = 32;

        this->kernelRotation.InitFromData(this->pDevice, uploadHeap, texInfo, rotations.data(), "Noise Texture");
    }

    //  upload noise data to GPU
    uploadHeap.FlushAndFinish();

    //  create image view
    this->kernelRotation.CreateSRV(&this->srv_kernelRotation);
}
