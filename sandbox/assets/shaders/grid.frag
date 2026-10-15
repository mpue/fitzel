#version 330 core

// Grid lines drawn from world coordinates, so the lattice is a property of the
// world rather than of the mesh: every line sits exactly where the 3D cursor's
// grid snap would put an object, which is the whole point of showing it.

in  vec3 vWorld;
out vec4 FragColor;

uniform vec3  uCamPos;
uniform float uCell;        // fine cell size = the cursor's snap step (metres)
uniform float uFade;        // distance at which the grid has gone entirely (m)
uniform vec3  uColor;
uniform float uOpacity;
uniform vec3  uCursor;      // the 3D cursor, whose cell is picked out
uniform float uCursorCell;  // 0/1 highlight that cell
uniform float uAxes;        // 0/1 colour the two lines through the world origin
uniform int   uAxis;        // the axis the plane is square to (grid.vert)
uniform vec3  uAlongA;      // colour of the origin line along the first in-plane axis
uniform vec3  uAlongB;      // ...and along the second
uniform float uOrtho;       // 1: an orthographic lens, looking along uViewDir
uniform vec3  uViewDir;

// A point's two coordinates in the grid's plane: (z, y), (x, z) or (x, y).
vec2 inPlane(vec3 w) {
    return uAxis == 0 ? w.zy : (uAxis == 2 ? w.xy : w.xz);
}
float alongNormal(vec3 w) {
    return uAxis == 0 ? w.x : (uAxis == 2 ? w.z : w.y);
}

// The scene's depth buffer, as a texture, plus the size of the image being drawn
// into. The grid is drawn AFTER the post chain -- so no exposure, bloom, fog or
// tonemap touches it and its colour is exactly the colour authored -- and by
// then the depth buffer that shaded the world is no longer attached, only
// readable. Hence the occlusion test below is done by hand.
uniform sampler2D uSceneDepth;
uniform vec2      uViewport;

// How much of this pixel the nearest grid line covers. Dividing the distance to
// the line by the pixel's own footprint (fwidth) is what keeps a line one pixel
// wide at any distance -- and what makes the whole field dissolve into flat tone
// instead of a moire storm once the cells fall below a pixel.
float lineCoverage(vec2 p, float cell) {
    vec2 q = p / cell;
    vec2 d = abs(fract(q - 0.5) - 0.5) / fwidth(q);
    return 1.0 - min(min(d.x, d.y), 1.0);
}

// The same measure for a single line at v == 0 (the world axes).
float axisCoverage(float v) {
    return 1.0 - min(abs(v) / fwidth(v), 1.0);
}

void main() {
    // Hidden by the solid world: same comparison a depth test would make, in the
    // same window-space depth the scene pass wrote, so ground and objects hide
    // the plane behind them exactly as they did when this was a depth-tested draw.
    if (texture(uSceneDepth, gl_FragCoord.xy / uViewport).r < gl_FragCoord.z)
        discard;

    vec2  p    = inPlane(vWorld);
    float dist = length(p - inPlane(uCamPos));

    // Two levels: the fine cell you snap to, and a heavier line every tenth of
    // them. The fine one is given up first, so distance thins the grid out to a
    // readable frame rather than washing it into grey.
    float fine = lineCoverage(p, uCell);
    float bold = lineCoverage(p, uCell * 10.0);
    float fadeFine = 1.0 - smoothstep(uFade * 0.16, uFade * 0.55, dist);
    float fadeBold = 1.0 - smoothstep(uFade * 0.55, uFade,        dist);

    // Grazing angle: towards the horizon a plane's lines pile up faster than
    // pixels can carry them, and no amount of per-line anti-aliasing saves that.
    // Fade them out as the view flattens onto the plane instead.
    // An orthographic lens looks the same way at every pixel.
    vec3  look  = uOrtho > 0.5 ? uViewDir : normalize(vWorld - uCamPos);
    float graze = smoothstep(0.0, 0.10, abs(alongNormal(look)));

    float a   = max(fine * 0.40 * fadeFine, bold * 0.85 * fadeBold) * graze;
    vec3  col = uColor;

    // World axes, in the usual colours (X red, Y green, Z blue): the line along
    // the first in-plane axis is where the second is zero, and the other way
    // round. They ride the bold level's fade, so the origin stays legible as
    // long as the heavy lines do.
    if (uAxes > 0.5) {
        float alongA = axisCoverage(p.y) * fadeBold * graze;
        float alongB = axisCoverage(p.x) * fadeBold * graze;
        float axA    = max(alongA, alongB);
        if (axA > 0.001) {
            col = mix(col, alongA >= alongB ? uAlongA : uAlongB, axA);
            a   = max(a, axA * 0.95);
        }
    }

    // The cursor's own cell, tinted. This is the grid answering "where will the
    // next thing land" -- the cell you are about to snap into, seen rather than
    // worked out from three numbers in a panel.
    if (uCursorCell > 0.5) {
        float hit = float(all(equal(floor(p / uCell), floor(inPlane(uCursor) / uCell))));
        col = mix(col, vec3(0.95, 0.42, 0.32), hit * 0.75);
        a   = max(a, hit * 0.16 * fadeFine * graze);
    }

    if (a <= 0.002) discard;   // the far field: nothing to blend, don't pay for it
    FragColor = vec4(col, a * uOpacity);
}
