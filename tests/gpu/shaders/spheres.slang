// A grid of spheres seen from straight ahead, shaded by pbr.slang with nothing but a triangle and
// each pixel's own normal, which is how its tests see a material before there's a mesh: x right, y
// up, z toward the viewer. Metallic goes up its rows, downward, and roughness across its columns,
// unless they're given.

import pbr;

struct Spheres
{
    Environment environment;
    /// The light's direction, unit length, and whether there's an environment to read.
    float4 toLight;
    /// What the light gives, and the normal map's tangent's w: +X its tangent, 0 for no normal map,
    /// 2 for a tangent of no length, and 3 for one whose x is the environment's intensity.
    float4 radiance;
    /// The base color, and the metallic if it isn't below 0.
    float4 baseColor;
    /// The roughness if it isn't below 0, the columns, and the rows; and the occlusion.
    float4 grid;
    /// A normal map's texel, and its scale.
    float4 texel;
    /// What it emits, and how many pixels across the target is.
    float4 emissive;
};

[[vk::push_constant]]
ConstantBuffer<Spheres> spheres;

[shader("vertex")]
float4 vertexMain(uint index : SV_VulkanVertexID) : SV_Position
{
    const float2 at = float2(float((index << 1) & 2), float(index & 2));
    return float4((at * 2.0) - 1.0, 0.0, 1.0);
}

[shader("fragment")]
float4 fragmentMain(float4 position : SV_Position) : SV_Target
{
    const float2 count = spheres.grid.yz;
    const float2 cell = position.xy * count / spheres.emissive.w;
    const float2 which = floor(cell);
    const float2 from = ((cell - which) * 2.0) - 1.0;
    const float squared = dot(from, from);
    if (squared >= 1.0)
    {
        return float4(0.0);
    }
    Surface surface;
    surface.baseColor = spheres.baseColor.rgb;
    surface.metallic = spheres.baseColor.a >= 0.0 ? spheres.baseColor.a : which.y / max(count.y - 1.0, 1.0);
    surface.roughness = spheres.grid.x >= 0.0 ? spheres.grid.x : which.x / max(count.x - 1.0, 1.0);
    surface.normal = normalize(float3(from.x, -from.y, sqrt(1.0 - squared)));
    surface.occlusion = spheres.grid.w;
    surface.emissive = spheres.emissive.rgb;
    if (spheres.radiance.w != 0.0)
    {
        float4 tangent = spheres.radiance.w == 2.0 ? float4(0.0, 0.0, 0.0, 1.0) : float4(1.0, 0.0, 0.0, spheres.radiance.w);
        if (spheres.radiance.w == 3.0)
        {
            tangent = float4(spheres.environment.intensity, 0.0, 0.0, 1.0);
        }
        surface.normal = normalFromMap(surface.normal, tangent, spheres.texel.xyz, spheres.texel.w);
    }
    const float3 view = float3(0.0, 0.0, 1.0);
    float3 color = surface.emissive + shadePunctual(surface, view, spheres.toLight.xyz, spheres.radiance.rgb);
    if (spheres.toLight.w > 0.0)
    {
        color += shadeEnvironment(surface, view, spheres.environment);
    }
    return float4(color, 1.0);
}
