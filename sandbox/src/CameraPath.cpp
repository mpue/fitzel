#include "CameraPath.hpp"

#include <algorithm>

void samplePath(const std::vector<CamKey>& k, float time,
                glm::vec3& pos, float& yaw, float& pitch, float& fov) {
    const int n = static_cast<int>(k.size());
    if (n == 0) return;
    if (n == 1 || time <= k.front().t) {
        pos = k.front().pos; yaw = k.front().yaw;
        pitch = k.front().pitch; fov = k.front().fov; return;
    }
    if (time >= k.back().t) {
        pos = k.back().pos; yaw = k.back().yaw;
        pitch = k.back().pitch; fov = k.back().fov; return;
    }
    int i = 0;
    while (i < n - 1 && k[i + 1].t <= time) ++i;
    const float seg = k[i + 1].t - k[i].t;
    const float u   = seg > 1e-6f ? (time - k[i].t) / seg : 0.0f;
    const int i0 = std::max(0, i - 1), i1 = i, i2 = i + 1, i3 = std::min(n - 1, i + 2);
    pos   = catmull(k[i0].pos,   k[i1].pos,   k[i2].pos,   k[i3].pos,   u);
    yaw   = catmull(k[i0].yaw,   k[i1].yaw,   k[i2].yaw,   k[i3].yaw,   u);
    pitch = catmull(k[i0].pitch, k[i1].pitch, k[i2].pitch, k[i3].pitch, u);
    fov   = catmull(k[i0].fov,   k[i1].fov,   k[i2].fov,   k[i3].fov,   u);
}
