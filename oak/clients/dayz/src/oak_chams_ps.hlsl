cbuffer OakChamsCB : register(b0)
{
    float4 oak_color;
};

float4 main() : SV_Target
{
    return oak_color;
}
