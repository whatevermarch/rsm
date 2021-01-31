#pragma once

#include "RSM.h"
#include "DirectLighting.h"
#include "IndirectLighting.h"
#include "Aggregator.h"

class Renderer
{
public:

	struct State
	{
		XMVECTOR sunDir;
		float DIWeight = 0.5f; // 0 = full dLight, 1 = full iLight
	};

	//	mandatory methods
	void OnCreate(Device* pDevice, SwapChain* pSwapChain);
	void OnDestroy();
	void OnCreateWindowSizeDependentResources(SwapChain* pSwapChain, uint32_t Width, uint32_t Height);
	void OnDestroyWindowSizeDependentResources();
	void OnRender(SwapChain* pSwapChain, Camera* pCamera, State* pState);

	int loadScene(GLTFCommon* pLoader, int stage);
	void unloadScene();

protected:

	//	pointer to device
	Device* pDevice = nullptr;

	uint32_t width, height;

	//	viewport & rectangle scissor
	VkViewport viewport;
	VkRect2D rectScissor;

	//  GUI (view component in MVC, 
	//	controller(C) will be managed in frontend App class)
	GUI gui;

	//	resources managers
	StaticBufferPool sBufferPool;	// geometry
	DynamicBufferRing dBufferRing;	// uniform buffers
	ResourceViewHeaps resViewHeaps;	// descriptor sets
	CommandListRing cmdBufferRing;	// command buffers
	UploadHeap uploadHeap;			// staging buffers
	// GPUTimestamps gTimeStamps;
	AsyncPool asyncPool;

	//	resources handles
	GLTFTexturesAndBuffers* res_scene = nullptr;
	
	//	G-Buffer pass
	GBuffer* pGBuffer = nullptr;						// G-Buffer itself
	GBufferRenderPass rp_gBuffer_full;
	GltfPbrPass* pGltfPbrPass = nullptr;				// PBR pass to store geom data

	//	RSM pass
	RSM* rsm = nullptr;
	
	//	lighting passes
	DirectLighting* dLighting = nullptr;
	IndirectLighting* iLighting = nullptr;
	bool dLightReady = false, iLightReady = false;

	//	skydome
	//SkyDome skyDome;
	SkyDomeProc skyDomeProc;
	GBufferRenderPass rp_skyDome;

	//	post-processing handle
	Aggregator aggregator;
	ToneMapping toneMapping;
	TAA tAA;
	
	//	ToDo : setup renderpass containing multiple subpasses instead
	void setupRenderPass();

	void doGeomDataTransition(VkCommandBuffer cmdBuf); // except camDepth and Motion
	void doLightingTransition(VkCommandBuffer cmdBuf); // D + I
	void doAggregationTransition(VkCommandBuffer cmdBuf); // include camDepth and Motion
};
