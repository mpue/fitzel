// --- The clouds' shadow on the ground (see CloudShadow.hpp) ---------------------
//
// A map of how much sunlight gets through the cumulus deck, rendered once a
// frame from the sky's own cloud density for the ground plane at uCloudRef.y
// and looked up here for any point: the point is slid along the sun's ray down
// to that plane, which is the same ray, so a summit two kilometres up is shaded
// by exactly the cloud that shades it -- not the one above it.
//
// Uniforms (applyCloudShadow in FrameRender.hpp):
uniform sampler2D uCloudShadow;
uniform int   uCloudShadowOn;     // 0 = no clouds cast anything this frame
uniform vec4  uCloudRect;         // xy = map origin (world XZ), z = 1/size, w = plane height
uniform vec3  uCloudSun;          // towards the sun

// 1 = full sun, less under a cloud.
float cloudLight(vec3 wp) {
    if (uCloudShadowOn == 0 || uCloudSun.y < 0.03) return 1.0;
    vec2 q  = wp.xz - uCloudSun.xz * ((wp.y - uCloudRect.w) / uCloudSun.y);
    vec2 uv = (q - uCloudRect.xy) * uCloudRect.z;
    // Towards the map's edge the shadow thins out rather than stopping on a line.
    vec2 e  = min(uv, 1.0 - uv);
    float edge = smoothstep(0.0, 0.12, min(e.x, e.y));
    if (edge <= 0.0) return 1.0;
    return mix(1.0, texture(uCloudShadow, uv).r, edge);
}
