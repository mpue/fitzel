# Blender -> Fitzel: the conventions the two disagree on, in one place.
#
# Blender is Z-up, Fitzel is Y-up like glTF, so a point (x, y, z) becomes
# (x, z, -y) -- exactly what the glTF exporter does to the vertices it writes,
# which is why a transform converted here lines up with a mesh exported there.
#
# Fitzel stores a rotation as an Euler triple in degrees composed Rz * Ry * Rx
# (docs/invariants.md, section 4). That is Blender's 'XYZ' order, so
# Matrix.to_euler('XYZ') is the decomposition, no reordering needed.
#
# What "forward" means differs per kind of object:
#   - a camera looks down its local -Z with +Y up in BOTH programs, so only the
#     world axes change (C @ R);
#   - a Fitzel spot shines down its local +Z, a Blender spot down -Z: a half
#     turn about Y on top (C @ R @ FLIP);
#   - everything else is geometry, whose local axes change with the world's
#     (C @ R @ C^T), matching the vertices the glTF exporter converted.

import math

from mathutils import Matrix, Vector

C = Matrix(((1.0, 0.0, 0.0),
            (0.0, 0.0, 1.0),
            (0.0, -1.0, 0.0)))
CT = C.transposed()
FLIP = Matrix(((-1.0, 0.0, 0.0),
               (0.0, 1.0, 0.0),
               (0.0, 0.0, -1.0)))


def point(v):
    """A Blender position or direction in Fitzel's axes."""
    return C @ Vector(v)


def axes(v):
    """A per-axis size (scale, bounding box) in Fitzel's axes -- unsigned."""
    return Vector((abs(v[0]), abs(v[2]), abs(v[1])))


def rotation(world, kind="OBJECT"):
    """Fitzel world rotation (3x3) of a Blender world matrix."""
    r = world.to_3x3().normalized()
    if kind == "VIEW":
        return C @ r
    if kind == "SPOT":
        return C @ r @ FLIP
    return C @ r @ CT


def scale_of(world):
    """The world matrix's scale per local axis, in Fitzel's axes."""
    m = world.to_3x3()
    return axes((m.col[0].length, m.col[1].length, m.col[2].length))


def needs_bake(world):
    """True when a matrix cannot be a Fitzel transform: mirrored, or sheared
    (a rotated child under a non-uniformly scaled parent). Such geometry goes
    into the model file with its scale and rotation applied."""
    m = world.to_3x3()
    if m.determinant() < 0.0:
        return True
    cols = [m.col[i].normalized() for i in range(3) if m.col[i].length > 1e-9]
    for i in range(len(cols)):
        for j in range(i + 1, len(cols)):
            if abs(cols[i].dot(cols[j])) > 1e-4:
                return True
    return False


def _clean(x, nd=6):
    x = round(float(x), nd)
    return 0.0 if x == 0.0 else x   # no -0.0 in the file


def vec3(v):
    return [_clean(v[0]), _clean(v[1]), _clean(v[2])]


def euler_deg(r):
    e = r.to_euler("XYZ")
    return vec3((math.degrees(e.x), math.degrees(e.y), math.degrees(e.z)))


def yaw_pitch(forward):
    """Fitzel's fly-camera angles for a Fitzel-space direction: yaw 0 looks
    down +X, 90 down +Z (engine/src/scene/Camera.cpp)."""
    f = Vector(forward).normalized()
    yaw = math.degrees(math.atan2(f.z, f.x))
    pitch = math.degrees(math.asin(max(-1.0, min(1.0, f.y))))
    return yaw, pitch


def sun_clock(world):
    """(timeOfDay, sunDeclination) that put Fitzel's sun where a Blender sun
    lamp shines from, at sunLatitude 0.

    Fitzel derives the sun from the clock (main.cpp, "The sun on its day
    arc"): with the latitude at 0 the direction TO the sun is
    (-cos(dec) sin(H), cos(dec) cos(H), -sin(dec)), H the hour angle from
    noon. Every direction has an exact answer there, so the sky stands where
    the lamp does."""
    to_sun = (C @ (world.to_3x3().normalized() @ Vector((0.0, 0.0, 1.0)))).normalized()
    dec = math.degrees(math.asin(max(-1.0, min(1.0, -to_sun.z))))
    hour_angle = math.atan2(-to_sun.x, to_sun.y)
    hour = (12.0 + hour_angle / (2.0 * math.pi) * 24.0) % 24.0
    return hour, dec


def vertical_fov(cam, render):
    """Vertical field of view in degrees, the one Fitzel's camera takes."""
    w = render.resolution_x * render.pixel_aspect_x
    h = render.resolution_y * render.pixel_aspect_y
    aspect = w / h if h > 0 else 16.0 / 9.0
    fit = cam.sensor_fit
    if fit == "AUTO":
        sensor = cam.sensor_width
        fit = "HORIZONTAL" if aspect >= 1.0 else "VERTICAL"
    elif fit == "HORIZONTAL":
        sensor = cam.sensor_width
    else:
        sensor = cam.sensor_height
    if cam.type == "PERSP":
        half = math.atan(sensor / (2.0 * max(cam.lens, 1e-3)))
    else:   # Fitzel's cameras are perspective; keep a sane default
        half = math.radians(30.0)
    if fit == "HORIZONTAL":
        half = math.atan(math.tan(half) / aspect)
    return math.degrees(2.0 * half)


