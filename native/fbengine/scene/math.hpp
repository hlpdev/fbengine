#pragma once

// The scene's math, in std.math's space: right-handed, Y up, -Z forward, radians, column vectors,
// and matrices column-major, as std.math's Mat4 and the GPU have them. A quaternion q * v turns v,
// and a * b turns by b, then by a.

#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>

namespace fbe::scene {

struct vec3_t {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;

    constexpr vec3_t() = default;
    constexpr vec3_t(float x_value, float y_value, float z_value)
        : x(x_value), y(y_value), z(z_value) {}
};

struct quat_t {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    float w = 1.0F;

    constexpr quat_t() = default;
    constexpr quat_t(float x_value, float y_value, float z_value, float w_value)
        : x(x_value), y(y_value), z(z_value), w(w_value) {}
};

/// A 4x4 matrix: its columns are the axes it turns and scales, then the origin.
struct mat4_t {
    std::array<float, 16> m{};

    /// The matrix that changes nothing.
    constexpr mat4_t() {
        at(0, 0) = 1.0F;
        at(1, 1) = 1.0F;
        at(2, 2) = 1.0F;
        at(3, 3) = 1.0F;
    }

    /// The matrix of three axes and an origin, which moves, turns, and scales.
    constexpr mat4_t(vec3_t x, vec3_t y, vec3_t z, vec3_t origin) : mat4_t() {
        set_axis(0, x);
        set_axis(1, y);
        set_axis(2, z);
        set_axis(3, origin);
    }

    [[nodiscard]] constexpr float at(int column, int row) const {
        return m[index(column, row)];
    }
    constexpr float& at(int column, int row) {
        return m[index(column, row)];
    }
    [[nodiscard]] constexpr vec3_t axis(int column) const {
        return { at(column, 0), at(column, 1), at(column, 2) };
    }

private:
    static constexpr std::size_t index(int column, int row) {
        return (static_cast<std::size_t>(column) * 4) + static_cast<std::size_t>(row);
    }

    constexpr void set_axis(int column, vec3_t v) {
        at(column, 0) = v.x;
        at(column, 1) = v.y;
        at(column, 2) = v.z;
    }
};

inline vec3_t operator+(vec3_t a, vec3_t b) {
    return { a.x + b.x, a.y + b.y, a.z + b.z };
}
inline vec3_t operator-(vec3_t a, vec3_t b) {
    return { a.x - b.x, a.y - b.y, a.z - b.z };
}
inline vec3_t operator-(vec3_t a) {
    return { -a.x, -a.y, -a.z };
}
inline vec3_t operator*(vec3_t a, float s) {
    return { a.x * s, a.y * s, a.z * s };
}
inline vec3_t operator*(vec3_t a, vec3_t b) {
    return { a.x * b.x, a.y * b.y, a.z * b.z };
}

inline float dot(vec3_t a, vec3_t b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}
inline vec3_t cross(vec3_t a, vec3_t b) {
    const float x = (a.y * b.z) - (a.z * b.y);
    const float y = (a.z * b.x) - (a.x * b.z);
    const float z = (a.x * b.y) - (a.y * b.x);
    return { x, y, z };
}
inline float length(vec3_t v) {
    return std::sqrt(dot(v, v));
}
/// The direction of `v`, or zero for a zero `v`.
inline vec3_t normalize(vec3_t v) {
    const float size = length(v);
    return size > 0.0F ? v * (1.0F / size) : vec3_t();
}
/// `a` over `b` per component, 0 where `b` is 0.
inline vec3_t divide(vec3_t a, vec3_t b) {
    const float x = b.x != 0.0F ? a.x / b.x : 0.0F;
    const float y = b.y != 0.0F ? a.y / b.y : 0.0F;
    const float z = b.z != 0.0F ? a.z / b.z : 0.0F;
    return { x, y, z };
}

inline quat_t operator*(quat_t a, quat_t b) {
    const float x = (a.w * b.x) + (a.x * b.w) + (a.y * b.z) - (a.z * b.y);
    const float y = (a.w * b.y) - (a.x * b.z) + (a.y * b.w) + (a.z * b.x);
    const float z = (a.w * b.z) + (a.x * b.y) - (a.y * b.x) + (a.z * b.w);
    const float w = (a.w * b.w) - (a.x * b.x) - (a.y * b.y) - (a.z * b.z);
    return { x, y, z, w };
}
inline vec3_t operator*(quat_t q, vec3_t v) {
    const vec3_t axis(q.x, q.y, q.z);
    return v * ((q.w * q.w) - dot(axis, axis)) + axis * (2.0F * dot(v, axis)) +
           cross(axis, v) * (2.0F * q.w);
}
inline quat_t inverse(quat_t q) {
    return { -q.x, -q.y, -q.z, q.w };
}
/// `q` at length 1, or no rotation for a zero `q`.
inline quat_t normalize(quat_t q) {
    const float size = std::sqrt((q.x * q.x) + (q.y * q.y) + (q.z * q.z) + (q.w * q.w));
    if (!(size > 0.0F) || !std::isfinite(size)) {
        return {};
    }
    const float scale = 1.0F / size;
    return { q.x * scale, q.y * scale, q.z * scale, q.w * scale };
}
inline quat_t axis_angle(vec3_t axis, float angle) {
    const vec3_t unit = normalize(axis);
    const float sine = std::sin(angle / 2.0F);
    return { unit.x * sine, unit.y * sine, unit.z * sine, std::cos(angle / 2.0F) };
}
/// std.math's Quat.FromEuler: `z` about Z, then `x` about X, then `y` about Y.
inline quat_t euler(float x, float y, float z) {
    const quat_t yaw(0.0F, std::sin(y / 2.0F), 0.0F, std::cos(y / 2.0F));
    const quat_t pitch(std::sin(x / 2.0F), 0.0F, 0.0F, std::cos(x / 2.0F));
    const quat_t roll(0.0F, 0.0F, std::sin(z / 2.0F), std::cos(z / 2.0F));
    return yaw * pitch * roll;
}
/// The shortest rotation turning the direction `from` to the direction `to`; opposite ones turn
/// half a turn about an axis at right angles to them, and a zero one is no rotation.
inline quat_t from_to(vec3_t from, vec3_t to) {
    const vec3_t start = normalize(from);
    const vec3_t finish = normalize(to);
    if (dot(start, start) == 0.0F || dot(finish, finish) == 0.0F) {
        return {};
    }
    const float cosine = dot(start, finish);
    if (cosine < -1.0F + 1e-6F) {
        vec3_t axis = cross({ 1.0F, 0.0F, 0.0F }, start);
        if (dot(axis, axis) < 1e-6F) {
            axis = cross({ 0.0F, 1.0F, 0.0F }, start);
        }
        return axis_angle(axis, std::numbers::pi_v<float>);
    }
    const vec3_t axis = cross(start, finish);
    return normalize(quat_t(axis.x, axis.y, axis.z, 1.0F + cosine));
}
/// Part of the way from `a` to `b`, the short way round.
inline quat_t slerp(quat_t a, quat_t b, float t) {
    float cosine = (a.x * b.x) + (a.y * b.y) + (a.z * b.z) + (a.w * b.w);
    if (cosine < 0.0F) {
        b = quat_t(-b.x, -b.y, -b.z, -b.w);
        cosine = -cosine;
    }
    if (cosine > 0.9995F) {
        return normalize(quat_t(a.x + ((b.x - a.x) * t), a.y + ((b.y - a.y) * t),
                                a.z + ((b.z - a.z) * t), a.w + ((b.w - a.w) * t)));
    }
    const float angle = std::acos(cosine);
    const float sine = std::sin(angle);
    const float from = std::sin((1.0F - t) * angle) / sine;
    const float to = std::sin(t * angle) / sine;
    const float x = (a.x * from) + (b.x * to);
    const float y = (a.y * from) + (b.y * to);
    const float z = (a.z * from) + (b.z * to);
    const float w = (a.w * from) + (b.w * to);
    return { x, y, z, w };
}

/// Scales, then turns, then moves, as an entity's transform does.
inline mat4_t compose(vec3_t position, quat_t rotation, vec3_t scale) {
    return { rotation * vec3_t(scale.x, 0.0F, 0.0F), rotation * vec3_t(0.0F, scale.y, 0.0F),
             rotation * vec3_t(0.0F, 0.0F, scale.z), position };
}

inline mat4_t operator*(const mat4_t& a, const mat4_t& b) {
    mat4_t product;
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0F;
            for (int k = 0; k < 4; ++k) {
                sum += a.at(k, row) * b.at(column, k);
            }
            product.at(column, row) = sum;
        }
    }
    return product;
}

