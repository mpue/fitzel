#version 330 core
// LensDrops: a wiper blade went over the lens. Multiplies the film (blended
// dst * src): inside the sector the blade swept this frame -- between two
// angles round the pivot, between two radii -- the wet liquid is gone and most
// of the dried; a thin streak of it stays where the rubber's edge passed.

in vec2 vNdc;

uniform vec2  uPivot;    // 0..1 from the top left of the view (y down)
uniform float uA0, uA1;  // radians, 0 straight up, positive to the right
uniform float uR0, uR1;  // shares of the view's height
uniform float uAspect;   // width / height
uniform vec2  uKeep;     // what stays of (wet, dried) under the blade

out vec4 FragColor;

void main() {
    vec2  uv = vec2(vNdc.x * 0.5 + 0.5, 0.5 - vNdc.y * 0.5);
    vec2  d  = vec2((uv.x - uPivot.x) * uAspect, uv.y - uPivot.y);
    float r  = length(d);
    float a  = atan(d.x, -d.y);
    float lo = min(uA0, uA1) - 0.004, hi = max(uA0, uA1) + 0.004;
    float inA = step(lo, a) * step(a, hi);
    float inR = smoothstep(uR0 - 0.01, uR0 + 0.01, r) * (1.0 - smoothstep(uR1 - 0.01, uR1 + 0.01, r));
    // the rubber's edge leaves faint streaks along the arc
    float streak = 0.5 + 0.5 * sin(r * 900.0);
    vec2  keep = mix(vec2(1.0), uKeep * (0.6 + 0.8 * streak), inA * inR);
    FragColor = vec4(keep, 1.0, 1.0);
}
