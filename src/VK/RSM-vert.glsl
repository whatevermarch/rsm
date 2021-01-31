#version 450

//--------------------------------------------------------------------------------------
//  uniform data
//  set 0 : uniform data
//  set 1 : texture samplers
//--------------------------------------------------------------------------------------

#include "perFrameStruct.h"

layout (std140, set = 0, binding = ID_PER_FRAME) uniform perFrame 
{
    //mat4 u_MVPMatrix;
    Light u_light;
} myPerFrame;

layout (std140, set = 0, binding = ID_PER_OBJECT) uniform perObject
{
    mat4 u_ModelMatrix;
} myPerObject;

//--------------------------------------------------------------------------------------
//  VS outputs
//--------------------------------------------------------------------------------------

#include "GLTF_VS2PS_IO.glsl"

//--------------------------------------------------------------------------------------
//  main function
//--------------------------------------------------------------------------------------

//  helper functions used in gltfVertexFactory()
mat4 GetWorldMatrix()
{
    return myPerObject.u_ModelMatrix;
}

mat4 GetCameraViewProj()
{
    //return myPerFrame.u_MVPMatrix;
    return myPerFrame.u_light.mLightViewProj;
}

#include "GLTFVertexFactory.glsl"

void main()
{
    gltfVertexFactory();
}