inline vec3_t transform_point(const mat4_t& m, vec3_t p) {
    return m.axis(0) * p.x + m.axis(1) * p.y + m.axis(2) * p.z + m.axis(3);
}
inline vec3_t transform_vector(const mat4_t& m, vec3_t v) {
    return m.axis(0) * v.x + m.axis(1) * v.y + m.axis(2) * v.z;
}

/// The inverse of a matrix that turns, scales, and moves, which leaves its bottom row alone; false
/// when it scales an axis to nothing, so it has none.
inline bool inverse_affine(const mat4_t& m, mat4_t& inverse) {
    const vec3_t a = m.axis(0);
    const vec3_t b = m.axis(1);
    const vec3_t c = m.axis(2);
    // The rows of the 3x3's inverse are the columns' cross products over the determinant.
    const vec3_t row0 = cross(b, c);
    const float determinant = dot(a, row0);
    if (!std::isnormal(determinant)) {
        return false;
    }
    const float scale = 1.0F / determinant;
    const vec3_t r0 = row0 * scale;
    const vec3_t r1 = cross(c, a) * scale;
    const vec3_t r2 = cross(a, b) * scale;
    const vec3_t t = m.axis(3);
    inverse = mat4_t(vec3_t(r0.x, r1.x, r2.x), vec3_t(r0.y, r1.y, r2.y), vec3_t(r0.z, r1.z, r2.z),
                     vec3_t(-dot(r0, t), -dot(r1, t), -dot(r2, t)));
    return true;
}

/// A normal turned by the matrix: through its 3x3's cofactor, the inverse transpose times the
/// determinant, which a matrix that flattens an axis has too, then made length 1.
inline vec3_t transform_normal(const mat4_t& m, vec3_t n) {
    const vec3_t a = m.axis(0);
    const vec3_t b = m.axis(1);
    const vec3_t c = m.axis(2);
    return normalize(cross(b, c) * n.x + cross(c, a) * n.y + cross(a, b) * n.z);
}

} // namespace fbe::scene
