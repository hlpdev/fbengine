// The sky, behind everything opaque a camera drew: one triangle over its viewport at the far plane,
// so only what's still at the far plane takes it. Each pixel looks along its ray, from the camera's
// turn and how its view spreads, into an equirectangular texture, or through the atmosphere: one
// scattering of the sun's light, Rayleigh's and Mie's, marched along the ray, in linear light.

import bindless;

struct Sky
{
    // The view's axes in the world, and its half-size at a distance of 1, 0 for an orthographic one.
    float4 right;
    float4 up;
    float4 forward;
    float2 spread;
    // 1 for the atmosphere, 2 for a texture.
    uint mode;
    uint texture;
    // Which way the sun is, and how bright, in the world.
    float3 sun;
    float brightness;
    uint sampler;
    // Whether it's mirrored across, as a reflection's view is.
    float flip;
    // The planes of mirrors whose reflections show instead of it: a normal and the eye's height off it.
    float4* mirrors;
    uint mirrorCount;
    uint spare;
};

struct Push
{
    Sky* sky;
};

[[vk::push_constant]]
ConstantBuffer<Push> push;

struct Corner
{
    float4 position : SV_Position;
    float2 across : ACROSS;
};

[shader("vertex")]
Corner vertexMain(uint index : SV_VulkanVertexID)
{
    const float2 at = float2(float((index << 1) & 2), float(index & 2)) * 2.0 - float2(1.0, 1.0);
    Corner corner;
    corner.position = float4(at, 0.0, 1.0);
    // Clip space's y is down the screen.
    corner.across = float2(at.x, -at.y);
    return corner;
}

static const float pi = 3.14159265358979;
static const float groundRadius = 6360000.0;
static const float airRadius = 6420000.0;
static const float rayleighHeight = 7994.0;
static const float mieHeight = 1200.0;
static const float3 rayleighScatter = float3(5.8e-6, 13.5e-6, 33.1e-6);
static const float mieScatter = 21e-6;
static const float mieShape = 0.76;

// How far along a ray from a place to where it leaves a sphere about the world's middle.
float leaving(float3 place, float3 way, float radius)
{
    const float along = dot(place, way);
    const float left = along * along - dot(place, place) + radius * radius;
    return left > 0.0 ? -along + sqrt(left) : 0.0;
}

// How far along a ray from a place the ground is, or a negative for never.
float toGround(float3 place, float3 way)
{
    const float along = dot(place, way);
    const float left = along * along - dot(place, place) + groundRadius * groundRadius;
    return along < 0.0 && left > 0.0 ? -along - sqrt(left) : -1.0;
}

float3 atmosphere(Sky* sky, float3 way)
{
    const float3 eye = float3(0.0, groundRadius + 1000.0, 0.0);
    // Below the horizon, as far as the ground.
    const float ground = toGround(eye, way);
    const float travel = ground >= 0.0 ? ground : leaving(eye, way, airRadius);
    const uint steps = 8;
    const uint sunSteps = 4;
    const float length = travel / float(steps);
    float3 rayleigh = float3(0.0, 0.0, 0.0);
    float3 mie = float3(0.0, 0.0, 0.0);
    float rayleighDepth = 0.0;
    float mieDepth = 0.0;
    for (uint i = 0; i < steps; ++i)
    {
        const float3 place = eye + way * ((float(i) + 0.5) * length);
        const float height = sqrt(dot(place, place)) - groundRadius;
        const float thinR = exp(-max(height, 0.0) / rayleighHeight) * length;
        const float thinM = exp(-max(height, 0.0) / mieHeight) * length;
        rayleighDepth += thinR;
        mieDepth += thinM;
        // Toward the sun from there, unless the ground's in the way.
        if (toGround(place, sky.sun) >= 0.0)
        {
            continue;
        }
        const float sunTravel = leaving(place, sky.sun, airRadius);
        const float sunLength = sunTravel / float(sunSteps);
        float sunR = 0.0;
        float sunM = 0.0;
        for (uint j = 0; j < sunSteps; ++j)
        {
            const float3 up = place + sky.sun * ((float(j) + 0.5) * sunLength);
            const float above = sqrt(dot(up, up)) - groundRadius;
            sunR += exp(-max(above, 0.0) / rayleighHeight) * sunLength;
            sunM += exp(-max(above, 0.0) / mieHeight) * sunLength;
        }
        const float3 tau = rayleighScatter * (rayleighDepth + sunR) +
                           mieScatter * 1.1 * float3(mieDepth + sunM, mieDepth + sunM, mieDepth + sunM);
        const float3 through = float3(exp(-tau.x), exp(-tau.y), exp(-tau.z));
        rayleigh += through * thinR;
        mie += through * thinM;
    }
    const float cosine = dot(way, sky.sun);
    const float phaseR = 3.0 / (16.0 * pi) * (1.0 + cosine * cosine);
    const float g = mieShape;
    const float phaseM = 3.0 / (8.0 * pi) * ((1.0 - g * g) * (1.0 + cosine * cosine)) /
                         ((2.0 + g * g) * pow(max(1.0 + g * g - 2.0 * g * cosine, 0.0001), 1.5));
    return (rayleigh * rayleighScatter * phaseR + mie * mieScatter * phaseM) * sky.brightness;
}

[shader("fragment")]
float4 fragmentMain(Corner corner) : SV_Target
{
    Sky* sky = push.sky;
    const float2 spread = sky.spread * corner.across;
    const float3 toward = sky.right.xyz * (spread.x * sky.flip) + sky.up.xyz * spread.y + sky.forward.xyz;
    const float3 way = normalize(toward);
    for (uint i = 0; i < sky.mirrorCount; ++i)
    {
        const float4 mirror = sky.mirrors[i];
        if (mirror.w * dot(mirror.xyz, way) < 0.0)
        {
            discard;
        }
    }
    if (sky.mode == 2)
    {
        const float longitude = atan2(way.x, -way.z) / (2.0 * pi) + 0.5;
        const float latitude = 0.5 - asin(clamp(way.y, -1.0, 1.0)) / pi;
        // By its first level alone: a derivative across the seam at the back of the world would take
        // a far coarser one for its pixels' row.
        const float3 color = sampleTextureGrad(sky.texture, sky.sampler, float2(longitude, latitude),
                                               float2(0.0, 0.0), float2(0.0, 0.0)).rgb;
        return float4(color * sky.brightness, 1.0);
    }
    return float4(atmosphere(sky, way), 1.0);
}
