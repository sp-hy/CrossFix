// Replacement for RDE's polygon vertex shader. Same input and leading output
// signature. POSITION.z carries the PGXP depth of a corrected polygon:
//   z = 0: as the original (w = POSITION.w = 1, same instructions, same output)
//   z > 0: the depth w of a perspective-corrected polygon
// perspective = 1 when the polygon has a w.

struct VSIn
{
  float4 pos : POSITION;
  float4 col : COLOR;
  float2 uv : TEXCOORD;
};

struct VSOut
{
  float4 pos : SV_POSITION;
  float4 col : COLOR0;
  float2 uv : TEXCOORD0;
  noperspective float4 affineCol : COLOR1;
  nointerpolation float perspective : TEXCOORD1;
};

VSOut main(VSIn i)
{
  VSOut o;
  bool corrected = i.pos.z > 0.0;
  float w = corrected ? i.pos.z : i.pos.w;
  float2 xw = i.pos.xy * w;
  o.pos.x = dot(float2(xw.x, w), float2(1.0 / 2048.0, -1.0));
  o.pos.y = dot(float2(xw.y, w), float2(-1.0 / 1024.0, 1.0));
  o.pos.z = 0.0;
  o.pos.w = w;
  o.col = i.col;
  o.uv = i.uv;
  o.affineCol = i.col;
  o.perspective = corrected ? 1.0 : 0.0;
  return o;
}
