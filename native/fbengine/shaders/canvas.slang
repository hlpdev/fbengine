// 2D's quads: each drawn from its corners, in pixels, read through the address the push constants
// give, six vertices a quad; its color, sRGB bytes, made linear unless the target's pixels are
// data, times its texture's color. A line's quad covers the pixels around it, and keeps, along its
// longer axis, the one nearest it, as shapes.hpp's line picks it, a tie rounding up. A part of a
// texture keeps its uv half a texel inside the part, so filtering never reaches a neighbor's
// texels, and a part tiled has its uv in tiles, each wrapped into the part.

import bindless;

struct Quad
{
    float2 corners[4];
    float4 uv;
    float4 part;
    uint color;
    uint texture;
    uint flags;
    uint spare;
};

struct Canvas
{
    Quad* quads;
    float2 scale;
    uint data;
};

[[vk::push_constant]]
ConstantBuffer<Canvas> canvas;

struct Corner
{
    float4 position : SV_Position;
    // Centroid, so a multisampled pixel at a quad's edge doesn't take its uv from past the edge.
    centroid float2 uv : TEXCOORD0;
    nointerpolation float4 part : PART;
    float4 color : COLOR;
    nointerpolation uint texture : TEXTURE;
    nointerpolation int4 line : LINEPARAMS;
    nointerpolation uint flags : FLAGS;
};

// A quad's flags: it's a line; steeper than long; a part of its texture, left, top, right, and
// bottom in uv; and that part tiled.
static const uint lineFlag = 1;
static const uint steepFlag = 2;
static const uint partFlag = 4;
static const uint tileFlag = 8;

// A quad's two triangles, from its corners clockwise from the top left.
static const uint cornerOf[6] = { 0, 1, 2, 0, 2, 3 };

float toLinear(float value)
{
    return value <= 0.04045 ? value / 12.92 : pow((value + 0.055) / 1.055, 2.4);
}

[shader("vertex")]
Corner vertexMain(uint index : SV_VulkanVertexID)
{
    const Quad quad = canvas.quads[index / 6];
    const uint at = cornerOf[index % 6];
    Corner corner;
    corner.position = float4(quad.corners[at] * canvas.scale - 1.0, 0.0, 1.0);
    corner.uv = float2(at == 1 || at == 2 ? quad.uv.z : quad.uv.x, at >= 2 ? quad.uv.w : quad.uv.y);
    const float4 bytes = float4(quad.color & 0xFF, (quad.color >> 8) & 0xFF, (quad.color >> 16) & 0xFF,
                                quad.color >> 24) / 255.0;
    const float4 decoded = float4(toLinear(bytes.r), toLinear(bytes.g), toLinear(bytes.b), bytes.a);
    corner.color = canvas.data != 0 ? bytes : decoded;
    corner.part = quad.part;
    corner.texture = quad.texture;
    corner.line = asint(quad.uv);
    corner.flags = quad.flags;
    if ((quad.flags & lineFlag) != 0)
    {
        corner.uv = float2(0.0, 0.0);
    }
    return corner;
}

[shader("fragment")]
float4 fragmentMain(Corner corner) : SV_Target
{
    if ((corner.flags & lineFlag) != 0)
    {
        // The line's start along its longer axis, and across, its length, and how far across it
        // goes: this pixel's drawn if it's the one nearest the line in its column or row.
        const int2 pixel = int2(floor(corner.position.xy));
        const bool steep = (corner.flags & steepFlag) != 0;
        const int along = (steep ? pixel.y : pixel.x) - corner.line.x;
        const int length = corner.line.z;
        if (along < 0 || along > length)
        {
            discard;
        }
        int nearest = corner.line.y;
        if (length > 0)
        {
            // Divided unsigned, as Vulkan leaves signed division of negative numbers undefined:
            // 2 length squared more on top, and length less after, it's never negative, and fits
            // for a length up to 32767.
            const uint top = uint(2 * corner.line.w * along + length) + uint(2 * length * length);
            nearest += int(top / uint(2 * length)) - length;
        }
        if ((steep ? pixel.x : pixel.y) != nearest)
        {
            discard;
        }
    }
    const uint texture = corner.texture & 0xFFFFFF;
    const uint sampler = corner.texture >> 24;
    float2 uv = corner.uv;
    float2 across = ddx(corner.uv);
    float2 down = ddy(corner.uv);
    if ((corner.flags & partFlag) != 0)
    {
        const float2 low = corner.part.xy;
        const float2 high = corner.part.zw;
        if ((corner.flags & tileFlag) != 0)
        {
            uv = low + frac(uv) * (high - low);
            across *= high - low;
            down *= high - low;
        }
        const float2 inset = 0.5 / textureSize(texture);
        const float2 middle = (low + high) * 0.5;
        uv = clamp(uv, min(low + inset, middle), max(high - inset, middle));
    }
    return corner.color * sampleTextureGrad(texture, sampler, uv, across, down);
}
