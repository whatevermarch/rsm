#version 450

#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

precision highp float;

//--------------------------------------------------------------------------------------
//  FS inputs
//--------------------------------------------------------------------------------------

layout (location = 0) in vec2 UV;

//--------------------------------------------------------------------------------------
//  uniform data
//  set 0 : uniform data
//  set 1 : RSM samplers
//  set 2 : G-BUffer input attachments
//--------------------------------------------------------------------------------------

#include "perFrameStruct.h"

layout (std140, set = 0, binding = ID_PER_FRAME) uniform perFrame 
{
    vec4 u_cameraPos;
    Light u_light;
} myPerFrame;

// shadow (depth) sampler is declared in "shadowFiltering.h"
// layout(set = 1, binding = ID_shadowMap(= 0)) uniform sampler2DShadow u_shadowMap;

// input_attachment_index = index declared in framebuffer
layout (input_attachment_index = 1, set = 2, binding = 0) uniform subpassInput gcam_WorldCoord;
layout (input_attachment_index = 2, set = 2, binding = 1) uniform subpassInput gcam_Normal;
layout (input_attachment_index = 3, set = 2, binding = 2) uniform subpassInput gcam_Diffuse;
layout (input_attachment_index = 4, set = 2, binding = 3) uniform subpassInput gcam_Specular;

//--------------------------------------------------------------------------------------
//  FS outputs
//--------------------------------------------------------------------------------------

layout (location = 0) out vec4 out_color;

//--------------------------------------------------------------------------------------
//  main function
//--------------------------------------------------------------------------------------

#include "functions.glsl"
#include "shadowFiltering.h"
#include "PBRLighting.h"

void main()
{
    //  load pixel data from g-buffer
    vec4 cam_WorldPos = subpassLoad(gcam_WorldCoord).rgba;

    //  discard irrelevant pixel
    if (cam_WorldPos.w != 1.0f)
        discard;

    vec3 cam_Normal = subpassLoad(gcam_Normal).rgb * 2.0 - 1.0;
    vec3 cam_Diffuse = subpassLoad(gcam_Diffuse).rgb;
    vec4 cam_SpecularRoughness = subpassLoad(gcam_Specular).rgba;
    vec3 cam_Specular = cam_SpecularRoughness.xyz;
    float cam_AlphaRoughness = cam_SpecularRoughness.w;

    vec3 view = normalize(myPerFrame.u_cameraPos.xyz - cam_WorldPos.xyz);

    //  evaluate shadow
    float shadow = DoSpotShadow(cam_WorldPos.xyz, myPerFrame.u_light);
    
    //  direct lighting
    out_color = vec4(doPbrLighting(
                        myPerFrame.u_light,
                        shadow,
                        cam_WorldPos.xyz,
                        cam_Normal,
                        view,
                        cam_Diffuse,
                        cam_Specular,
                        cam_AlphaRoughness), 
                    1.0);
}