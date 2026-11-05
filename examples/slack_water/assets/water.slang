// The sea: its normals, two tiles of waves scrolled across each other, and its color, how much it
// hides, and its foam, by how deep it is over the ground. sea.fb gives it what it reads:
//
// - Its first parameter: how high the sea is, the time in seconds, half the ground map's width, and
//   how far the sheet goes each way from its middle, in meters.
// - Its second and third: each tile of waves, how many meters across, which way it goes, x and z, and
//   how fast, in meters a second.
// - Its fourth: the deep's color, linear, and 1 in w to still it, a mirror.
// - Its fifth: the shallows' color, linear; its sixth, the foam's, and in w how far below 0 the
//   ground map's red starts.
// - Its first layer: the ground map, over the island, in half floats: red the ground's height, and
//   green what breaks the foam up. Its second: a tile of waves' normals. Each has no alpha, laid over
//   what's under by it, so neither colors the sea.
#define FBE_BENDS_NORMALS

static const float shallows = 2.6;

float smoothFrom(float from, float to, float at)
{
    return smoothstep(from, to, at);
}

// Where on the sea a pixel is, in the world's meters across and along.
float2 placeOf(Surface surface)
{
    const float sheet = surface.parameter(0).w;
    return (surface.uv0 - float2(0.5, 0.5)) * (2.0 * sheet);
}

// A tile of waves' normal at a place, -1 to 1, scrolled by its parameter.
float3 waves(Surface surface, uint which, float2 place)
{
    const float4 tile = surface.parameter(which);
    const float time = surface.parameter(0).y;
    const float2 moved = place + tile.yz * (tile.w * time);
    return surface.texture(1, moved / tile.x).xyz * 2.0 - float3(1.0, 1.0, 1.0);
}

float3 bend(Surface surface)
{
    if (surface.parameter(3).w != 0.0)
    {
        return float3(0.0, 0.0, 1.0);
    }
    const float2 place = placeOf(surface);
    const float3 one = waves(surface, 1, place);
    // The second turned, so the two never line up.
    const float2 turned = float2(place.x * 0.8 - place.y * 0.6, place.x * 0.6 + place.y * 0.8);
    const float3 other = waves(surface, 2, turned);
    return normalize(float3(one.xy + other.xy, one.z * other.z));
}

float4 shade(Surface surface)
{
    const float2 place = placeOf(surface);
    const float half = surface.parameter(0).z;
    const float2 onMap = (place + float2(half, half)) / (2.0 * half);
    float depth = 100.0;
    float broken = 0.5;
    if (all(onMap > float2(0.0, 0.0)) && all(onMap < float2(1.0, 1.0)))
    {
        const float4 ground = surface.texture(0, onMap);
        depth = surface.parameter(0).x - (ground.r - surface.parameter(5).w);
        broken = ground.g;
    }
    if (depth <= 0.0)
    {
        return float4(surface.color.rgb, 0.0);
    }
    const float deep = smoothFrom(0.0, shallows, depth);
    const float hides = smoothFrom(0.0, 0.05, depth) * (0.45 + 0.55 * smoothFrom(0.0, 1.4, depth));
    // At the very edge, and in lines rolling in, which fade as they're out.
    const float edge = 1.0 - smoothFrom(0.0, 0.16, depth);
    const float rolled = depth / 0.7 + surface.parameter(0).y * 0.22;
    const float through = rolled - floor(rolled);
    const float line = (smoothFrom(0.7, 0.92, through) - smoothFrom(0.92, 1.0, through)) *
                       (1.0 - smoothFrom(0.2, 1.1, depth));
    const float foam = smoothFrom(broken - 0.14, broken + 0.14, saturate(edge + line));
    // What the deep's lit as, with the shallows' color in place of its own where it's shallow.
    const float3 lit = surface.light(surface.normal, 0.0);
    const float3 water = surface.color.rgb +
                         (surface.parameter(4).rgb - surface.parameter(3).rgb) * lit * (1.0 - deep);
    const float3 foamed = surface.parameter(5).rgb * lit;
    return float4(lerp(water, foamed, foam), hides + (0.95 - hides) * foam);
}