# --- Back from Fitzel ----------------------------------------------------------

def from_point(v):
    """A Fitzel position or direction in Blender's axes."""
    return CT @ Vector(v)


def from_rotation(r, kind="OBJECT"):
    """Blender world rotation (3x3) of a Fitzel world rotation -- rotation()
    undone (FLIP is its own inverse)."""
    if kind == "VIEW":
        return CT @ r
    if kind == "SPOT":
        return CT @ r @ FLIP
    return CT @ r @ C


def euler_matrix(deg):
    """Fitzel's Euler triple (degrees, Rz * Ry * Rx) as a 3x3 matrix."""
    from mathutils import Euler
    return Euler([math.radians(a) for a in deg], "XYZ").to_matrix()


# --- Light units -----------------------------------------------------------------
# Blender's lights are radiometric: a point or spot lamp of P watts has the
# radiant intensity P / 4pi W/sr in every direction (what Cycles renders and
# the glTF exporter writes), a sun of S is S W/m^2 on a surface facing it, an
# area lamp of P radiates P / pi straight ahead. Blender shades a diffuse
# surface as albedo * E / pi; Fitzel as albedo * E, without the pi, while both
# light it from an environment as albedo * the environment's mean radiance. So
# Fitzel shows the picture Blender renders when every lamp's irradiance is
# divided by pi and the environment is taken as it is -- the "Like Blender"
# lighting. A Fitzel light with the inverse-square fall-off (LightComponent::
# falloff 1) then has intensity = W/sr / pi, and nothing is fitted or guessed.
#
# "Fitzel's look" keeps Fitzel's own sky, sun and exposure, which are tuned
# around a sun of intensity 1 (about 3.64 at noon, main.cpp). There a Blender
# sun of strength 3 -- a clear day -- is intensity 1 and every lamp scales with
# it, so lamps and sun stay in the proportion Blender has them in.

FITZEL_NOON = 3.4 * (0.12 + 0.95)          # Fitzel's sun at intensity 1, high in the sky
UNIT = {"MATCH": 1.0 / math.pi, "FITZEL": FITZEL_NOON / 3.0}


def unit(mode):
    return UNIT.get(mode, UNIT["MATCH"])


def _exposure(light):
    return 2.0 ** getattr(light, "exposure", 0.0)


def lamp_watts_per_sr(light, obj_world=None):
    """What the lamp radiates straight ahead, W/sr (Blender's own units)."""
    p = light.energy * _exposure(light)
    if light.type == "AREA":
        if not getattr(light, "normalize", True) and obj_world is not None:
            try:
                p *= light.area(matrix_world=obj_world)
            except (TypeError, AttributeError):
                pass
        return p / math.pi
    return p / (4.0 * math.pi)


def lamp_intensity(light, mode, obj_world=None):
    """Fitzel intensity of an inverse-square light."""
    return lamp_watts_per_sr(light, obj_world) * unit(mode)


def lamp_energy(intensity, light, mode, obj_world=None):
    """lamp_intensity undone: the Blender power giving `intensity`."""
    w_sr = intensity / unit(mode)
    if light.type == "AREA":
        p = w_sr * math.pi
        if not getattr(light, "normalize", True) and obj_world is not None:
            try:
                p /= max(light.area(matrix_world=obj_world), 1e-9)
            except (TypeError, AttributeError):
                pass
    else:
        p = w_sr * 4.0 * math.pi
    return p / _exposure(light)


def lamp_range(light, threshold=0.01, obj_world=None):
    """Where the lamp is done: its custom distance, else where Blender itself
    stops counting it -- EEVEE's light threshold, in the picture's terms (the
    "Like Blender" units, so the range is the same in both lightings)."""
    if getattr(light, "use_custom_distance", False):
        return max(0.5, light.cutoff_distance)
    peak = max(max(light.color), 1e-6) * lamp_watts_per_sr(light, obj_world) / math.pi
    return max(0.5, min(math.sqrt(peak / max(threshold, 1e-6)), 1000.0))


# A Blender spot is full inside cos(outer) + blend * (1 - cos(outer)) and fades
# to nothing at the outer cone (Cycles, in cosines); Fitzel's inner cone is
# outer * (1 - blend) (main.cpp), in angles.

def spot_cone(light):
    """(spotAngle, spotBlend) for Fitzel, from a Blender spot."""
    outer = min(light.spot_size * 0.5, math.radians(89.0))
    co = math.cos(outer)
    inner = math.acos(min(1.0, co + light.spot_blend * (1.0 - co)))
    return math.degrees(outer), (1.0 - inner / outer) if outer > 1e-6 else 0.0


