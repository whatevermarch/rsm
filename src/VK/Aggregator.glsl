#version 450

#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable
#extension GL_ARB_compute_shader  : enable

//--------------------------------------------------------------------------------------
//  CS workgroup definition
//--------------------------------------------------------------------------------------

layout (local_size_x = 32, local_size_y = 32) in;

//--------------------------------------------------------------------------------------
//  uniform data
//  set 0 : uniform data
//--------------------------------------------------------------------------------------

struct AggregatorParams
{
    float weight;
    uint imgWidth;
    uint imgHeight;

    float padding;
};
layout (std140, binding = ID_Params) uniform Params 
{
    AggregatorParams params;
};

layout (rgba16f, binding = ID_DLight) uniform image2D img_DLight;
layout (binding = ID_ILight) uniform sampler2D img_ILight;

//--------------------------------------------------------------------------------------
//  main function
//--------------------------------------------------------------------------------------

void main()
{
    ivec2 coords = ivec2(gl_GlobalInvocationID.xy);

    vec4 dLightColor = imageLoad(img_DLight, coords).rgba;
    vec3 iLightColor = texture(img_ILight, vec2(float(coords.x) / params.imgWidth, float(coords.y) / params.imgHeight)).rgb;

    float d_weight = clamp(params.weight, 0.0f, 0.5f);
    float i_weight = clamp(params.weight, 0.5f, 1.0f);
    vec3 outColor = (dLightColor.xyz * d_weight + iLightColor * (1.0f - i_weight)) * 2.0f;

    imageStore(img_DLight, coords, vec4(outColor, dLightColor.w));
}
