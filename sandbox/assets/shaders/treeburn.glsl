// --- A tree on fire (VegetationSystem::burnTree) ------------------------------------
//
// `burn` runs 0 (green) .. 1 (charcoal) over the fire's life. The leaves go
// first: they brown, glow along the edge of what is burning and drop, a card
// at a time, until by 0.7 the crown is bare. The wood darkens behind them,
// embers creeping over it while the fire lasts, and ends black. The forest
// itself never burns -- its attribute 9 is not fed and reads 0.
//
// A leaf card's key is one random number per triangle (flat, from the
// provoking vertex), from its place in the tree's own frame: the same card
// goes at the same moment in the lit pass, the shadow and the motion vectors.

float treeBurnKey(vec3 local) {
    return fract(sin(dot(floor(local * 37.0), vec3(12.9898, 78.233, 37.719))) * 43758.5453);
}

// How much of the crown has gone by now.
float treeLeavesGone(float burn) { return smoothstep(0.08, 0.7, burn); }

// Has this leaf card dropped?
bool treeLeafBurnt(float key, float burn) {
    return burn > 0.0 && key < treeLeavesGone(burn);
}

// The albedo as the fire leaves it (linear), and the glow on top.
vec3 treeBurnAlbedo(vec3 albedo, bool leaf, float burn) {
    if (burn <= 0.0) return albedo;
    vec3 charcoal = vec3(0.018, 0.016, 0.014);
    if (leaf) {
        vec3 brown = vec3(0.16, 0.07, 0.02);
        return mix(mix(albedo, brown, smoothstep(0.0, 0.25, burn)), charcoal, smoothstep(0.3, 0.7, burn));
    }
    return mix(albedo, charcoal, smoothstep(0.15, 0.85, burn));
}

vec3 treeBurnGlow(vec3 local, float key, bool leaf, float burn, float time) {
    if (burn <= 0.0 || burn >= 1.0) return vec3(0.0);
    vec3 ember = vec3(1.0, 0.32, 0.05);
    float flick = 0.6 + 0.4 * sin(time * 9.0 + key * 40.0);
    if (leaf) {
        // the rim of the fire: cards about to go
        float edge = 1.0 - smoothstep(0.0, 0.12, key - treeLeavesGone(burn));
        return ember * edge * flick * 3.0;
    }
    // embers in the bark, coming and going while it burns
    float spot = fract(sin(dot(floor(local * vec3(18.0, 40.0, 18.0)), vec3(7.13, 157.1, 113.7))) * 43758.5453);
    float live = smoothstep(0.1, 0.3, burn) * (1.0 - smoothstep(0.75, 1.0, burn));
    return ember * step(0.88, spot) * live * flick * 2.0;
}
