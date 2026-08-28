struct FragmentIn
{
    float4 position : SV_Position;
    float3 color : COLOR0;
};

float4 main(FragmentIn input) : SV_Target0
{
    return float4(input.color, 1.0);
}
