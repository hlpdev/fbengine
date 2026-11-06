// Slack Water's ground up close: what its cover map is at a pixel, turf, sand, rock, or trodden, by its
// color, given fine detail of its own, from noise in the world, its albedo varied and its normal
// bumped; rock that's steep mapped three ways, by its normal, so a cliff isn't stretched; all of it fading
// out with distance, where the cover map's own texels are small enough. Layer 0 is the cover map, over
// the terrain's whole, which parameter 0's x says the size of, in meters; its y is where the detail's
// gone, and its z how far it takes to go.

static const float DETAIL_STRENGTH = 1.0;

float hashOf(float2 at)
{
    const float3 p = frac(float3(at.xyx) * 0.1031);
    const float3 q = p + dot(p, p.yzx + 33.33);
    return frac((q.x + q.y) * q.z);
}

// Value noise from 0 to 1, smooth between whole numbers.
float noiseAt(float2 at)
{
    const float2 cell = floor(at);
    const float2 within = at - cell;
    const float2 u = within * within * (3.0 - 2.0 * within);
    const float a = hashOf(cell);
    const float b = hashOf(cell + float2(1.0, 0.0));
    const float c = hashOf(cell + float2(0.0, 1.0));
    const float d = hashOf(cell + float2(1.0, 1.0));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

float layered(float2 at)
{
    return noiseAt(at) * 0.55 + noiseAt(at * 2.13 + 7.1) * 0.3 + noiseAt(at * 4.37 + 3.7) * 0.15;
}

// Rock's detail, mapped three ways in the world by how its normal faces.
float rockAt(float3 world, float3 normal)
{
    float3 weights = pow(abs(normal), float3(4.0, 4.0, 4.0));
    weights /= max(weights.x + weights.y + weights.z, 0.0001);
    const float fromX = layered(world.zy * 2.4);
    const float fromY = layered(world.xz * 2.4);
    const float fromZ = layered(world.xy * 2.4);
    const float base = fromX * weights.x + fromY * weights.y + fromZ * weights.z;
    // Its cracks, the thin dark lines where the noise crosses a half.
    const float crack = 1.0 - saturate(abs(layered(world.xz * 0.9 + world.y * 0.6) - 0.5) * 18.0);
    return base - crack * 0.5;
}

float4 shade(Surface surface)
{
    float4 color = surface.color;
    const float4 given = surface.parameter(0);
    const float side = max(given.x, 1.0);
    const float far = max(given.y, 1.0);
    const float fade = max(given.z, 0.5);
    const float3 world = surface.world();
    const float3 up = normalize(surface.worldNormal());
    // What's covering it, by the cover map's color, whose v runs against z.
    const float3 cover = surface.texture(0, surface.uv0 / side).rgb;
    // By how its colors stand to each other, as light and dark of each are alike: turf's greener than
    // red, sand and trodden ground redder than blue, sand the brighter, and rock as grey as it's blue.
    const float most = max(cover.r, max(cover.g, cover.b));
    const float green = cover.g / max(cover.r, 0.004);
    const float warm = cover.r / max(cover.b, 0.004);
    // What's steep is rock, whatever color it's painted.
    const float steep = saturate((0.8 - up.y) * 3.0);
    const float turf = saturate((green - 1.08) * 6.0) * (1.0 - steep);
    const float sand = saturate((warm - 1.35) * 4.0) * saturate((most - 0.25) * 8.0) * (1.0 - turf) *
                       (1.0 - steep);
    const float rock = max(saturate(1.0 - abs(warm - 1.0) * 3.0) * (1.0 - turf) * (1.0 - sand), steep);
    const float trodden = saturate(1.0 - turf - sand - rock);
    // Each one's detail, as a height, and how far it moves its albedo.
    const float2 flat = world.xz;
    const float blades = noiseAt(flat * float2(31.0, 13.0)) * 0.6 + noiseAt(flat * 6.0 + 2.0) * 0.4;
    const float grains = sin(flat.x * 4.7 + noiseAt(flat * 1.3) * 5.0) * 0.25 + 0.5 +
                         (noiseAt(flat * 45.0) - 0.5) * 0.5;
    const float pebbles = saturate((noiseAt(flat * 7.0 + 11.0) - 0.45) * 4.0) * 0.7 +
                          noiseAt(flat * 23.0) * 0.3;
    const float stone = rockAt(world, up);
    const float height = blades * turf + grains * sand + pebbles * trodden + stone * rock;
    const float albedo = 1.0 + ((blades - 0.5) * 0.7 * turf + (grains - 0.5) * 0.35 * sand +
                                (pebbles - 0.45) * 0.8 * trodden + (stone - 0.4) * 0.8 * rock);
    // Its normal, bumped by how that height changes across the pixel, as the view has it.
    const float3 normal = surface.normal;
    const float3 dpdx = ddx(surface.place);
    const float3 dpdy = ddy(surface.place);
    const float3 r1 = cross(dpdy, normal);
    const float3 r2 = cross(normal, dpdx);
    const float det = dot(dpdx, r1);
    const float3 gradient = sign(det) * (ddx(height) * r1 + ddy(height) * r2) * 0.06;
    const float3 bumped = normalize(abs(det) * normal - gradient);
    const float3 litFlat = surface.light(normal, 0.0);
    const float3 litBumped = surface.light(bumped, 0.0);
    const float lit = (dot(litBumped, float3(0.3, 0.6, 0.1)) + 0.03) / (dot(litFlat, float3(0.3, 0.6, 0.1)) + 0.03);
    const float near = saturate((far - length(surface.place)) / fade) * DETAIL_STRENGTH;
    color.rgb *= lerp(1.0, max(albedo, 0.2) * clamp(lit, 0.55, 1.6), near);
    return color;
}
