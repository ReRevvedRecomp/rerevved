// Scales the post-gamma R10G10B10A2 display texture into an RGBA8 swap chain.
// The producer already applied the guest gamma ramp, so this pass samples and
// quantizes only: no second gamma and no implicit sRGB conversion.

Texture2D<float4> displayTexture : register(t0);
SamplerState      displaySampler : register(s1);

struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

// One triangle covers the viewport; the viewport carries the letterbox.
VertexOutput VSMain(uint vertexId : SV_VertexID)
{
    VertexOutput output;
    const float2 corner = float2((vertexId << 1) & 2, vertexId & 2);
    output.position     = float4(corner.x * 2.0 - 1.0, 1.0 - corner.y * 2.0, 0.0, 1.0);
    output.uv           = corner;
    return output;
}

float4 PSMain(VertexOutput input) : SV_Target0
{
    const float3 color = displayTexture.Sample(displaySampler, input.uv).rgb;
    // Round to the nearest 8-bit level here. The render target conversion of
    // an arbitrary float may round either way near a tie, so it only ever
    // receives exact multiples of 1/255.
    return float4(floor(color * 255.0 + 0.5) / 255.0, 1.0);
}
