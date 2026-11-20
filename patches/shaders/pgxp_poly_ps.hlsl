// Replacement for RDE's textured polygon pixel shaders (ROUND_ALPHA: the
// variant that truncates alpha to 0 or 1). Texture coordinates interpolate
// perspective-correct when the vertex shader passes a w (perspective); the
// colour of such a polygon interpolates affinely, as on the PS1. Without w the
// result is computed as by the original shader.

cbuffer cbParameter : register(b4)
{
  float4 textureWindow;
};

Texture2D inputTexture : register(t0);
SamplerState inputSampler : register(s0);

float4 main(float4 pos : SV_POSITION, float4 col : COLOR0, float2 uv : TEXCOORD0,
            noperspective float4 affineCol : COLOR1,
            nointerpolation float perspective : TEXCOORD1) : SV_Target
{
  float2 scaled = uv * textureWindow.zw;
  float2 size = (scaled >= -scaled) ? textureWindow.zw : -textureWindow.zw;
  float2 t = textureWindow.xy + size * frac((1.0 / size) * uv);
  float4 c = inputTexture.Sample(inputSampler, t) * ((perspective > 0.5) ? affineCol : col);
#ifdef ROUND_ALPHA
  c.w = (float)(uint)c.w;
#endif
  return c;
}
