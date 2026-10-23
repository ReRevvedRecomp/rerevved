// Matches the SDK immediate drawer: UNORM vertex color times a level-zero
// RGBA sample, with straight-alpha RGB blending and additive alpha.
Texture2D<float4> overlayTexture : register(t0);
SamplerState overlaySampler : register(s1);
cbuffer Coordinates : register(b0) { float2 inverseSize; };

struct VertexInput
{
    float2 position : POSITION;
    float2 uv : TEXCOORD;
    float4 color : COLOR;
};
struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD;
    float4 color : COLOR;
};
VertexOutput VSMain(VertexInput input)
{
    VertexOutput output;
    output.position = float4(input.position * inverseSize * float2(2, -2) + float2(-1, 1), 0, 1);
    output.uv = input.uv;
    output.color = input.color;
    return output;
}
float4 PSMain(VertexOutput input) : SV_Target
{
    return overlayTexture.SampleLevel(overlaySampler, input.uv, 0) * input.color;
}
