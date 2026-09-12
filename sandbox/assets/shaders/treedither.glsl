// --- Mesh <-> impostor crossfade -----------------------------------------------
//
// Across the band where a tree hands over from its mesh to its impostor both are
// drawn, and each pixel belongs to exactly one of them: the same screen-space
// threshold decides, the mesh keeping the pixels below its remaining share and
// the card the rest. No blending (both are alpha-tested and write depth), no
// double-bright band, and TAA turns the pattern into a smooth dissolve.

float treeDitherValue() {
    // Interleaved gradient noise: a well-spread threshold per pixel.
    return fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
}

// `share`: how much of the handover the MESH still owns (1 = all of it).
// isCard = false for the mesh, true for the impostor.
bool treeDitherKeep(float share, bool isCard) {
    float t = treeDitherValue();
    return isCard ? (t >= share) : (t < share);
}
