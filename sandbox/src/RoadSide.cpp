#include "RoadSide.hpp"

#include <algorithm>
#include <cmath>

#include <glm/gtc/constants.hpp> // glm::pi

namespace roadside {

Line preset(Kind k) {
    Line L;
    L.kind = k;
    switch (k) {
        case Kind::GuardRail:
            // A continuous beam hard against the shoulder. Spacing is a typical
            // rail-segment length; the user matches it to their model.
            L.offset  = 0.6f;
            L.spacing = 4.0f;
            L.lift    = 0.5f;   // beam rides above the ground on its posts
            L.sink    = 0.0f;
            L.side    = Side::Right;
            L.faceRoad = true;
            break;
        case Kind::Curb:
            // A low edging right at the carriageway edge, run end-to-end.
            L.offset  = 0.1f;
            L.spacing = 2.0f;
            L.lift    = 0.0f;
            L.sink    = 0.05f;
            L.side    = Side::Both;
            L.faceRoad = true;
            break;
        case Kind::Post:
            // Discrete markers standing apart -- delineators, bollards, lamps.
            L.offset  = 1.0f;
            L.spacing = 25.0f;
            L.lift    = 0.0f;
            L.sink    = 0.1f;
            L.side    = Side::Right;
            L.faceRoad = false; // a post looks the same from every side
            L.knockable = true; // bowl it over when the car clips it
            L.mass      = 8.0f;
            break;
        case Kind::OnRoad:
            // On the carriageway itself: cones, chicane blocks, arrows. Offset is
            // measured from the centreline, so 0 puts it down the middle.
            L.onRoad  = true;
            L.offset  = 0.0f;
            L.spacing = 20.0f;
            L.lift    = 0.0f;
            L.sink    = 0.0f;   // it stands ON the asphalt, not sunk into it
            L.side    = Side::Right;
            L.faceRoad = false; // "face the road" is meaningless in the middle of it
            // Anything sitting on the driving line will be hit, so let it be hit:
            // a static box there is a wall that stops a car dead.
            L.knockable = true;
            L.mass      = 8.0f;
            break;
    }
    return L;
}

std::vector<Instance> generate(const Line& line, const std::vector<glm::vec2>& center,
                               float halfWidth,
                               const std::function<float(float, float)>& groundAt,
                               const std::vector<float>& surfaceY,
                               const std::vector<float>& bankDeg) {
    std::vector<Instance> out;
    if (!line.enabled || line.model.empty() || center.size() < 2) return out;

    const float step   = std::max(0.25f, line.spacing);
    const float extraY = glm::radians(line.yaw);
    float       next   = 0.0f;   // first station at the road start
    float       walked = 0.0f;
    bool        altLeft = true;  // toggled per station in Alternate mode

    // An on-road line rides the carriageway: its lateral offset starts at the
    // centreline instead of the edge, and its height comes from the road profile
    // (deck top over a bridge, tilted by the cross-fall) rather than the terrain.
    const bool  onRoad = line.onRoad;
    const float lateral0 = onRoad ? 0.0f : halfWidth;
    const bool  haveProfile = onRoad && surfaceY.size() == center.size();
    const bool  haveBank    = haveProfile && bankDeg.size() == center.size();
    // Dead centre: Left and Right would land the same object twice, so keep one.
    const bool  centred = onRoad && std::abs(line.offset) < 1e-3f;

    for (std::size_t i = 1; i < center.size(); ++i) {
        const glm::vec2 a = center[i - 1], b = center[i];
        const float     seg = glm::length(b - a);
        if (seg < 1e-5f) continue;
        const glm::vec2 dir = (b - a) / seg;
        // Left of travel in XZ (matches the ribbon's own winding: cross(up,dir)).
        const glm::vec2 perp(dir.y, -dir.x);
        // Heading along the road, in the same convention the model entities use
        // (yaw about +Y, 0 = facing +Z). atan2(x, z) so +Z maps to yaw 0.
        const float heading = std::atan2(dir.x, dir.y);

        while (next <= walked + seg) {
            const float     u = (next - walked) / seg;   // where in this segment
            const glm::vec2 c = a + dir * (next - walked);
            next += step;

            bool doLeft  = line.side == Side::Left  || line.side == Side::Both ||
                           (line.side == Side::Alternate && altLeft);
            bool doRight = line.side == Side::Right || line.side == Side::Both ||
                           (line.side == Side::Alternate && !altLeft);
            altLeft = !altLeft;
            // On the centreline both sides are the same spot: emit it once, as
            // the left (unflipped) copy.
            if (centred && (doLeft || doRight)) { doLeft = true; doRight = false; }

            for (int s = 0; s < 2; ++s) {
                const bool leftSide = (s == 0);
                if (leftSide ? !doLeft : !doRight) continue;
                // loft() puts the LEFT edge at center - side, right at + side; match
                // it so "Left" here is the ribbon's left.
                const float sign = leftSide ? -1.0f : 1.0f;
                const float across = sign * (lateral0 + line.offset); // + = right
                const glm::vec2 p = c + perp * across;
                float g = groundAt ? groundAt(p.x, p.y) : 0.0f;
                if (haveProfile) {
                    // The road's own surface at this station, then out along the
                    // cross-fall to where the object actually stands. Same sign as
                    // RoadSystem::surfaceHeightAt: a positive bank drops the right
                    // edge, so the surface FALLS across a positive offset.
                    g = glm::mix(surfaceY[i - 1], surfaceY[i], u);
                    const float bk = haveBank
                        ? glm::mix(bankDeg[i - 1], bankDeg[i], u) : 0.0f;
                    // Clamped to the carriageway, exactly as surfaceHeightAt does:
                    // past the edge the tilt would extrapolate off into the air.
                    if (bk != 0.0f)
                        g -= glm::clamp(across, -halfWidth, halfWidth) *
                             std::tan(glm::radians(bk));
                }
                // The far side is rotated 180 so a one-sided profile keeps its
                // face toward the road; a symmetric post opts out via faceRoad.
                float yaw = heading + extraY;
                if (line.faceRoad && !leftSide) yaw += glm::pi<float>();
                // Sink hides the seam of a STATIC object; a knockable one must not
                // start buried, or its dynamic body is ejected (and topples) on the
                // first physics step. So it sits flush and the caller adds a tiny
                // clearance for the collider instead.
                const float base = g + line.lift - (line.knockable ? 0.0f : line.sink);
                out.push_back({glm::vec3(p.x, base, p.y), yaw, line.scale});
            }
        }
        walked += seg;
    }
    return out;
}

} // namespace roadside
