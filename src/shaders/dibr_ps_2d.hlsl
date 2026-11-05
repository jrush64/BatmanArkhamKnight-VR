Texture2D colorTex : register(t0);
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
    float edgeGuard;
    float edgeThreshold;
};

float ReadDepth(float2 uv)
{
    return depthTex.Sample(s0, saturate(uv));
}

float NearAmount(float2 uv)
{
    return saturate(ReadDepth(uv) * max(depthScale, 1.0));
}

float4 PSMain(float4 pos : SV_Position, float2 uvIn : TEXCOORD0) : SV_Target
{
    float nearAmount = NearAmount(uvIn);
    float disp = signv * gain * nearAmount;
    if (edgeGuard > 0.0)
    {
        float2 texel = 1.0 / float2(max(depthW, 1.0), max(depthH, 1.0));
        float nL = NearAmount(uvIn - float2(texel.x, 0.0));
        float nR = NearAmount(uvIn + float2(texel.x, 0.0));
        float nU = NearAmount(uvIn - float2(0.0, texel.y));
        float nD = NearAmount(uvIn + float2(0.0, texel.y));
        float grad = max(max(abs(nL - nearAmount), abs(nR - nearAmount)),
                         max(abs(nU - nearAmount), abs(nD - nearAmount)));
        float edgeFade = saturate((grad - edgeThreshold) * edgeGuard);
        disp *= 1.0 - edgeFade;
    }
    float2 uv = saturate(float2(uvIn.x + disp, uvIn.y));
    return colorTex.Sample(s0, uv);
}
