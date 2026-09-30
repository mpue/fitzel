// --- Bark relief from a tangent-space normal map (needs nothing else) ----------
//
// A tree vertex carries no tangent -- eight floats, position, normal, uv, the
// layout every tree path shares -- so the basis is built per pixel from the
// screen-space derivatives of the position and the uv (Schueler's cotangent
// frame, as lit.frag does for scene objects).
//
// The sign of green. Tree textures follow glTF: decoded top row first, v = 0 at
// the top of the image, so v runs DOWN the picture. A glTF (OpenGL-convention)
// normal map puts +Y UP the picture -- towards -v. The derivative bitangent is
// the direction v grows in, so green has to be negated; without it every ridge
// in the bark is lit from the wrong end, which on a trunk lit from above reads as
// bark that has been turned inside out.
//
// `strength` scales the tilt (glTF normalTexture.scale times the part's own).
// Returns N unchanged where the uv does not vary across the pixel (a degenerate
// basis would otherwise turn into NaN and a black speck).
vec3 treeNormalMap(sampler2D nmap, vec3 N, vec3 p, vec2 uv, float strength) {
    vec3 nt = texture(nmap, uv).xyz * 2.0 - 1.0;
    nt.xy *= strength;
    nt.y = -nt.y;
    vec3 dp1 = dFdx(p), dp2 = dFdy(p);
    vec2 du1 = dFdx(uv), du2 = dFdy(uv);
    vec3 dp2p = cross(dp2, N), dp1p = cross(N, dp1);
    vec3 T = dp2p * du1.x + dp1p * du2.x;
    vec3 B = dp2p * du1.y + dp1p * du2.y;
    float m = max(dot(T, T), dot(B, B));
    if (m < 1e-24) return N;
    float inv = inversesqrt(m);
    vec3 n = mat3(T * inv, B * inv, N) * nt;
    float l = length(n);
    return l > 1e-6 ? n / l : N;
}