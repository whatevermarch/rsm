#pragma once

#define WIN32_LEAN_AND_MEAN             // Exclude rarely-used stuff from Windows headers
// Windows Header Files:
#include <windows.h>
#include <windowsx.h>

// C RunTime Header Files
#include <vector>
#include <fstream>

#include "vulkan/vulkan.h"

// DirectX math library
#include <DirectXMath.h>
using namespace DirectX;

// TODO: reference additional headers your program requires here
// ex. #include "PostProc/ToneMapping.h"
#include "Base/Imgui.h"
#include "Base/ImguiHelper.h"
#include "Base/Helper.h"
#include "Base/Device.h"
#include "Base/Texture.h"
#include "Base/SwapChain.h"
#include "Base/UploadHeap.h"
#include "Base/GPUTimestamps.h"
#include "Base/ExtDebugMarkers.h"
#include "Base/CommandListRing.h"
#include "Base/StaticBufferPool.h"
#include "Base/DynamicBufferRing.h"
#include "Base/ResourceViewHeaps.h"
#include "Base/ShaderCompilerHelper.h"
#include "Base/GBuffer.h"

#include "Misc/Misc.h"
#include "Misc/Camera.h"
#include "Misc/FrameworkWindows.h"

#include "GLTF/GLTFTexturesAndBuffers.h"
#include "GLTF/GltfPbrPass.h"
#include "GLTF/GltfDepthPass.h"

#include "PostProc/SkyDome.h"
#include "PostProc/SkyDomeProc.h"
#include "PostProc/DownSamplePS.h"
#include "PostProc/Tonemapping.h"
#include "PostProc/TAA.h"

using namespace CAULDRON_VK;
using SceneLoader = GLTFCommon;
using GUI = ImGUI;
