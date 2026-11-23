#include "gfx/astra3d.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>

namespace astra {

namespace {

constexpr float kPi = 3.14159265358979323846f;

float dot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 cross(const Vec3& a, const Vec3& b) {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}

Vec3 normalize(const Vec3& v) {
    float len = std::sqrt(dot(v, v));
    if (len < 1e-6f) return {0, 1, 0};
    return {v.x / len, v.y / len, v.z / len};
}

uint32_t shade_pixel(uint32_t base_argb, uint8_t mat_tag, const Vec3& obj_pos,
                     const Vec3& normal, const Vec3& light_dir,
                     const Vec3& view_dir, const Vec3& half_vec) {
    float r = static_cast<float>((base_argb >> 16) & 0xFF);
    float g = static_cast<float>((base_argb >> 8) & 0xFF);
    float b = static_cast<float>(base_argb & 0xFF);

    // Procedural latitude/atmosphere shader for planet & metallic bands for rings
    if (mat_tag == 1) {
        float lat = std::sin(obj_pos.y * 5.2f + std::sin(obj_pos.x * 3.0f + obj_pos.z * 2.5f) * 0.45f);
        float band = 0.5f + 0.5f * lat;
        r = 24.0f + band * 52.0f;
        g = 115.0f + band * 95.0f;
        b = 185.0f + (1.0f - band) * 65.0f;
        // Polar ice caps
        if (std::abs(obj_pos.y) > 0.78f) {
            float cap = std::clamp((std::abs(obj_pos.y) - 0.78f) * 5.0f, 0.0f, 1.0f);
            r = r * (1.0f - cap) + 235.0f * cap;
            g = g * (1.0f - cap) + 245.0f * cap;
            b = b * (1.0f - cap) + 255.0f * cap;
        }
    } else if (mat_tag == 2) {
        float rad = std::sqrt(obj_pos.x * obj_pos.x + obj_pos.z * obj_pos.z);
        float stripe = 0.65f + 0.35f * std::sin(rad * 26.0f);
        r *= stripe;
        g *= stripe;
        b *= (0.85f + 0.15f * stripe);
    }

    float diffuse = std::max(0.0f, dot(normal, light_dir));
    float ndv = std::clamp(std::abs(dot(normal, view_dir)), 0.0f, 1.0f);
    float rim = std::pow(1.0f - ndv, 2.4f);
    float spec = std::pow(std::max(0.0f, dot(normal, half_vec)), 28.0f) * 0.7f;

    float ambient = 0.16f;
    float lum = ambient + diffuse * 0.84f;
    r = r * lum + rim * 48.0f + spec * 215.0f;
    g = g * lum + rim * 120.0f + spec * 228.0f;
    b = b * lum + rim * 200.0f + spec * 248.0f;

    uint8_t ir = static_cast<uint8_t>(std::clamp(r, 0.0f, 255.0f));
    uint8_t ig = static_cast<uint8_t>(std::clamp(g, 0.0f, 255.0f));
    uint8_t ib = static_cast<uint8_t>(std::clamp(b, 0.0f, 255.0f));
    return 0xFF000000u | (static_cast<uint32_t>(ir) << 16) |
           (static_cast<uint32_t>(ig) << 8) | static_cast<uint32_t>(ib);
}

void append_mesh(Mesh3D& dst, const Mesh3D& src, const Mat4& transform) {
    uint32_t base_v = static_cast<uint32_t>(dst.vertices.size());
    for (size_t i = 0; i < src.vertices.size(); ++i) {
        const auto& v = src.vertices[i];
        Vec4 t = transform.mul({v.x, v.y, v.z, 1.0f});
        dst.vertices.push_back({t.x, t.y, t.z});
        if (i < src.normals.size()) {
            const auto& n = src.normals[i];
            Vec4 tn = transform.mul({n.x, n.y, n.z, 0.0f});
            dst.normals.push_back(normalize({tn.x, tn.y, tn.z}));
        } else {
            dst.normals.push_back({0, 1, 0});
        }
    }
    for (auto tri : src.triangles) {
        tri.i0 += base_v;
        tri.i1 += base_v;
        tri.i2 += base_v;
        dst.triangles.push_back(tri);
    }
}

void draw_line_2d(uint32_t* target, int stride, int dst_x, int dst_y, int w, int h,
                  int x0, int y0, int x1, int y1, uint32_t argb) {
    int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        if (x0 >= 0 && x0 < w && y0 >= 0 && y0 < h) {
            target[(dst_y + y0) * stride + dst_x + x0] = argb;
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

} // namespace

void Mesh3D::recompute_smooth_normals() {
    normals.assign(vertices.size(), {0, 0, 0});
    for (const auto& tri : triangles) {
        const Vec3& a = vertices[tri.i0];
        const Vec3& b = vertices[tri.i1];
        const Vec3& c = vertices[tri.i2];
        Vec3 fn = cross(b - a, c - a);
        normals[tri.i0] = normals[tri.i0] + fn;
        normals[tri.i1] = normals[tri.i1] + fn;
        normals[tri.i2] = normals[tri.i2] + fn;
    }
    for (auto& n : normals) n = normalize(n);
}

Mat4 Mat4::identity() {
    Mat4 r;
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

Mat4 Mat4::perspective(float fov_rad, float aspect, float znear, float zfar) {
    Mat4 r;
    float f = 1.0f / std::tan(fov_rad * 0.5f);
    r.m[0] = f / std::max(0.1f, aspect);
    r.m[5] = f;
    r.m[10] = (zfar + znear) / (znear - zfar);
    r.m[11] = -1.0f;
    r.m[14] = (2.0f * zfar * znear) / (znear - zfar);
    return r;
}

Mat4 Mat4::look_at(Vec3 eye, Vec3 target, Vec3 up) {
    Vec3 f = normalize(target - eye);
    Vec3 s = normalize(cross(f, up));
    Vec3 u = cross(s, f);

    Mat4 r = identity();
    r.m[0] = s.x;  r.m[4] = s.y;  r.m[8]  = s.z;
    r.m[1] = u.x;  r.m[5] = u.y;  r.m[9]  = u.z;
    r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z;
    r.m[12] = -dot(s, eye);
    r.m[13] = -dot(u, eye);
    r.m[14] = dot(f, eye);
    return r;
}

Mat4 Mat4::rotate_y(float rad) {
    Mat4 r = identity();
    float c = std::cos(rad), s = std::sin(rad);
    r.m[0] = c;  r.m[8] = s;
    r.m[2] = -s; r.m[10] = c;
    return r;
}

Mat4 Mat4::rotate_x(float rad) {
    Mat4 r = identity();
    float c = std::cos(rad), s = std::sin(rad);
    r.m[5] = c; r.m[9] = -s;
    r.m[6] = s; r.m[10] = c;
    return r;
}

Mat4 Mat4::translate(float x, float y, float z) {
    Mat4 r = identity();
    r.m[12] = x; r.m[13] = y; r.m[14] = z;
    return r;
}

Mat4 Mat4::scale(float s) {
    Mat4 r = identity();
    r.m[0] = r.m[5] = r.m[10] = s;
    return r;
}

Mat4 Mat4::operator*(const Mat4& b) const {
    Mat4 out;
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += m[k * 4 + row] * b.m[col * 4 + k];
            }
            out.m[col * 4 + row] = sum;
        }
    }
    return out;
}

Vec4 Mat4::mul(const Vec4& v) const {
    return {
        m[0] * v.x + m[4] * v.y + m[8]  * v.z + m[12] * v.w,
        m[1] * v.x + m[5] * v.y + m[9]  * v.z + m[13] * v.w,
        m[2] * v.x + m[6] * v.y + m[10] * v.z + m[14] * v.w,
        m[3] * v.x + m[7] * v.y + m[11] * v.z + m[15] * v.w
    };
}

Mesh3D Astra3D::create_icosphere(float radius, int subdivisions, uint32_t color_argb, uint8_t mat_tag) {
    Mesh3D mesh;
    const float t = (1.0f + std::sqrt(5.0f)) * 0.5f;
    std::vector<Vec3> verts = {
        {-1,  t,  0}, { 1,  t,  0}, {-1, -t,  0}, { 1, -t,  0},
        { 0, -1,  t}, { 0,  1,  t}, { 0, -1, -t}, { 0,  1, -t},
        { t,  0, -1}, { t,  0,  1}, {-t,  0, -1}, {-t,  0,  1}
    };
    for (auto& v : verts) v = normalize(v);
    mesh.vertices = std::move(verts);

    const uint32_t faces[20][3] = {
        {0,11,5},{0,5,1},{0,1,7},{0,7,10},{0,10,11},
        {1,5,9},{5,11,4},{11,10,2},{10,7,6},{7,1,8},
        {3,9,4},{3,4,2},{3,2,6},{3,6,8},{3,8,9},
        {4,9,5},{2,4,11},{6,2,10},{8,6,7},{9,8,1}
    };
    for (const auto& f : faces) {
        mesh.triangles.push_back({f[0], f[1], f[2], color_argb, mat_tag});
    }

    for (int s = 0; s < subdivisions; ++s) {
        std::map<uint64_t, uint32_t> midpoint_cache;
        auto get_mid = [&](uint32_t a, uint32_t b) -> uint32_t {
            uint64_t key = (static_cast<uint64_t>(std::min(a, b)) << 32) | std::max(a, b);
            auto it = midpoint_cache.find(key);
            if (it != midpoint_cache.end()) return it->second;
            Vec3 mid = normalize((mesh.vertices[a] + mesh.vertices[b]) * 0.5f);
            uint32_t idx = static_cast<uint32_t>(mesh.vertices.size());
            mesh.vertices.push_back(mid);
            midpoint_cache[key] = idx;
            return idx;
        };

        std::vector<Triangle3D> next;
        next.reserve(mesh.triangles.size() * 4);
        for (const auto& tri : mesh.triangles) {
            uint32_t ab = get_mid(tri.i0, tri.i1);
            uint32_t bc = get_mid(tri.i1, tri.i2);
            uint32_t ca = get_mid(tri.i2, tri.i0);
            next.push_back({tri.i0, ab, ca, color_argb, mat_tag});
            next.push_back({tri.i1, bc, ab, color_argb, mat_tag});
            next.push_back({tri.i2, ca, bc, color_argb, mat_tag});
            next.push_back({ab, bc, ca, color_argb, mat_tag});
        }
        mesh.triangles = std::move(next);
    }

    mesh.normals = mesh.vertices; // Unit sphere normals
    for (auto& v : mesh.vertices) v = v * radius;
    return mesh;
}

Mesh3D Astra3D::create_torus(float major_r, float minor_r, int seg_u, int seg_v, uint32_t color_argb) {
    Mesh3D mesh;
    for (int i = 0; i < seg_u; ++i) {
        float u = (2.0f * kPi * i) / seg_u;
        float cu = std::cos(u), su = std::sin(u);
        for (int j = 0; j < seg_v; ++j) {
            float v = (2.0f * kPi * j) / seg_v;
            float cv = std::cos(v), sv = std::sin(v);
            float r = major_r + minor_r * cv;
            mesh.vertices.push_back({r * cu, minor_r * sv, r * su});
            mesh.normals.push_back(normalize({cv * cu, sv, cv * su}));
        }
    }
    for (int i = 0; i < seg_u; ++i) {
        int ni = (i + 1) % seg_u;
        for (int j = 0; j < seg_v; ++j) {
            int nj = (j + 1) % seg_v;
            uint32_t a = static_cast<uint32_t>(i * seg_v + j);
            uint32_t b = static_cast<uint32_t>(ni * seg_v + j);
            uint32_t c = static_cast<uint32_t>(ni * seg_v + nj);
            uint32_t d = static_cast<uint32_t>(i * seg_v + nj);
            mesh.triangles.push_back({a, c, b, color_argb, 0});
            mesh.triangles.push_back({a, d, c, color_argb, 0});
        }
    }
    return mesh;
}

Mesh3D Astra3D::create_ring(float inner_r, float outer_r, int segments, uint32_t color_argb) {
    Mesh3D mesh;
    for (int i = 0; i < segments; ++i) {
        float a = (2.0f * kPi * i) / segments;
        float ca = std::cos(a), sa = std::sin(a);
        mesh.vertices.push_back({inner_r * ca, 0.0f, inner_r * sa});
        mesh.normals.push_back({0.0f, 1.0f, 0.0f});
        mesh.vertices.push_back({outer_r * ca, 0.0f, outer_r * sa});
        mesh.normals.push_back({0.0f, 1.0f, 0.0f});
    }
    for (int i = 0; i < segments; ++i) {
        int ni = (i + 1) % segments;
        uint32_t i0 = static_cast<uint32_t>(i * 2);
        uint32_t i1 = i0 + 1;
        uint32_t i2 = static_cast<uint32_t>(ni * 2);
        uint32_t i3 = i2 + 1;
        mesh.triangles.push_back({i0, i1, i3, color_argb, 2});
        mesh.triangles.push_back({i0, i3, i2, color_argb, 2});
        mesh.triangles.push_back({i0, i3, i1, color_argb, 2});
        mesh.triangles.push_back({i0, i2, i3, color_argb, 2});
    }
    return mesh;
}

Mesh3D Astra3D::create_cube(float h, uint32_t color_argb) {
    Mesh3D mesh;
    mesh.vertices = {
        {-h, -h,  h}, { h, -h,  h}, { h,  h,  h}, {-h,  h,  h},
        {-h, -h, -h}, { h, -h, -h}, { h,  h, -h}, {-h,  h, -h}
    };
    const uint32_t idx[12][3] = {
        {0,1,2},{0,2,3}, {5,4,7},{5,7,6},
        {4,0,3},{4,3,7}, {1,5,6},{1,6,2},
        {3,2,6},{3,6,7}, {4,5,1},{4,1,0}
    };
    for (const auto& t : idx) {
        mesh.triangles.push_back({t[0], t[1], t[2], color_argb, 0});
    }
    mesh.recompute_smooth_normals();
    return mesh;
}

bool Astra3D::load_obj_file(const std::string& path, Mesh3D& out, uint32_t color_argb) {
    std::ifstream f(path);
    if (!f) return false;
    out.vertices.clear();
    out.normals.clear();
    out.triangles.clear();

    std::string line;
    while (std::getline(f, line)) {
        std::istringstream iss(line);
        std::string kind;
        iss >> kind;
        if (kind == "v") {
            Vec3 v;
            if (iss >> v.x >> v.y >> v.z) out.vertices.push_back(v);
        } else if (kind == "f") {
            std::vector<uint32_t> face;
            std::string tok;
            while (iss >> tok) {
                size_t slash = tok.find('/');
                int idx = std::atoi((slash == std::string::npos ? tok : tok.substr(0, slash)).c_str());
                if (idx < 0) idx = static_cast<int>(out.vertices.size()) + idx + 1;
                if (idx >= 1 && static_cast<size_t>(idx) <= out.vertices.size()) {
                    face.push_back(static_cast<uint32_t>(idx - 1));
                }
            }
            for (size_t i = 1; i + 1 < face.size(); ++i) {
                out.triangles.push_back({face[0], face[i], face[i + 1], color_argb, 0});
            }
        }
    }
    out.recompute_smooth_normals();
    return !out.vertices.empty() && !out.triangles.empty();
}

Render3DStats Astra3D::render_scene_into(uint32_t* target_argb,
                                         int target_stride,
                                         int dst_x, int dst_y,
                                         int width, int height,
                                         std::string_view scene_spec,
                                         float time_sec,
                                         const Camera3D& cam) {
    Render3DStats stats;
    if (!target_argb || width <= 0 || height <= 0) return stats;

    // 1. Deep space gradient + starfield
    for (int y = 0; y < height; ++y) {
        float t = static_cast<float>(y) / static_cast<float>(std::max(1, height - 1));
        uint8_t br = static_cast<uint8_t>(6 + t * 10);
        uint8_t bg = static_cast<uint8_t>(10 + t * 18);
        uint8_t bb = static_cast<uint8_t>(22 + t * 30);
        uint32_t col = 0xFF000000u | (br << 16) | (bg << 8) | bb;
        uint32_t* row = target_argb + (dst_y + y) * target_stride + dst_x;
        for (int x = 0; x < width; ++x) row[x] = col;
    }

    uint32_t seed = 0x51A7F1EDu;
    for (int i = 0; i < (width * height) / 950; ++i) {
        seed = seed * 1664525u + 1013904223u;
        int sx = static_cast<int>((seed >> 16) % static_cast<uint32_t>(width));
        seed = seed * 1664525u + 1013904223u;
        int sy = static_cast<int>((seed >> 16) % static_cast<uint32_t>(height));
        uint8_t lum = static_cast<uint8_t>(145 + (seed & 0x6F));
        target_argb[(dst_y + sy) * target_stride + dst_x + sx] =
            0xFF000000u | (lum << 16) | (lum << 8) | static_cast<uint8_t>(std::min(255, lum + 24));
    }

    // 2. Build scene geometry
    Mesh3D scene;
    if (scene_spec.size() > 4 && scene_spec.substr(scene_spec.size() - 4) == ".obj") {
        if (!load_obj_file(std::string(scene_spec), scene, 0xFF38BDF8)) {
            scene = create_torus(1.05f, 0.36f, 40, 22, 0xFF38BDF8);
        }
    } else if (scene_spec == "torus") {
        scene = create_torus(1.05f, 0.36f, 44, 24, 0xFF34D399);
    } else if (scene_spec == "cube") {
        scene = create_cube(0.95f, 0xFFF59E0B);
    } else {
        // High-smoothness planetary system with banded atmosphere shader + grooved ring + moon
        Mesh3D planet = create_icosphere(0.95f, 3, 0xFF38BDF8, 1);
        append_mesh(scene, planet, Mat4::identity());

        Mesh3D ring = create_ring(1.26f, 1.82f, 64, 0xFFE6C17A);
        append_mesh(scene, ring, Mat4::rotate_x(0.32f));

        Mesh3D moon = create_icosphere(0.24f, 2, 0xFFE2E8F0, 0);
        Mat4 moon_xform = Mat4::translate(1.88f * std::cos(time_sec * 1.15f),
                                          0.46f,
                                          1.88f * std::sin(time_sec * 1.15f));
        append_mesh(scene, moon, moon_xform);
    }

    stats.vertices = scene.vertices.size();
    stats.triangles_submitted = scene.triangles.size();

    // 3. Transform matrices using interactive Camera3D
    Mat4 model = Mat4::rotate_y(cam.yaw + time_sec * 0.55f) * Mat4::rotate_x(cam.pitch);
    Vec3 eye{0.0f, 1.25f, std::clamp(cam.distance, 2.2f, 10.0f)};
    Mat4 view = Mat4::look_at(eye, {0, 0, 0}, {0, 1, 0});
    Mat4 proj = Mat4::perspective(45.0f * kPi / 180.0f,
                                  static_cast<float>(width) / static_cast<float>(height),
                                  0.1f, 50.0f);
    Mat4 mvp = proj * view * model;

    Vec3 light_dir = normalize({0.68f, 0.72f, 0.58f});
    Vec3 view_dir = normalize(eye);
    Vec3 half_vec = normalize(light_dir + view_dir);

    std::vector<float> zbuf(static_cast<size_t>(width * height), std::numeric_limits<float>::infinity());

    // 4. Per-pixel smooth normal interpolation + procedural shaders + Z-buffer
    for (const auto& tri : scene.triangles) {
        const Vec3& ov0 = scene.vertices[tri.i0];
        const Vec3& ov1 = scene.vertices[tri.i1];
        const Vec3& ov2 = scene.vertices[tri.i2];

        Vec4 w0 = model.mul({ov0.x, ov0.y, ov0.z, 1.0f});
        Vec4 w1 = model.mul({ov1.x, ov1.y, ov1.z, 1.0f});
        Vec4 w2 = model.mul({ov2.x, ov2.y, ov2.z, 1.0f});
        Vec3 p0{w0.x, w0.y, w0.z}, p1{w1.x, w1.y, w1.z}, p2{w2.x, w2.y, w2.z};
        Vec3 flat_normal = normalize(cross(p1 - p0, p2 - p0));

        Vec3 wn0 = flat_normal, wn1 = flat_normal, wn2 = flat_normal;
        if (cam.smooth_shading && tri.i2 < scene.normals.size() && tri.material_tag != 2) {
            Vec4 tn0 = model.mul({scene.normals[tri.i0].x, scene.normals[tri.i0].y, scene.normals[tri.i0].z, 0.0f});
            Vec4 tn1 = model.mul({scene.normals[tri.i1].x, scene.normals[tri.i1].y, scene.normals[tri.i1].z, 0.0f});
            Vec4 tn2 = model.mul({scene.normals[tri.i2].x, scene.normals[tri.i2].y, scene.normals[tri.i2].z, 0.0f});
            wn0 = normalize({tn0.x, tn0.y, tn0.z});
            wn1 = normalize({tn1.x, tn1.y, tn1.z});
            wn2 = normalize({tn2.x, tn2.y, tn2.z});
        }

        Vec4 c0 = mvp.mul({ov0.x, ov0.y, ov0.z, 1.0f});
        Vec4 c1 = mvp.mul({ov1.x, ov1.y, ov1.z, 1.0f});
        Vec4 c2 = mvp.mul({ov2.x, ov2.y, ov2.z, 1.0f});
        if (c0.w <= 0.05f || c1.w <= 0.05f || c2.w <= 0.05f) continue;

        float sx0 = (c0.x / c0.w * 0.5f + 0.5f) * width;
        float sy0 = (1.0f - (c0.y / c0.w * 0.5f + 0.5f)) * height;
        float sz0 = c0.z / c0.w;

        float sx1 = (c1.x / c1.w * 0.5f + 0.5f) * width;
        float sy1 = (1.0f - (c1.y / c1.w * 0.5f + 0.5f)) * height;
        float sz1 = c1.z / c1.w;

        float sx2 = (c2.x / c2.w * 0.5f + 0.5f) * width;
        float sy2 = (1.0f - (c2.y / c2.w * 0.5f + 0.5f)) * height;
        float sz2 = c2.z / c2.w;

        float area = (sx1 - sx0) * (sy2 - sy0) - (sy1 - sy0) * (sx2 - sx0);
        if (std::abs(area) < 0.25f) continue;
        if (area > 0.0f) continue;

        int min_x = std::max(0, static_cast<int>(std::floor(std::min({sx0, sx1, sx2}))));
        int max_x = std::min(width - 1, static_cast<int>(std::ceil(std::max({sx0, sx1, sx2}))));
        int min_y = std::max(0, static_cast<int>(std::floor(std::min({sy0, sy1, sy2}))));
        int max_y = std::min(height - 1, static_cast<int>(std::ceil(std::max({sy0, sy1, sy2}))));

        float inv_area = 1.0f / area;
        bool drew_any = false;

        for (int py = min_y; py <= max_y; ++py) {
            float fy = static_cast<float>(py) + 0.5f;
            for (int px = min_x; px <= max_x; ++px) {
                float fx = static_cast<float>(px) + 0.5f;
                float e0 = (sx1 - sx0) * (fy - sy0) - (sy1 - sy0) * (fx - sx0);
                float e1 = (sx2 - sx1) * (fy - sy1) - (sy2 - sy1) * (fx - sx1);
                float e2 = (sx0 - sx2) * (fy - sy2) - (sy0 - sy2) * (fx - sx2);
                if (e0 <= 0.0f && e1 <= 0.0f && e2 <= 0.0f) {
                    float w_b2 = e0 * inv_area;
                    float w_b0 = e1 * inv_area;
                    float w_b1 = e2 * inv_area;
                    float depth = w_b0 * sz0 + w_b1 * sz1 + w_b2 * sz2;
                    size_t zi = static_cast<size_t>(py * width + px);
                    if (depth < zbuf[zi]) {
                        zbuf[zi] = depth;
                        Vec3 n = normalize(wn0 * w_b0 + wn1 * w_b1 + wn2 * w_b2);
                        Vec3 obj_p = ov0 * w_b0 + ov1 * w_b1 + ov2 * w_b2;
                        target_argb[(dst_y + py) * target_stride + dst_x + px] =
                            shade_pixel(tri.base_color_argb, tri.material_tag, obj_p, n,
                                        light_dir, view_dir, half_vec);
                        drew_any = true;
                    }
                }
            }
        }
        if (drew_any) {
            ++stats.triangles_rasterized;
            if (cam.wireframe) {
                draw_line_2d(target_argb, target_stride, dst_x, dst_y, width, height,
                             static_cast<int>(sx0), static_cast<int>(sy0),
                             static_cast<int>(sx1), static_cast<int>(sy1), 0xFF7DD3FCu);
                draw_line_2d(target_argb, target_stride, dst_x, dst_y, width, height,
                             static_cast<int>(sx1), static_cast<int>(sy1),
                             static_cast<int>(sx2), static_cast<int>(sy2), 0xFF7DD3FCu);
                draw_line_2d(target_argb, target_stride, dst_x, dst_y, width, height,
                             static_cast<int>(sx2), static_cast<int>(sy2),
                             static_cast<int>(sx0), static_cast<int>(sy0), 0xFF7DD3FCu);
            }
        }
    }

    return stats;
}

} // namespace astra
