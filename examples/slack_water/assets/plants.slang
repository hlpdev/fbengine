// What grows on the island, bent by the wind: grass, shrubs, and trees, each moved in its own space,
// where its base is at 0 and up is +Y, by how high up it is, so its roots stay put. plants.fb gives it
// what it reads:
//
// - Its first parameter: which way the wind goes, x and z, how strong it is, from 0 to 1, and the
//   time in seconds.
// - Its second: how high the plant is, in meters, where its bend's full; which beat it's on, so not
//   every plant sways at once; and how far its top goes at its full bend, in meters.
#define FBE_MOVES_VERTICES

void move(inout float3 position, inout float3 normal, MovingVertex vertex)
{
    const float4 wind = vertex.parameter(0);
    const float4 shape = vertex.parameter(1);
    const float up = saturate(position.y / shape.x);
    // Gusts that go across the plant, and a flutter of its own parts, each by where it is.
    const float gust = 0.55 + 0.45 * sin(wind.w * 1.3 + shape.y + position.x * 0.9 + position.z * 0.7);
    const float flutter = 0.18 * sin(wind.w * 4.7 + shape.y * 2.3 + vertex.uv0.x * 6.2831853);
    const float bend = shape.z * wind.z * up * up * (gust + flutter);
    position.x += wind.x * bend;
    position.z += wind.y * bend;
    // Over, not up: what bends goes down a little as it goes along.
    position.y -= 0.5 * bend * bend / max(shape.x, 0.001) * up;
}

float4 shade(Surface surface)
{
    return surface.color;
}
