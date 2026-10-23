[[vk::binding(0, 0)]] Texture2DMS<float4, 4> sourceSamples;
[[vk::binding(1, 0)]] RWByteAddressBuffer sampleBytes;

uint packRgba8(float4 color)
{
    uint4 bytes = uint4(round(saturate(color) * 255.0));
    return bytes.x | (bytes.y << 8) | (bytes.z << 16) | (bytes.w << 24);
}

[numthreads(8, 8, 1)]
void main(uint3 pixel : SV_DispatchThreadID)
{
    uint width, height, samples;
    sourceSamples.GetDimensions(width, height, samples);
    if (pixel.x >= width || pixel.y >= height)
    {
        return;
    }
    uint offset = (pixel.y * width + pixel.x) * 4;
    sampleBytes.Store(offset, packRgba8(sourceSamples.Load(pixel.xy, 0)));
    sampleBytes.Store(width * height * 4 + offset, packRgba8(sourceSamples.Load(pixel.xy, 3)));
}
