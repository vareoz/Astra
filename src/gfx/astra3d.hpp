#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace astra {

struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
};

struct Vec4 {
    float x = 0, y = 0, z = 0, w = 1;
};

struct Mat4 {
    float m[16] = {0};

    static Mat4 identity();
    static Mat4 perspective(float fov_rad, float aspect, float znear, float zfar);
    static Mat4 look_at(Vec3 eye, Vec3 target, Vec3 up);
    static Mat4 rotate_y(float rad);
    static Mat4 rotate_x(float rad);
    static Mat4 translate(float x, float y, float z);
    static Mat4 scale(float s);

    Mat4 operator*(const Mat4& b) const;
    Vec4 mul(const Vec4& v) const;
};

struct Camera3D {
    float yaw = 0.72f;
    float pitch = 0.28f;
    float distance = 4.1f;
    bool wireframe = false;
    bool smooth_shading = true;
};

struct Triangle3D {
    uint32_t i0 = 0, i1 = 0, i2 = 0;
    uint32_t base_color_argb = 0xFF38BDF8;
    uint8_t material_tag = 0; // 0 = solid, 1 = banded planet, 2 = metallic ring
};

struct Mesh3D {
    std::vector<Vec3> vertices;
    std::vector<Vec3> normals; // smooth per-vertex normals
    std::vector<Triangle3D> triangles;

    void recompute_smooth_normals();
};

struct Render3DStats {
    size_t vertices = 0;
    size_t triangles_submitted = 0;
    size_t triangles_rasterized = 0;
};

// Astra3D v2: Zero-dependency native C++20 3D engine with Z-buffer,
// smooth Gouraud/Blinn-Phong per-pixel normal interpolation, procedural
// planetary surface shaders, interactive orbit camera, wireframe mode, and .obj loader.
class Astra3D {
public:
    static Mesh3D create_icosphere(float radius, int subdivisions, uint32_t color_argb, uint8_t mat_tag = 0);
    static Mesh3D create_torus(float major_r, float minor_r, int seg_u, int seg_v, uint32_t color_argb);
    static Mesh3D create_ring(float inner_r, float outer_r, int segments, uint32_t color_argb);
    static Mesh3D create_cube(float half_size, uint32_t color_argb);
    static bool load_obj_file(const std::string& path, Mesh3D& out, uint32_t color_argb = 0xFF38BDF8);

    static Render3DStats render_scene_into(uint32_t* target_argb,
                                           int target_stride,
                                           int dst_x, int dst_y,
                                           int width, int height,
                                           std::string_view scene_spec,
                                           float time_sec = 0.65f,
                                           const Camera3D& cam = Camera3D{});
};

} // namespace astra
