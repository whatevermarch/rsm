#version 450

#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

precision highp float;

//--------------------------------------------------------------------------------------
//  FS inputs
//--------------------------------------------------------------------------------------

#include "GLTF_VS2PS_IO.glsl"

layout (location = 0) in VS2PS Input;

//--------------------------------------------------------------------------------------
//  uniform data
//  set 0 : uniform data
//  set 1 : texture samplers
//--------------------------------------------------------------------------------------

#include "perFrameStruct.h"

layout (std140, set = 0, binding = ID_PER_FRAME) uniform perFrame 
{
    Light u_light;
} myPerFrame;

#include "PixelParams.glsl"

layout (std140, set = 0, binding = ID_PER_OBJECT) uniform perObject
{
    mat4 u_ModelMatrix;
    PBRFactors u_pbrParams;
} myPerObject;

//--------------------------------------------------------------------------------------
//  FS outputs
//--------------------------------------------------------------------------------------

layout (location = 0) out vec4 g_worldCoord;

layout (location = 1) out vec4 g_normal;

layout (location = 2) out vec4 g_flux;

//--------------------------------------------------------------------------------------
//  main function
//--------------------------------------------------------------------------------------

#include "functions.glsl"
#include "PBRLighting.h"

vec4 calculateDirectionalLightFlux(Light light)
{
    vec3 flux = light.intensity * light.color;
    return vec4(flux, 1);
}

vec4 calculateSpotLightFlux(Light light, vec3 worldPos, vec3 normal)
{
    vec3 pointToLight = light.position - worldPos;
    float distanceFromLight = length(pointToLight);
    float rangeAttenuation = getRangeAttenuation(light.range, distanceFromLight);
    float spotAttenuation = getSpotAttenuation(pointToLight, -light.direction, light.outerConeCos, light.innerConeCos);
    vec3 flux = rangeAttenuation * spotAttenuation * light.intensity * light.color;
    return vec4(flux, 1);
}

void main()
{
    //  discard if this fragment is completely transparent
    discardPixelIfAlphaCutOff(Input);

    //  record to G-buffer
    //  world coord
    g_worldCoord = vec4(Input.WorldPos, 1);

    //  normal
    vec3 Normal = getPixelNormal(Input);
    g_normal = vec4((Normal + 1) / 2, 0); // transform range from [-1,1] to [0,1]

    //  flux
    vec4 flux = getBaseColor(Input, myPerObject.u_pbrParams);
    if (myPerFrame.u_light.type == LightType_Directional)
        flux *= calculateDirectionalLightFlux(myPerFrame.u_light);
    else if (myPerFrame.u_light.type == LightType_Spot)
        flux *= calculateSpotLightFlux(myPerFrame.u_light, Input.WorldPos, Normal);
    g_flux = flux;
}
