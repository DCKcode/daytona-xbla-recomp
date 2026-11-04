[[vk::binding(0,0)]] Texture2D<float4> sourceImage;
[[vk::binding(1,0)]] cbuffer Display { uint4 gamma[256]; };
float4 main(float4 pos : SV_Position) : SV_Target {
  uint3 rgb = uint3(round(saturate(sourceImage.Load(int3(pos.xy,0)).rgb) * 255));
  return float4(gamma[rgb.r].r, gamma[rgb.g].g, gamma[rgb.b].b, 255) / 255;
}
