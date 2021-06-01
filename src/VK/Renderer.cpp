#include "Renderer.h"

#include <algorithm>

// We are queuing (2 backbuffers + 0.5) frames, so we need to triple buffer the command lists
//	ToDo : let's experiment if 2 is sufficient.
static const int backBufferCount = 3;

//  Shadow map size (the texture dimension is shadowmapSize * shadowmapSize)
static const uint32_t shadowmapSize = 1024;

void Renderer::OnCreate(Device* pDevice, SwapChain* pSwapChain)
{
	this->pDevice = pDevice;

	// Create a 'static' pool for vertices and indices 
	const uint32_t staticGeometryMemSize = 128 * 1024 * 1024;
	this->sBufferPool.OnCreate(pDevice, staticGeometryMemSize, true, "StaticGeom");

	// Create a 'dynamic' constant buffer
	const uint32_t constantBuffersMemSize = 20 * 1024 * 1024;
	this->dBufferRing.OnCreate(pDevice, backBufferCount, constantBuffersMemSize, "Uniforms");

	//	create all the heaps for the resources views
	const uint32_t cbvDescriptorCount = 2000;
	const uint32_t srvDescriptorCount = 2000;
	const uint32_t uavDescriptorCount = 10;
	const uint32_t samplerDescriptorCount = 20;
	this->resViewHeaps.OnCreate(pDevice, cbvDescriptorCount, srvDescriptorCount,
		uavDescriptorCount, samplerDescriptorCount);

	//	create command buffer ring for the Direct queue
	uint32_t commandListsPerBackBuffer = 8;
	this->cmdBufferRing.OnCreate(pDevice, backBufferCount, commandListsPerBackBuffer);

	// Quick helper to upload resources, it has it's own commandList and uses suballocation.
	// for 4K textures we'll need 100Megs
	const uint32_t uploadHeapMemSize = 128 * 1024 * 1024;
	this->uploadHeap.OnCreate(pDevice, uploadHeapMemSize); // initialize an upload heap (uses suballocation for faster results)

	// initialize the GPU time stamps module
	// this->gTimeStamps.OnCreate(pDevice, backBufferCount);

	//	setup pass resources
    //
    //  pass 1.1 : reflective shadow map (4x of 1024x1024)
    {
        const uint32_t totalRSMSize = shadowmapSize * 2;

        this->pRSM = new GBuffer();
        this->pRSM->OnCreate(this->pDevice,
            &this->resViewHeaps,
            {
                //  RSM
                { GBUFFER_DEPTH, VK_FORMAT_D32_SFLOAT},
                { GBUFFER_WORLD_COORD, VK_FORMAT_R16G16B16A16_SFLOAT},
                { GBUFFER_NORMAL_BUFFER, VK_FORMAT_R16G16B16A16_SFLOAT},
                { GBUFFER_DIFFUSE, VK_FORMAT_R16G16B16A16_SFLOAT}, // represent flux map
                { GBUFFER_SPECULAR_ROUGHNESS, VK_FORMAT_R16G16B16A16_SFLOAT},
            },
            1
            );
        GBufferFlags fullRSM = GBUFFER_DEPTH |
            GBUFFER_WORLD_COORD | GBUFFER_NORMAL_BUFFER |
            GBUFFER_DIFFUSE | GBUFFER_SPECULAR_ROUGHNESS;
        this->rp_RSM_full.OnCreate(this->pRSM, fullRSM, true, "RSM RenderPass");

        //  init data immediately since they don't depend on window size
        this->pRSM->OnCreateWindowSizeDependentResources(pSwapChain, totalRSMSize, totalRSMSize);
        this->rp_RSM_full.OnCreateWindowSizeDependentResources(totalRSMSize, totalRSMSize);
    }
	//	pass 1.2 : G-buffer
    {
        this->pGBuffer = new GBuffer();
        this->pGBuffer->OnCreate(this->pDevice, 
            &this->resViewHeaps,
            {
                //  g-buffer
                { GBUFFER_DEPTH, VK_FORMAT_D32_SFLOAT},
                { GBUFFER_WORLD_COORD, VK_FORMAT_R16G16B16A16_SFLOAT},
                { GBUFFER_NORMAL_BUFFER, VK_FORMAT_R16G16B16A16_SFLOAT},
                { GBUFFER_DIFFUSE, VK_FORMAT_R16G16B16A16_SFLOAT},
                { GBUFFER_SPECULAR_ROUGHNESS, VK_FORMAT_R16G16B16A16_SFLOAT},
                { GBUFFER_MOTION_VECTORS, VK_FORMAT_R16G16_SFLOAT},
                //  final rt
                { GBUFFER_FORWARD, VK_FORMAT_R16G16B16A16_SFLOAT},
            },
            1
        );
        GBufferFlags fullGBuffer = GBUFFER_DEPTH | 
            GBUFFER_WORLD_COORD | GBUFFER_NORMAL_BUFFER | 
            GBUFFER_DIFFUSE | GBUFFER_SPECULAR_ROUGHNESS | 
            GBUFFER_MOTION_VECTORS;
        this->rp_gBuffer_full.OnCreate(this->pGBuffer, fullGBuffer, true, "G-Buffer RenderPass");
        this->rp_skyDome.OnCreate(this->pGBuffer, GBUFFER_FORWARD, true, "SkyDome RenderPass");
    }
    //  pass 2.1 : D-Light
    {
        this->dLighting = new DirectLighting();
        this->dLighting->OnCreate(this->pDevice,
            &this->uploadHeap,
            &this->resViewHeaps,
            &this->dBufferRing,
            &this->sBufferPool
        );

        DLightInput::LightGBuffer lightGB;
        lightGB.depth = this->pRSM->m_DepthBufferSRV;
        this->dLighting->setLightGBuffer(&lightGB);
    }
    //  pass 2.2 : I-Light
    {
        this->iLighting = new IndirectLighting();
        this->iLighting->OnCreate(this->pDevice,
            &this->uploadHeap,
            &this->resViewHeaps,
            &this->dBufferRing,
            &this->sBufferPool
        );

        ILightInput::LightGBuffer lightGB;
        lightGB.worldCoord = this->pRSM->m_WorldCoordSRV;
        lightGB.normal = this->pRSM->m_NormalBufferSRV;
        lightGB.flux = this->pRSM->m_DiffuseSRV;
        this->iLighting->setLightGBuffer(&lightGB);
    }

    //  skydome
    {
        //  create skydome handle
        /*this->skyDome.OnCreate(this->pDevice, this->rp_pGBuffer_no_motion.GetRenderPass(),
        &this->uploadHeap, VK_FORMAT_R16G16B16A16_SFLOAT,
        &this->resViewHeaps, &this->dBufferRing, &this->sBufferPool,
        "..\\res\\Cauldron-Media\\envmaps\\papermill\\diffuse.dds",
        "..\\res\\Cauldron-Media\\envmaps\\papermill\\specular.dds",
        VK_SAMPLE_COUNT_1_BIT);*/
        this->skyDomeProc.OnCreate(this->pDevice, this->rp_skyDome.GetRenderPass(),
            &this->uploadHeap, VK_FORMAT_R16G16B16A16_SFLOAT,
            &this->resViewHeaps, &this->dBufferRing, &this->sBufferPool,
            VK_SAMPLE_COUNT_1_BIT);
    }

    //  initialize post-processing handles
    this->aggregator.OnCreate(this->pDevice, &this->resViewHeaps, &this->dBufferRing);
    this->toneMapping.OnCreate(this->pDevice, pSwapChain->GetRenderPass(), 
        &this->resViewHeaps, &this->sBufferPool, &this->dBufferRing);
    this->tAA.OnCreate(this->pDevice, &this->resViewHeaps, &this->sBufferPool, &this->dBufferRing);

    // Initialize UI rendering resources
    this->gui.OnCreate(this->pDevice, pSwapChain->GetRenderPass(), &this->uploadHeap, &this->dBufferRing);

	//	upload geom data to GPU
	this->sBufferPool.UploadData(this->uploadHeap.GetCommandList());
	this->uploadHeap.FlushAndFinish();
}

