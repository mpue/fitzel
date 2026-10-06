// Which face of a cube map a direction lands on, and where on that face: the
// selection a samplerCube makes by itself, written out (GL spec 8.13, "Cube Map
// Texture Selection"). For the browser's point shadows, whose six faces are the
// layers of a 2D array (lit.frag, FITZEL_WEB). xy = (s, t) in [0, 1], z = the
// face in GL's order: +X, -X, +Y, -Y, +Z, -Z.
//
// tools/cubefacecheck.cpp holds this against the driver's own cube lookup.
vec3 cubeFaceUv(vec3 d) {
    vec3  a = abs(d);
    float face, ma;
    vec2  st;
    if (a.x >= a.y && a.x >= a.z) {
        ma = a.x;
        face = d.x > 0.0 ? 0.0 : 1.0;
        st = d.x > 0.0 ? vec2(-d.z, -d.y) : vec2(d.z, -d.y);
    } else if (a.y >= a.z) {
        ma = a.y;
        face = d.y > 0.0 ? 2.0 : 3.0;
        st = d.y > 0.0 ? vec2(d.x, d.z) : vec2(d.x, -d.z);
    } else {
        ma = a.z;
        face = d.z > 0.0 ? 4.0 : 5.0;
        st = d.z > 0.0 ? vec2(d.x, -d.y) : vec2(-d.x, -d.y);
    }
    return vec3(0.5 * (st / max(ma, 1e-6) + 1.0), face);
}
