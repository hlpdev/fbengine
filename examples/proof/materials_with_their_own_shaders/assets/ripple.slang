// Vertices moved by a wave, up by its height, and colored by how far up they went. The parameter is
// its height, how many waves there are across its UVs, and how far along they've gone.
#define FBE_MOVES_VERTICES

void move(inout float3 position, inout float3 normal, MovingVertex vertex)
{
    const float4 wave = vertex.parameter(0);
    position.y += wave.x * sin(vertex.uv0.x * wave.y + wave.z);
}

float4 shade(Surface surface)
{
    return float4(surface.parameter(1).rgb * surface.light(surface.normal, 0.0), 1.0);
}