void Renderer::OnDestroy()
{
    this->gui.OnDestroy();

    this->tAA.OnDestroy();
    this->toneMapping.OnDestroy();
    this->aggregator.OnDestroy();

    this->skyDomeProc.OnDestroy();
    /*this->skyDome.OnDestroy();*/
    this->rp_skyDome.OnDestroy();

    this->iLighting->OnDestroy();
    delete this->iLighting;
    this->iLighting = nullptr;

    this->dLighting->OnDestroy();
    delete this->dLighting;
    this->dLighting = nullptr;

    this->rp_gBuffer_full.OnDestroy();
    this->pGBuffer->OnDestroy();
    delete this->pGBuffer;
    this->pGBuffer = nullptr;

    this->rp_RSM_full.OnDestroyWindowSizeDependentResources();
    this->pRSM->OnDestroyWindowSizeDependentResources();
    this->rp_RSM_full.OnDestroy();
    this->pRSM->OnDestroy();
    delete this->pRSM;
    this->pRSM = nullptr;

    // this->gTimeStamps.OnDestroy();
    this->uploadHeap.OnDestroy();
    this->cmdBufferRing.OnDestroy();
    this->resViewHeaps.OnDestroy();
    this->dBufferRing.OnDestroy();
    this->sBufferPool.OnDestroy();
}

