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
//  set 2 : G-BUffer samplers
//--------------------------------------------------------------------------------------

#include "perFrameStruct.h"

layout (std140, set = 0, binding = ID_PER_FRAME) uniform perFrame 
{
    Light u_light;
} myPerFrame;

layout (std140, set = 0, binding = ID_sampleOffsets) uniform SampleOffsets
{
    vec4 sampleOffsets[50]; // since it's impossible to align as 8-byte, use packed elements instead.
};
const int numSamples = 100;
const float r_max = 0.125; // considered from texcoord range (1 quarter) [0.0,0.5]

layout (set = 0, binding = ID_kernelRotations) uniform sampler2D kernelRotations;
const float noiseResolution = 4.0; // recommended at 4-16 

layout (set = 1, binding = 0) uniform sampler2D glight_WorldCoord;
layout (set = 1, binding = 1) uniform sampler2D glight_Normal;
layout (set = 1, binding = 2) uniform sampler2D glight_Flux;

layout (set = 2, binding = 0) uniform sampler2D gcam_WorldCoord;
layout (set = 2, binding = 1) uniform sampler2D gcam_Normal;

//--------------------------------------------------------------------------------------
//  FS outputs
//--------------------------------------------------------------------------------------

layout (location = 0) out vec4 out_color;

//--------------------------------------------------------------------------------------
//  main function
//--------------------------------------------------------------------------------------

#include "functions.glsl"
#include "PBRLighting.h"

vec3 integrateIndirectIrradiance(Light light, vec3 worldPos, vec3 normal)
{
    vec3 irradiance = vec3(0.0, 0.0, 0.0);

    //  transform worldPos with LightViewProj
    vec4 rsmTexCoord = light.mLightViewProj * vec4(worldPos, 1.0);
    rsmTexCoord.xyz = rsmTexCoord.xyz / rsmTexCoord.w;
    if((rsmTexCoord.z < 0) || (rsmTexCoord.z > 1.0)) return irradiance;

    //  remember we are splitting the shadow map in 4 quarters 
    //  from [-1,1] to [0,0.5] (+ offset)
    rsmTexCoord.x = (1.0 + rsmTexCoord.x) * 0.25;
    rsmTexCoord.y = (1.0 - rsmTexCoord.y) * 0.25;
    const float offsetsX[4] = { 0.0, 0.5, 0.0, 0.5 };
    const float offsetsY[4] = { 0.0, 0.0, 0.5, 0.5 };

    //  fetch rotation angle
    //  map rsmTexCoord to noiseTexCoord : [0,0.5] -> [0,4] (repeatable texture)
    float rotationAngle = texture(kernelRotations, rsmTexCoord.xy * noiseResolution).r;
    rotationAngle *= 2.0 * M_PI;

    //  sampling & accumulate
    for (int i = 0; i < numSamples; i++)
    {
        //  fetch offset
        vec2 sampleOffset = vec2(0.0, 0.0);
        if (i % 2 == 0) sampleOffset = sampleOffsets[i/2].xy;
        else sampleOffset = sampleOffsets[i/2].zw;

        //  rotate the offset vector
        float sin_theta = sin(rotationAngle);
        float cos_theta = cos(rotationAngle);
        sampleOffset = mat2(cos_theta, sin_theta, -sin_theta, cos_theta) * sampleOffset;
        
        //  determine sampling location in RSM
        vec2 sampleCoord = rsmTexCoord.xy + sampleOffset * r_max;
        if ((sampleCoord.y < 0) || (sampleCoord.y > .5) ||
            (sampleCoord.x < 0) || (sampleCoord.x > .5)) continue;
        sampleCoord.x += offsetsX[light.shadowMapIndex];
        sampleCoord.y += offsetsY[light.shadowMapIndex];

        //  calculate irradiance
        vec3 light_WorldPos = texture(glight_WorldCoord, sampleCoord).rgb;
        vec3 light_Normal = texture(glight_Normal, sampleCoord).rgb * 2.0 - 1.0;
        vec3 light_Flux = texture(glight_Flux, sampleCoord).rgb;

        vec3 lightPointToCamPoint = worldPos - light_WorldPos;
        vec3 camPointToLightPoint = light_WorldPos - worldPos;
        float distanceBetweenPoint = length(lightPointToCamPoint);
        float angleAttenuation = max(dot(normalize(lightPointToCamPoint), light_Normal), 0.0) * max(dot(normalize(camPointToLightPoint), normal), 0.0);
        float rangeAttenuation = light.range * getRangeAttenuation(light.range, length(light.position - light_WorldPos));
        if (rangeAttenuation > 0.0) rangeAttenuation = getRangeAttenuation(rangeAttenuation / M_PI, distanceBetweenPoint);

        //  perform importance sampling (Monte Carlo integration)
        float weight = dot(sampleOffset, sampleOffset);
        irradiance += light_Flux * weight * angleAttenuation * rangeAttenuation / pow(distanceBetweenPoint, 4);
    }
    irradiance /= numSamples;

    return irradiance;
}

void main()
{
    //  flip UV coord first
    vec2 texCoord = vec2(UV.x, 1.0 - UV.y);

    //  load pixel data from g-buffer
    vec3 cam_WorldPos = texture(gcam_WorldCoord, texCoord).rgb;
    vec3 cam_Normal = texture(gcam_Normal, texCoord).rgb * 2.0 - 1.0;
    
    //  indirect lighting
    vec3 i_light = integrateIndirectIrradiance(myPerFrame.u_light, cam_WorldPos, cam_Normal);

    //  calculate final color
    out_color = vec4(i_light, 1.0);
}
