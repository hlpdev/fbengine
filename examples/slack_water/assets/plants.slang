// What grows on the island, bent by the wind: grass, shrubs, and trees, each moved in its own
// space, where its base is at 0 and up is +Y, by how high up it is, so its roots stay put; each
// point on its own beat, by where it is, so nothing tears, and each tuft of grass by where it is
// too. plants.fb gives it what it reads:
//
// - Its first parameter: which way the wind goes, x and z; how strong it is, from 0 to 1; and where
//   its beat is, in seconds, from 0 to a beat of 60, in which every wave of it goes round a whole
//   number of times, so it wraps with no jump.
// - Its second: how high the plant is, in meters, where its bend's full; the beat it's on, so not
//   every plant sways at once; how far its top goes at its full bend, in meters; and 1 if light
//   comes through it from behind, as leaves'.
// - Its third: the color that light comes through as, and in w 1 if its normals follow its bend.
#define FBE_MOVES_VERTICES

static const float turn = 6.2831853;

// How far it's bent at a place, and how fast that grows with height.
float2 bentAt(float3 position, float3 place, MovingVertex vertex)
{
    const float4 wind = vertex.parameter(0);
    const float4 shape = vertex.parameter(1);
    const float up = saturate(position.y / shape.x);
    const float3 at = position + place;
    // Gusts across it, 13 times a beat, and a flutter of its parts, 45 times, each by where it is.
    const float gust = 0.55 + 0.45 * sin(turn * 13.0 / 60.0 * wind.w + shape.y + at.x * 0.9 +
                                         at.z * 0.7);
    const float flutter = 0.18 * sin(turn * 45.0 / 60.0 * wind.w + shape.y * 2.3 + at.x * 2.1 +
                                     at.y * 1.7 + at.z * 2.6);
    const float amount = shape.z * wind.z * (gust + flutter);
    return float2(amount * up * up, amount * 2.0 * up / max(shape.x, 0.001));
}

void move(inout float3 position, inout float3 normal, MovingVertex vertex)
{
    const float4 wind = vertex.parameter(0);
    const float4 shape = vertex.parameter(1);
    const float2 bent = bentAt(position, vertex.place, vertex);
    const float up = saturate(position.y / shape.x);
    position.x += wind.x * bent.x;
    position.z += wind.y * bent.x;
    // Over, not up: what bends goes down a little as it goes along.
    position.y -= 0.5 * bent.x * bent.x / max(shape.x, 0.001) * up;
    // Its normal leans back from the way it's bent, by how fast that bend grows up it.
    if (vertex.parameter(2).w != 0.0)
    {
        normal = normalize(normal - float3(wind.x, 0.0, wind.y) * (bent.y * normal.y));
    }
}

float4 shade(Surface surface)
{
    if (surface.parameter(1).w == 0.0)
    {
        return surface.color;
    }
    // Leaves let some of the light behind them through.
    const float3 behind = surface.light(-surface.normal, 0.0) * surface.parameter(2).rgb;
    return float4(surface.color.rgb + behind * 0.6, surface.color.a);
}
