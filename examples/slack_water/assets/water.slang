// The sea: its normals, two tiles of waves scrolled across each other, and its color, how much it
// hides, and its foam, by how deep it is over the ground. sea.fb gives it what it reads:
//
// - Its first parameter: how high the sea is, the time in seconds, half the ground map's width, and
//   how far the sheet goes each way from its middle, in meters.
// - Its second and third: each tile of waves, how many meters across; where it's sampled from, in
//   tiles, which sea.fb moves so the first's waves go along (0.92, 0.39) in the world and the
//   second's along (-0.45, 0.89), each wrapped to a tile so it never jumps; and how much of its slopes
//   are kept, 1 but to see the other alone. The second's sampled turned by `turned` below, and its
//   slopes turned back, so they're the world's.
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

// A place turned by the second tile's turn, of 36.87 degrees, so the tiles never line up.
float2 turned(float2 place)
{
    return float2(place.x * 0.8 - place.y * 0.6, place.x * 0.6 + place.y * 0.8);
}

// Slopes in the turned frame, back in the world's.
float2 turnedBack(float2 slopes)
{
    return float2(slopes.x * 0.8 + slopes.y * 0.6, -slopes.x * 0.6 + slopes.y * 0.8);
}

// A tile of waves' normal at a place, -1 to 1, from where its parameter has it sampled.
float3 waves(Surface surface, uint which, float2 place)
{
    const float4 tile = surface.parameter(which);
    return surface.texture(1, place / tile.x + tile.yz).xyz * 2.0 - float3(1.0, 1.0, 1.0);
}

float3 bend(Surface surface)
{
    if (surface.parameter(3).w != 0.0)
    {
        return float3(0.0, 0.0, 1.0);
    }
    const float2 place = placeOf(surface);
    const float3 one = waves(surface, 1, place);
    const float3 other = waves(surface, 2, turned(place));
    const float kept = surface.parameter(1).w;
    const float otherKept = surface.parameter(2).w;
    const float2 slopes = one.xy * kept + turnedBack(other.xy) * otherKept;
    return normalize(float3(slopes, lerp(1.0, one.z, kept) * lerp(1.0, other.z, otherKept)));
}

// A number from 0 to 1 for a lattice point.
float hash(float2 at)
{
    return frac(sin(dot(at, float2(127.1, 311.7))) * 43758.5453);
}

// Value noise, smooth between whole numbers, from 0 to 1.
float noise(float2 at)
{
    const float2 whole = floor(at);
    float2 part = at - whole;
    part = part * part * (3.0 - 2.0 * part);
    const float a = hash(whole);
    const float b = hash(whole + float2(1.0, 0.0));
    const float c = hash(whole + float2(0.0, 1.0));
    const float d = hash(whole + float2(1.0, 1.0));
    return lerp(lerp(a, b, part.x), lerp(c, d, part.x), part.y);
}

float4 shade(Surface surface)
{
    const float2 place = placeOf(surface);
    const float time = surface.parameter(0).y;
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
    // Froth: patches that drift, of two sizes, and what the ground map breaks it up by.
    const float froth = 0.45 * noise(place * 1.7 + float2(time * 0.07, 0.0)) +
                        0.3 * noise(place * 5.3 - float2(0.0, time * 0.13)) + 0.25 * broken;
    // At the edge, in patches with gaps between, less of it as it's deeper.
    const float edge = 1.0 - smoothFrom(0.0, 0.22, depth);
    const float edgeFoam = edge * smoothFrom(0.42 + 0.25 * (1.0 - edge), 0.58 + 0.25 * (1.0 - edge), froth);
    // Lines rolling in: streaks of froth along each, thinner the older it is, the shallower it's come.
    const float rolled = depth / 0.7 + time * 0.22;
    const float through = rolled - floor(rolled);
    const float line = (smoothFrom(0.72, 0.92, through) - smoothFrom(0.92, 1.0, through)) *
                       (1.0 - smoothFrom(0.25, 1.1, depth));
    const float age = 1.0 - smoothFrom(0.1, 1.0, depth);
    const float streaks = noise(place * float2(0.8, 3.1) + float2(time * 0.05, 0.0));
    const float lineFoam = line * smoothFrom(0.35 + 0.4 * age, 0.6 + 0.4 * age, streaks * 0.7 + froth * 0.3);
    const float foam = saturate(edgeFoam + lineFoam);
    // What the deep's lit as, with the shallows' color in place of its own where it's shallow.
    const float3 lit = surface.light(surface.normal, 0.0);
    const float3 water = surface.color.rgb +
                         (surface.parameter(4).rgb - surface.parameter(3).rgb) * lit * (1.0 - deep);
    const float3 foamed = surface.parameter(5).rgb * lit;
    return float4(lerp(water, foamed, foam), hides + (0.92 - hides) * foam);
}
