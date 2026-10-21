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


# --- Light units ---------------------------------------------------------------
# Blender lights are physical (watts, falling off with 1/r^2); a Fitzel light
# is a colour times an intensity that fades to nothing at its range
# ((1 - d/range)^2, lit.frag). There is no exact match, so these are chosen to
# land the usual cases where they look alike: a 1000 W room light becomes
# Fitzel's default (intensity 8, range ~12 m). Every light can override both.

def light_intensity(light):
    if light.type == "SUN":
        return light.energy / 3.0       # strength 3 (a clear day) -> 1.0
    return min(light.energy / 125.0, 100.0)


def light_range(light):
    if getattr(light, "use_custom_distance", False):
        return max(0.5, light.cutoff_distance)
    return max(1.0, min(math.sqrt(max(light.energy, 0.0)) * 0.4, 200.0))