void Renderer::OnCreateWindowSizeDependentResources(SwapChain* pSwapChain, uint32_t Width, uint32_t Height)
{
    this->width = Width;
    this->height = Height;

    // Set the viewport
    this->viewport.x = 0;
    this->viewport.y = (float)Height;
    this->viewport.width = (float)Width;
    this->viewport.height = -(float)(Height);
    this->viewport.minDepth = (float)0.0f;
    this->viewport.maxDepth = (float)1.0f;

    // Create scissor rectangle
    this->rectScissor.extent.width = Width;
    this->rectScissor.extent.height = Height;
    this->rectScissor.offset.x = 0;
    this->rectScissor.offset.y = 0;

    this->pGBuffer->OnCreateWindowSizeDependentResources(pSwapChain, Width, Height);
    this->rp_gBuffer_full.OnCreateWindowSizeDependentResources(Width, Height);
    this->rp_skyDome.OnCreateWindowSizeDependentResources(Width, Height);

    this->dLighting->OnCreateWindowSizeDependentResources(Width, Height, this->pGBuffer);
    {
        DLightInput::CameraGBuffer camGB;
        camGB.worldCoord = this->pGBuffer->m_WorldCoordSRV;
        camGB.normal = this->pGBuffer->m_NormalBufferSRV;
        camGB.diffuse = this->pGBuffer->m_DiffuseSRV;
        camGB.specular = this->pGBuffer->m_SpecularRoughnessSRV;
        this->dLighting->setCameraGBuffer(&camGB);
    }

    this->iLighting->OnCreateWindowSizeDependentResources(Width / 2, Height / 2);
    {
        ILightInput::CameraGBuffer camGB;
        camGB.worldCoord = this->pGBuffer->m_WorldCoordSRV;
        camGB.normal = this->pGBuffer->m_NormalBufferSRV;
        this->iLighting->setCameraGBuffer(&camGB);
    }

    this->aggregator.setInputImages(
        this->pGBuffer->m_HDRSRV,
        this->iLighting->srv_output,
        Width, Height
    );

    this->tAA.OnCreateWindowSizeDependentResources(Width, Height, this->pGBuffer);
    this->toneMapping.UpdatePipelines(pSwapChain->GetRenderPass());

    this->gui.UpdatePipeline(pSwapChain->GetRenderPass());
}

void Renderer::OnDestroyWindowSizeDependentResources()
{
    this->tAA.OnDestroyWindowSizeDependentResources();

    this->iLighting->OnDestroyWindowSizeDependentResources();
    this->dLighting->OnDestroyWindowSizeDependentResources();

    this->rp_skyDome.OnDestroyWindowSizeDependentResources();
    this->rp_gBuffer_full.OnDestroyWindowSizeDependentResources();
    this->pGBuffer->OnDestroyWindowSizeDependentResources();
}