def spot_from_fitzel(angle_deg, blend):
    """spot_cone undone: (spot_size, spot_blend)."""
    outer = math.radians(max(1.0, min(angle_deg, 89.0)))
    co = math.cos(outer)
    inner = outer * (1.0 - max(0.0, min(blend, 1.0)))
    b = (math.cos(inner) - co) / max(1.0 - co, 1e-9)
    return 2.0 * outer, max(0.0, min(b, 1.0))


def area_cone(light):
    """An area lamp as a Fitzel spot: one-sided, as wide as its spread allows."""
    spread = getattr(light, "spread", math.pi)
    return min(math.degrees(spread) * 0.5, 89.0), 1.0


# --- The sun -------------------------------------------------------------------
# Fitzel's sun is the clock's (sun_clock above), its light
#   tint * mix(noon colour, sunset orange, lowSun) * (0.12 + 0.95 dayF) * 3.4 * strength
# (main.cpp, "The Sun entity tints and scales the directional light"). "Like
# Blender" undoes the colour and the dimming the clock puts on it, so what
# reaches the ground is the lamp's own.

def _smoothstep(a, b, x):
    t = max(0.0, min((x - a) / (b - a), 1.0))
    return t * t * (3.0 - 2.0 * t)


def fitzel_sun_dir(hour, latitude, declination):
    """Fitzel's direction TO the sun (its axes) for its clock settings."""
    h = (hour - 12.0) / 24.0 * 2.0 * math.pi
    lat, dec = math.radians(latitude), math.radians(declination)
    up = math.sin(lat) * math.sin(dec) + math.cos(lat) * math.cos(dec) * math.cos(h)
    east = -math.cos(dec) * math.sin(h)
    north = math.cos(lat) * math.sin(dec) - math.sin(lat) * math.cos(dec) * math.cos(h)
    return Vector((east, up, -north)).normalized()


def _sun_curve(y):
    day = _smoothstep(-0.12, 0.18, y)
    low = 1.0 - max(0.0, min(y / 0.3, 1.0))
    col = Vector((1.0, 0.97, 0.9)).lerp(Vector((1.0, 0.55, 0.26)), low)
    return col, (0.12 + 0.95 * day) * 3.4 * _smoothstep(-0.03, 0.03, y)


def sun_light(light, world, mode):
    """(tint, intensity) of Fitzel's Sun component for a Blender sun lamp."""
    s = light.energy * _exposure(light)
    color = Vector(light.color)
    if mode != "MATCH":
        return color, s / 3.0
    y = (C @ (world.to_3x3().normalized() @ Vector((0.0, 0.0, 1.0)))).normalized().y
    col, scale = _sun_curve(y)
    if scale < 1e-4:
        return color, 0.0
    return Vector([color[i] / col[i] for i in range(3)]), s * unit(mode) / scale


def sun_from_fitzel(tint, intensity, to_sun_y, light, mode):
    """sun_light undone: (colour, strength) of the Blender sun."""
    tint = Vector(tint)
    if mode != "MATCH":
        return tint, intensity * 3.0 / _exposure(light)
    col, scale = _sun_curve(to_sun_y)
    color = Vector([tint[i] * col[i] for i in range(3)])
    return color, intensity * scale / unit(mode) / _exposure(light)


# --- The picture ---------------------------------------------------------------
# Blender's view transforms as Fitzel's curves (composite.frag, tonemapCurve:
# 0 ACES, 1 AgX with Fitzel's lift and look, 2 PBR Neutral, 3 AgX as Blender
# has it). Fitzel lifts its curves to ACES' mid grey (PBR Neutral by 1.705;
# its own AgX by 2.2409, curve 3 not at all), so the exposure is divided by
# that lift to put the picture where Blender's is.
TONEMAP = {"AgX": 3, "Filmic": 3, "Filmic Log": 3, "Khronos PBR Neutral": 2,
           "Standard": 2, "Raw": 2}
TONEMAP_BACK = {1: "AgX", 2: "Khronos PBR Neutral", 3: "AgX"}
LIFT = {0: 1.0, 1: 2.2409, 2: 1.705, 3: 1.0}


def tonemap_curve(view_transform):
    if "ACES" in view_transform:
        return 0
    return TONEMAP.get(view_transform, 3)


def fitzel_exposure(view_settings):
    """Fitzel's exposure for Blender's: its stops, over the curve's lift."""
    return 2.0 ** view_settings.exposure / LIFT[tonemap_curve(view_settings.view_transform)]


def blender_exposure(exposure, curve):
    """fitzel_exposure undone: Blender's stops."""
    return math.log2(max(exposure, 1e-6) * LIFT.get(curve, 1.0))


def look_grade(look):
    """(contrast, saturation) for Fitzel's grade, from an AgX/Filmic look name."""
    name = look.lower()
    contrast = 0.0
    for key, c in (("very low", -0.3), ("low contrast", -0.15), ("very high", 0.3),
                   ("high contrast", 0.15), ("punchy", 0.12)):
        if key in name:
            contrast = c
            break
    saturation = 0.0 if "greyscale" in name else (1.1 if "punchy" in name else 1.0)
    return contrast, saturation
