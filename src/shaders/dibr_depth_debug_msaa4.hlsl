Texture2DMS<float, 4> depthTex : register(t1);
SamplerState s0 : register(s0);

cbuffer Params : register(b0)
{
    float gain;
    float convergence;
    float signv;
    float depthW;
    float depthH;
    float depthScale;
    float pad1;
    float pad2;
};

float ReadDepth(float2 uv)
{
    uint2 p = uint2(saturate(uv) * float2(max(depthW - 1.0, 1.0), max(depthH - 1.0, 1.0)));
    float d0 = depthTex.Load(p, 0);
    float d1 = depthTex.Load(p, 1);
    float d2 = depthTex.Load(p, 2);
    float d3 = depthTex.Load(p, 3);
    return max(max(d0, d1), max(d2, d3));
}

float4 PSMain(float4 pos : SV_Position, float2 uvIn : TEXCOORD0) : SV_Target
{
    float d = ReadDepth(uvIn);
    float outv = sqrt(saturate(d * depthScale));   // live Depth-scale slider drives the F8 map (was hardcoded 96)
    return float4(outv, outv, outv, 1.0);
}