void Renderer::OnRender(SwapChain* pSwapChain, Camera* pCamera, Renderer::State* pState)
{
    //  preparing for a new frame
    this->dBufferRing.OnBeginFrame();

    //  start recording cmd buffer for main rendering
    VkCommandBuffer cmdBuf1 = this->cmdBufferRing.GetNewCommandList();
    {
        VkCommandBufferBeginInfo cmd_buf_info;
        cmd_buf_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        cmd_buf_info.pNext = NULL;
        cmd_buf_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        cmd_buf_info.pInheritanceInfo = NULL;
        VkResult res = vkBeginCommandBuffer(cmdBuf1, &cmd_buf_info);
        assert(res == VK_SUCCESS);
    }

    //  seed TAA
    static uint32_t seed;
    pCamera->SetProjectionJitter(this->width, this->height, seed);

    //  set per-frame data
    per_frame* pPerFrameData = nullptr;
    if (this->res_scene)
    {
        //  set camera
        pPerFrameData = this->res_scene->m_pGLTFCommon->SetPerFrameData(*pCamera);

        //  disable RSM write
        pPerFrameData->rsmLightIndex = -1;

        //  set light properties
        pPerFrameData->iblFactor = 0.36f;
        pPerFrameData->emmisiveFactor = 1.f;
        pPerFrameData->invScreenResolution[0] = 1.f / static_cast<float>(this->width);
        pPerFrameData->invScreenResolution[1] = 1.f / static_cast<float>(this->height);

        //  setup light render target
        int lightIndex = 0;
        pPerFrameData->lights[lightIndex].shadowMapIndex = 0;
        if (pPerFrameData->lights[lightIndex].type == LightType_Directional)
        {
            pPerFrameData->lights[lightIndex].depthBias = 100.0f / 100000.0f;
        }
        else if (pPerFrameData->lights[lightIndex].type == LightType_Spot)
        {
            pPerFrameData->lights[lightIndex].depthBias = 70.0f / 100000.0f;
        }
        else
            pPerFrameData->lights[lightIndex].shadowMapIndex = -1;
        
        this->res_scene->SetPerFrameConstants();
        //this->res_scene->SetSkinningMatricesForSkeletons();
    }

    //  render skydome as foundation
    if(pPerFrameData)
    {
        this->rp_skyDome.BeginPass(cmdBuf1, this->rectScissor);

        SkyDomeProc::Constants skyDomeConstants;
        skyDomeConstants.invViewProj = XMMatrixInverse(NULL, pPerFrameData->mCameraCurrViewProj);
        skyDomeConstants.vSunDirection = pState->sunDir;
        skyDomeConstants.turbidity = 10.0f;
        skyDomeConstants.rayleigh = 2.0f;
        skyDomeConstants.mieCoefficient = 0.005f;
        skyDomeConstants.mieDirectionalG = 0.8f;
        skyDomeConstants.luminance = 1.0f;
        skyDomeConstants.sun = true; // ToDo : try false and see difference
        this->skyDomeProc.Draw(cmdBuf1, skyDomeConstants);

        this->rp_skyDome.EndPass(cmdBuf1);
    }

    using BatchList = GltfPbrPass::BatchList;
    std::vector<BatchList> opaques, transparents;
    bool gBufReady = false, rsmReady = false;

    //  pass 1.1 : G-Buffer / main pass
    if(this->pGltfPbrPass && pPerFrameData)
    {
        //  retrieve render batch lists of separated opaque meshes and transparent meshes
        this->pGltfPbrPass->BuildBatchLists(&opaques, &transparents);

        //  determine render area
        VkRect2D rectScissor_GBuffer = this->rectScissor;

        //  render scene (opaque objects)
        //  ToDo : render scene (transparent objects)
        {
            this->rp_gBuffer_full.BeginPass(cmdBuf1, rectScissor_GBuffer);

            this->pGltfPbrPass->DrawBatchList(cmdBuf1, &opaques);

            this->rp_gBuffer_full.EndPass(cmdBuf1);
        }

        gBufReady = true;
    }

    //  Pass 1.2 : reflective shadow map
    if (this->pRSMPass && pPerFrameData)
    {
        //  setup each RSM quarter
        const uint32_t viewportOffsetsX[4] = { 0, 1, 0, 1 };
        const uint32_t viewportOffsetsY[4] = { 0, 0, 1, 1 };
        const uint32_t viewportWidth = shadowmapSize;
        const uint32_t viewportHeight = shadowmapSize;

        //  ToDo : setup this pass to utilize multiple light src. (<=4)
        //  set light frustum info
        int rsmIndex = 0;
        pPerFrameData->mCameraCurrViewProj = pPerFrameData->lights[rsmIndex].mLightViewProj;
        pPerFrameData->rsmLightIndex = rsmIndex;
        this->res_scene->SetPerFrameConstants();

        //  prepare batches
        opaques.clear(); transparents.clear();
        this->pRSMPass->BuildBatchLists(&opaques, &transparents);

        //  determine render area
        VkRect2D rectScissor_RSM;
        rectScissor_RSM.offset = { (int32_t)(viewportOffsetsX[rsmIndex] * viewportWidth),
                                (int32_t)(viewportOffsetsY[rsmIndex] * viewportHeight) };
        rectScissor_RSM.extent = { viewportWidth, viewportHeight };

        //  render scene (opaque objects)
        //  ToDo : render scene (transparent objects)
        {
            this->rp_RSM_full.BeginPass(cmdBuf1, rectScissor_RSM);

            this->pRSMPass->DrawBatchList(cmdBuf1, &opaques);

            this->rp_RSM_full.EndPass(cmdBuf1);
        }

        rsmReady = true;
    }

    if (gBufReady && rsmReady)
    {
        //  image barrier (synchronization) before lighting phase
        this->doGeomDataTransition(cmdBuf1);

        //  pass 2.1 : D-light
        //
        
        //  set uniform data
        DirectLighting::per_frame* dLightingPerFrameData = this->dLighting->SetPerFrameConstants();
        dLightingPerFrameData->cameraPos = pPerFrameData->cameraPos;
        dLightingPerFrameData->light = pPerFrameData->lights[0];

        this->dLighting->Draw(cmdBuf1, &this->rectScissor);
        

        //  pass 2.2 : I-light
        //
        
        //  set uniform data
        IndirectLighting::per_frame* iLightingPerFrameData = this->iLighting->SetPerFrameConstants();
        iLightingPerFrameData->light = pPerFrameData->lights[0];

        this->iLighting->Draw(cmdBuf1, &this->rectScissor);
    }

    //  image barrier (synchronization) before aggregation
    this->doLightingTransition(cmdBuf1);

    //  aggregation (D-Light + I-Light)
    this->aggregator.Draw(cmdBuf1, pState->DIWeight);

    //  image barrier (synchronization) before TAA
    this->doAggregationTransition(cmdBuf1);

    //  resolve TAA
    this->tAA.Draw(cmdBuf1);

    //  submit cmd buffer for rendering
    {
        VkResult res = vkEndCommandBuffer(cmdBuf1);
        assert(res == VK_SUCCESS);

        VkSubmitInfo submit_info;
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.pNext = NULL;
        submit_info.waitSemaphoreCount = 0;
        submit_info.pWaitSemaphores = NULL;
        submit_info.pWaitDstStageMask = NULL;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &cmdBuf1;
        submit_info.signalSemaphoreCount = 0;
        submit_info.pSignalSemaphores = NULL;
        res = vkQueueSubmit(this->pDevice->GetGraphicsQueue(), 1, &submit_info, VK_NULL_HANDLE);
        assert(res == VK_SUCCESS);
    }

    //  wait for next swapchain image
    int imageIndex = pSwapChain->WaitForSwapChain();

    //  preparing for a new frame
    this->cmdBufferRing.OnBeginFrame();

    //  start recording cmd buffer for tone-maping & GUI
    VkCommandBuffer cmdBuf2 = this->cmdBufferRing.GetNewCommandList();
    {
        VkCommandBufferBeginInfo cmd_buf_info;
        cmd_buf_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        cmd_buf_info.pNext = NULL;
        cmd_buf_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        cmd_buf_info.pInheritanceInfo = NULL;
        VkResult res = vkBeginCommandBuffer(cmdBuf2, &cmd_buf_info);
        assert(res == VK_SUCCESS);
    }

    //  begin render pass towards swapchain image
    {
        VkRenderPassBeginInfo rp_begin = {};
        rp_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp_begin.pNext = NULL;
        rp_begin.renderPass = pSwapChain->GetRenderPass();
        rp_begin.framebuffer = pSwapChain->GetFramebuffer(imageIndex);
        rp_begin.renderArea.offset.x = 0;
        rp_begin.renderArea.offset.y = 0;
        rp_begin.renderArea.extent.width = this->width;
        rp_begin.renderArea.extent.height = this->height;
        rp_begin.clearValueCount = 0;
        rp_begin.pClearValues = NULL;
        vkCmdBeginRenderPass(cmdBuf2, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

        vkCmdSetScissor(cmdBuf2, 0, 1, &this->rectScissor);
        vkCmdSetViewport(cmdBuf2, 0, 1, &this->viewport);
    }

    //  do tonemapping
    {
        //  Note : TAA already done transition on 'm_HDR' for us, so we don't need explicit transition
        this->toneMapping.Draw(cmdBuf2, this->pGBuffer->m_HDRSRV, 1.f, 0);
    }

    //  render GUI
    {
        this->gui.Draw(cmdBuf2);
    }

    //  end render pass
    vkCmdEndRenderPass(cmdBuf2);

    //  submit cmd buffer for presenting
    {
        VkResult res = vkEndCommandBuffer(cmdBuf2);
        assert(res == VK_SUCCESS);

        VkSemaphore ImageAvailableSemaphore;
        VkSemaphore RenderFinishedSemaphores;
        VkFence CmdBufExecutedFences;
        pSwapChain->GetSemaphores(&ImageAvailableSemaphore, &RenderFinishedSemaphores, &CmdBufExecutedFences);

        VkPipelineStageFlags submitWaitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit_info2;
        submit_info2.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info2.pNext = NULL;
        submit_info2.waitSemaphoreCount = 1;
        submit_info2.pWaitSemaphores = &ImageAvailableSemaphore;
        submit_info2.pWaitDstStageMask = &submitWaitStage;
        submit_info2.commandBufferCount = 1;
        submit_info2.pCommandBuffers = &cmdBuf2;
        submit_info2.signalSemaphoreCount = 1;
        submit_info2.pSignalSemaphores = &RenderFinishedSemaphores;

        res = vkQueueSubmit(this->pDevice->GetGraphicsQueue(), 1, &submit_info2, CmdBufExecutedFences);
        assert(res == VK_SUCCESS);
    }
}

int Renderer::loadScene(GLTFCommon* pLoader, int stage)
{
    // Loading stages
    //
    if (stage == 0)
    {
    }
    else if (stage == 1)
    {
        Profile p("SceneResource->Load");

        // here we are loading onto the GPU all the textures and the inverse matrices
        // this data will be used to create the PBR and Depth passes 
        this->res_scene = new GLTFTexturesAndBuffers();
        this->res_scene->OnCreate(
            this->pDevice,
            pLoader,
            &this->uploadHeap,
            &this->sBufferPool,
            &this->dBufferRing
        );
        this->res_scene->LoadTextures(&this->asyncPool);
    }
    else if (stage == 2)
    {
        Profile p("RSM->CreatePass");

        this->pRSMPass = new GltfPbrPass();
        this->pRSMPass->OnCreate(
            this->pDevice,
            &this->uploadHeap,
            &this->resViewHeaps,
            &this->dBufferRing,
            &this->sBufferPool,
            this->res_scene,
            nullptr, // no non-procedural skydome
            false, // no SSAO
            VK_NULL_HANDLE, // no shadow calculation in GBuffer pass
            &this->rp_RSM_full,
            &this->asyncPool
        );
    }
    else if (stage == 3)
    {
        Profile p("GBuffer->CreatePass");

        this->pGltfPbrPass = new GltfPbrPass();
        this->pGltfPbrPass->OnCreate(
            this->pDevice,
            &this->uploadHeap,
            &this->resViewHeaps,
            &this->dBufferRing,
            &this->sBufferPool,
            this->res_scene,
            nullptr, // no non-procedural skydome
            false, // no SSAO
            VK_NULL_HANDLE, // no shadow calculation in GBuffer pass
            &this->rp_gBuffer_full,
            &this->asyncPool
        );
    }
    else if (stage == 4)
    {
        Profile p("Flush");

        this->sBufferPool.UploadData(this->uploadHeap.GetCommandList());
        this->uploadHeap.FlushAndFinish();

        //  once everything is uploaded, we dont need the upload heaps anymore
        this->sBufferPool.FreeUploadHeap();

        //  tell caller that we are done loading the map
        return 0;
    }

    stage++;
    return stage;
}

void Renderer::unloadScene()
{
    this->pDevice->GPUFlush();

    if (this->pGltfPbrPass)
    {
        this->pGltfPbrPass->OnDestroy();
        delete this->pGltfPbrPass;
        this->pGltfPbrPass = nullptr;
    }

    if (this->pRSMPass)
    {
        this->pRSMPass->OnDestroy();
        delete this->pRSMPass;
        this->pRSMPass = nullptr;
    }

    if (this->res_scene)
    {
        this->res_scene->OnDestroy();
        delete this->res_scene;
        this->res_scene = nullptr;
    }
}

void Renderer::setupRenderPass()
{
}

void Renderer::doGeomDataTransition(VkCommandBuffer cmdBuf)
{
    //  transition images
    const uint32_t numBarriers = 8;
    VkImageMemoryBarrier barriers[numBarriers];
    barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[0].pNext = NULL;
    barriers[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
    barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].subresourceRange.baseMipLevel = 0;
    barriers[0].subresourceRange.levelCount = 1;
    barriers[0].subresourceRange.baseArrayLayer = 0;
    barriers[0].subresourceRange.layerCount = 1;

    //  for g-buffer
    {
        //  barrier 0 : world coord
        barriers[0].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barriers[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barriers[0].image = this->pGBuffer->m_WorldCoord.Resource();

        //  barrier 1 : normal
        barriers[1] = barriers[0];
        barriers[1].image = this->pGBuffer->m_NormalBuffer.Resource();

        //  barrier 2 : diffuse
        barriers[2] = barriers[0];
        barriers[2].image = this->pGBuffer->m_Diffuse.Resource();

        //  barrier 3 : specular
        barriers[3] = barriers[0];
        barriers[3].image = this->pGBuffer->m_SpecularRoughness.Resource();
    }

    //  for RSM
    {
        //  barrier 4 : world coord
        barriers[4] = barriers[0];
        barriers[4].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barriers[4].image = this->pRSM->m_WorldCoord.Resource();

        //  barrier 5 : normal
        barriers[5] = barriers[4];
        barriers[5].image = this->pRSM->m_NormalBuffer.Resource();

        //  barrier 6 : flux
        barriers[6] = barriers[4];
        barriers[6].image = this->pRSM->m_Diffuse.Resource();

        //  barrier 7 : depth
        barriers[7] = barriers[0];
        barriers[7].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        barriers[7].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barriers[7].oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        barriers[7].newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        barriers[7].subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        barriers[7].image = this->pRSM->m_DepthBuffer.Resource();
    }

    vkCmdPipelineBarrier(cmdBuf,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, NULL, 0, NULL,
        numBarriers, barriers);
}

void Renderer::doLightingTransition(VkCommandBuffer cmdBuf)
{
    //  transition images
    const uint32_t numBarriers = 2;
    VkImageMemoryBarrier barriers[numBarriers];
    barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[0].pNext = NULL;
    barriers[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT; // no RAW or WAW hazard for sure, so this works
    barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].subresourceRange.baseMipLevel = 0;
    barriers[0].subresourceRange.levelCount = 1;
    barriers[0].subresourceRange.baseArrayLayer = 0;
    barriers[0].subresourceRange.layerCount = 1;
    
    //  barrier 0 : d-light
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barriers[0].image = this->pGBuffer->m_HDR.Resource();

    //  barrier 1 : i-light
    barriers[1] = barriers[0];
    barriers[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barriers[1].image = this->iLighting->output.Resource();

    vkCmdPipelineBarrier(cmdBuf,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, NULL, 0, NULL,
        numBarriers, barriers);
}

void Renderer::doAggregationTransition(VkCommandBuffer cmdBuf)
{
    //  transition images
    const uint32_t numBarriers = 3;
    VkImageMemoryBarrier barriers[numBarriers];
    barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[0].pNext = NULL;
    barriers[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].subresourceRange.baseMipLevel = 0;
    barriers[0].subresourceRange.levelCount = 1;
    barriers[0].subresourceRange.baseArrayLayer = 0;
    barriers[0].subresourceRange.layerCount = 1;

    //  barrier 0 : color buffer -> TAA
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barriers[0].image = this->pGBuffer->m_HDR.Resource();

    //  barrier 1 : motion vector
    barriers[1] = barriers[0];
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barriers[1].image = this->pGBuffer->m_MotionVectors.Resource();

    //  barrier 2 : camera depth
    barriers[2] = barriers[0];
    barriers[2].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    barriers[2].oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    //barriers[2].newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL; // this layout should be used, but TAA's desc uses SHADER_READ_ONLY_OPTIMAL instead. 
    barriers[2].subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    barriers[2].image = this->pGBuffer->m_DepthBuffer.Resource();

    vkCmdPipelineBarrier(cmdBuf,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | 
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, NULL, 0, NULL,
        numBarriers, barriers);
}
