// --- A tree in the wind (needs wind.glsl) ---------------------------------------
//
// Three motions at three speeds, because that is how a tree actually moves and
// one sine for the whole crown is what made the old forest look like it was on
// a turntable:
//   - the trunk bows downwind as the gust reaches it and springs back, slow
//     (under a hertz) and different for every tree;
//   - the branches swing on their own, faster, the further out the more, with a
//     phase that is shared along a branch so it moves as one limb;
//   - the leaves flutter, fast and small, each card on its own.
// The shadow pass calls the same function, so the dappled light under a tree
// moves with the tree.
//
// `lp` is the vertex in the tree's own frame (unit height, base at 0),
// `base` the tree's world position, `scale` its height. Returns the world-space
// offset to add.
vec3 treeWindAt(vec3 lp, vec3 base, float scale, float yaw, bool leaf, float T) {
    float s = uWindStrength;
    if (s <= 0.0) return vec3(0.0);
    float g     = windGustAt(base.xz, T);
    float phase = fract(sin(dot(base.xz, vec2(12.9898, 78.233))) * 43758.5453) * 6.2831853;
    float h     = clamp(lp.y, 0.0, 1.2);
    vec2  dir   = uWindDir;
    vec2  perp  = vec2(-dir.y, dir.x);

    // Trunk: a bow downwind, deepest at the crown (h^2), breathing with the gust.
    float bow  = s * g * (0.55 + 0.45 * sin(T * 0.85 + phase));
    vec3  off  = vec3(dir.x, 0.0, dir.y) * (0.045 * bow * h * h * scale);
    // ...with a little of the sideways circle a real crown traces.
    off.xz += perp * (0.012 * s * g * sin(T * 0.63 + phase * 1.7) * h * h * scale);

    // Branches: out from the trunk, the reach sets the amplitude. The phase is
    // a SMOOTH function of position -- neighbouring vertices of one limb, one
    // leaf card, must move together, or the card tears into a streak (a phase
    // cut into cells did exactly that). A slow wave through the crown reads as
    // limbs swinging one after another.
    // Clamped: a crown is at most about as wide as the tree is tall, and a
    // mesh that lies off its origin must not be flung about by it.
    float reach = min(length(lp.xz), 0.8);
    // Gently varying across the crown (a radian or two from side to side):
    // limbs swing a little after one another, the crown does not ripple.
    float bph   = dot(lp, vec3(1.9, 1.2, 1.7)) + phase;
    float swing = sin(T * 1.6 + bph);
    off.xz += dir * (0.015 * s * g * reach * swing * scale);
    off.y  += 0.008 * s * g * reach * sin(T * 2.1 + bph * 1.3) * scale;

    // Leaves: quicker and small. Still smooth in space at the size of a card
    // (a tenth of the tree), so a card shivers as a card.
    if (leaf) {
        float lph = dot(lp, vec3(9.3, 7.1, 8.9)) + phase;
        off += vec3(sin(T * 4.7 + lph), sin(T * 5.9 + lph * 1.3),
                    cos(T * 4.3 + lph)) * (0.0018 * s * g * (0.3 + h) * scale);
    }
    return off;
}

vec3 treeWind(vec3 lp, vec3 base, float scale, float yaw, bool leaf) {
    return treeWindAt(lp, base, scale, yaw, leaf, uWindTime);
}
