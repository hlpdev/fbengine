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

// How far it's bent at a place, and, in `slope`, how fast that grows each way.
float bentAt(float3 position, float3 place, MovingVertex vertex, out float3 slope)
{
    const float4 wind = vertex.parameter(0);
    const float4 shape = vertex.parameter(1);
    const float high = max(shape.x, 0.001);
    const float up = saturate(position.y / high);
    const float rising = position.y > 0.0 && position.y < high ? 1.0 / high : 0.0;
    const float3 at = position + place;
    // Gusts across it, 13 times a beat, and a flutter of its parts, 45 times, each by where it is.
    const float gustAt = turn * 13.0 / 60.0 * wind.w + shape.y + at.x * 0.9 + at.z * 0.7;
    const float flutterAt = turn * 45.0 / 60.0 * wind.w + shape.y * 2.3 + at.x * 2.1 + at.y * 1.7 +
                            at.z * 2.6;
    const float strength = shape.z * wind.z;
    const float amount = strength * (0.55 + 0.45 * sin(gustAt) + 0.18 * sin(flutterAt));
    const float3 amountSlope = strength * (0.45 * cos(gustAt) * float3(0.9, 0.0, 0.7) +
                                           0.18 * cos(flutterAt) * float3(2.1, 1.7, 2.6));
    slope = amountSlope * up * up + float3(0.0, amount * 2.0 * up * rising, 0.0);
    return amount * up * up;
}

void move(inout float3 position, inout float3 normal, MovingVertex vertex)
{
    const float4 wind = vertex.parameter(0);
    const float high = max(vertex.parameter(1).x, 0.001);
    const float up = saturate(position.y / high);
    const float rising = position.y > 0.0 && position.y < high ? 1.0 / high : 0.0;
    float3 slope;
    const float bent = bentAt(position, vertex.place, vertex, slope);
    const float3 way = float3(wind.x, 0.0, wind.y);
    // Over, not up: what bends goes down a little as it goes along.
    const float drop = 0.5 * bent * bent / high * up;
    const float3 dropSlope =
        slope * (bent * up / high) + float3(0.0, 0.5 * bent * bent / high * rising, 0.0);
    position += way * bent - float3(0.0, drop, 0.0);
    // Its normal goes through the inverse transpose of how the move stretches what's around it,
    // each row of that stretch a moved axis, so a side square to the wind leans back from it and
    // what faces up stays up.
    if (vertex.parameter(2).w != 0.0)
    {
        const float3 stretchX = float3(1.0, 0.0, 0.0) + slope * way.x;
        const float3 stretchY = float3(0.0, 1.0, 0.0) - dropSlope;
        const float3 stretchZ = float3(0.0, 0.0, 1.0) + slope * way.z;
        const float3x3 inverseTransposed = float3x3(
            cross(stretchY, stretchZ), cross(stretchZ, stretchX), cross(stretchX, stretchY));
        normal = normalize(mul(inverseTransposed, normal));
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
