Texture2D<float> depthTex : register(t1);
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
    return depthTex.Sample(s0, saturate(uv));
}

float4 PSMain(float4 pos : SV_Position, float2 uvIn : TEXCOORD0) : SV_Target
{
    float d = ReadDepth(uvIn);
    float outv = sqrt(saturate(d * depthScale));   // live Depth-scale slider drives the F8 map (was hardcoded 96)
    return float4(outv, outv, outv, 1.0);
}
