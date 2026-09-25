#include "HouseGen.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>

#include <nlohmann/json.hpp>

#include "EditMesh.hpp"

using fitzel::AssetId;

namespace housegen {

namespace {

constexpr float kEps = 1e-3f;

// The U-stair: two flights of eight risers side by side with a half landing
// across both, the classic "halbgewendelte Podesttreppe" of a German family
// house. Fixed in plan -- only the rise follows the storey height -- because a
// stair is the one element of a house whose dimensions are not negotiable.
constexpr float kTread          = 0.27f;
constexpr int   kStepsPerFlight = 8;
constexpr float kFlightW        = 0.95f;
constexpr float kEyeW           = 0.10f;                        // gap between flights
constexpr float kStairW         = 2.0f * kFlightW + kEyeW;      // 2.00
constexpr float kLanding        = 0.94f;
constexpr float kStairLen       = kStepsPerFlight * kTread + kLanding;   // 3.10
constexpr float kArrival        = 1.10f;   // free strip at the foot of the stair
constexpr float kDoorReach      = 1.20f;   // hall overlap a room needs for a door
                                           // (0.885 leaf + 15 cm to each wall)
constexpr float kRoofThickness  = 0.30f;   // rafters + insulation + tiles

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

std::string fmt1(float v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.1f", v);
    return b;
}

// Clamp every field into a range the layout can actually build, so no stepper
// (and no hand-edited scene file) can ask for a 2 m wide house or a storey the
// stair cannot climb. Everything downstream may assume these bounds.
Params sane(const Params& in) {
    Params p = in;
    p.width        = clampf(p.width, 8.0f, 24.0f);   // below 8 m no stair fits beside rooms
    p.depth        = clampf(p.depth, 7.0f, 18.0f);
    p.storeys      = std::clamp(p.storeys, 1, kMaxStoreys);
    p.storeyHeight = clampf(p.storeyHeight, 2.55f, 3.40f);
    p.clearHeight  = clampf(p.clearHeight, 2.25f, p.storeyHeight - 0.20f);
    p.plinth       = clampf(p.plinth, 0.0f, 1.0f);
    p.outerWall    = clampf(p.outerWall, 0.20f, 0.60f);
    p.bearingWall  = clampf(p.bearingWall, 0.10f, 0.30f);
    p.partition    = clampf(p.partition, 0.07f, 0.20f);
    const float maxService = p.depth - 2.0f * p.outerWall - p.bearingWall - 2.8f;
    p.serviceDepth = clampf(p.serviceDepth, kStairW + 1.10f, std::max(kStairW + 1.10f, maxService));
    p.roofPitch     = clampf(p.roofPitch, 20.0f, 55.0f);
    p.kneeWall      = clampf(p.kneeWall, 0.0f, 2.0f);
    p.eaveOverhang  = clampf(p.eaveOverhang, 0.0f, 1.2f);
    p.gableOverhang = clampf(p.gableOverhang, 0.0f, 1.0f);
    p.cutLevel      = std::clamp(p.cutLevel, 0, kMaxStoreys);
    auto fixRooms = [](std::vector<RoomSpec>& rs) {
        if (rs.size() > 8) rs.resize(8);
        for (RoomSpec& r : rs) {
            const int t = std::clamp(static_cast<int>(r.type), 0,
                                     static_cast<int>(RoomType::Count) - 1);
            r.type = static_cast<RoomType>(t);
            r.area = clampf(r.area, 1.5f, 150.0f);
        }
    };
    for (auto& rs : p.storeyRooms) fixRooms(rs);
    fixRooms(p.atticRooms);
    return p;
}

std::string displayName(const RoomSpec& r) {
    return r.name.empty() ? std::string(roomTypeName(r.type)) : r.name;
}

// Split [a,b] into pieces proportional to `weights`, with `gap` between
// neighbours (the partition walls) and no piece narrower than its minimum.
std::vector<std::pair<float, float>> splitSpan(float a, float b,
                                               const std::vector<float>& weights,
                                               float gap, std::vector<float> mins) {
    std::vector<std::pair<float, float>> out;
    const int n = static_cast<int>(weights.size());
    if (n == 0) return out;
    const float avail = std::max(0.0f, (b - a) - gap * static_cast<float>(n - 1));
    mins.resize(n, 0.0f);
    float minSum = 0.0f;
    for (float m : mins) minSum += m;
    if (minSum > avail && minSum > 0.0f)   // asked for more than there is: share it
        for (float& m : mins) m *= avail / minSum;
    std::vector<float> w(n);
    float sum = 0.0f;
    for (int i = 0; i < n; ++i) sum += std::max(weights[i], 0.1f);
    for (int i = 0; i < n; ++i) w[i] = avail * std::max(weights[i], 0.1f) / sum;
    // Lift the pieces below their minimum and take the difference from the
    // rest, proportionally to what each has to spare.
    for (int pass = 0; pass < 4; ++pass) {
        float deficit = 0.0f, spare = 0.0f;
        for (int i = 0; i < n; ++i) {
            if (w[i] < mins[i]) deficit += mins[i] - w[i];
            else spare += w[i] - mins[i];
        }
        if (deficit <= 1e-5f || spare <= 1e-5f) break;
        const float k = std::min(1.0f, deficit / spare);
        for (int i = 0; i < n; ++i)
            w[i] = (w[i] < mins[i]) ? mins[i] : w[i] - (w[i] - mins[i]) * k;
    }
    float x = a;
    for (int i = 0; i < n; ++i) {
        const float x1 = (i == n - 1) ? b : x + w[i];
        out.push_back({x, x1});
        x = x1 + gap;
    }
    return out;
}

// A door (or pass) in wall `w` across [a,b] along it. `sideSign` is the side the
// leaf swings to: +1 towards +y (a wall along x) or +x (a wall along y).
Opening makeDoor(const Wall& w, float a, float b, int sideSign, Opening::Kind kind,
                 float head, bool leaf) {
    Opening o;
    o.kind   = kind;
    o.alongX = w.alongX;
    o.exterior = w.exterior;
    o.sill   = 0.0f;
    o.head   = head;
    o.hasLeaf = leaf;
    const float width = b - a;
    if (w.alongX) {
        o.r = {a, w.r.y0, b, w.r.y1};
        const float face = sideSign > 0 ? w.r.y1 : w.r.y0;
        o.hinge = {a, face};
        o.open  = {a, face + static_cast<float>(sideSign) * width};
        o.close = {b, face};
    } else {
        o.r = {w.r.x0, a, w.r.x1, b};
        const float face = sideSign > 0 ? w.r.x1 : w.r.x0;
        o.hinge = {face, a};
        o.open  = {face + static_cast<float>(sideSign) * width, a};
        o.close = {face, b};
    }
    return o;
}

Opening makeWindow(const Wall& w, float a, float b, float sill, float head,
                   Opening::Kind kind, int room) {
    Opening o;
    o.kind     = kind;
    o.alongX   = w.alongX;
    o.exterior = w.exterior;
    o.sill     = sill;
    o.head     = head;
    o.room     = room;
    o.r = w.alongX ? Rect{a, w.r.y0, b, w.r.y1} : Rect{w.r.x0, a, w.r.x1, b};
    return o;
}

// The stretch of [a,b] not taken by openings already in the wall, as the
// largest free interval (with `margin` kept to every neighbour).
std::pair<float, float> largestFree(float a, float b, const std::vector<Opening>& ops,
                                    const Wall& w, float margin) {
    std::vector<std::pair<float, float>> taken;
    for (const Opening& o : ops) {
        if (o.alongX != w.alongX) continue;
        const bool inWall = w.alongX
            ? (o.r.y0 >= w.r.y0 - kEps && o.r.y1 <= w.r.y1 + kEps)
            : (o.r.x0 >= w.r.x0 - kEps && o.r.x1 <= w.r.x1 + kEps);
        if (!inWall) continue;
        const float oa = w.alongX ? o.r.x0 : o.r.y0;
        const float ob = w.alongX ? o.r.x1 : o.r.y1;
        taken.push_back({oa - margin, ob + margin});
    }
    std::sort(taken.begin(), taken.end());
    std::pair<float, float> best{a, a};
    float cur = a;
    for (const auto& t : taken) {
        if (t.first > cur && t.first - cur > best.second - best.first)
            best = {cur, std::min(t.first, b)};
        cur = std::max(cur, t.second);
    }
    if (b - cur > best.second - best.first) best = {cur, b};
    if (best.second < best.first) best.second = best.first;
    return best;
}

float roofTan(const Params& p) { return std::tan(glm::radians(p.roofPitch)); }
float roofThicknessV(const Params& p) {
    return kRoofThickness / std::cos(glm::radians(p.roofPitch));
}
float atticFloor(const Params& p) {
    return p.plinth + static_cast<float>(p.storeys) * p.storeyHeight;
}
// Underside of the roof at plan x: the knee wall at the inner face of each eave
// wall, rising at the pitch towards the ridge over the middle of the house.
float roofUnder(const Params& p, float x) {
    const float T = p.outerWall, W = p.width;
    const float d = (x <= 0.5f * W) ? x - T : (W - T) - x;
    return atticFloor(p) + p.kneeWall + roofTan(p) * d;
}

// Living area of an attic room per WoFlV: full where the clear height is at
// least 2 m, half between 1 and 2 m, nothing below 1 m. Integrated across the
// room (the slope runs along x; the ridge is north-south).
float atticLivingArea(const Params& p, const Rect& r) {
    const float z0 = atticFloor(p);
    const int   n  = 200;
    float sum = 0.0f;
    const float dx = r.w() / static_cast<float>(n);
    for (int i = 0; i < n; ++i) {
        const float x = r.x0 + (static_cast<float>(i) + 0.5f) * dx;
        const float h = std::min(roofUnder(p, x) - z0, p.clearHeight);
        sum += (h >= 2.0f ? 1.0f : (h >= 1.0f ? 0.5f : 0.0f)) * dx;
    }
    return sum * r.h();
}

bool overlapsX(const Rect& a, float x0, float x1) { return a.x1 > x0 + kEps && a.x0 < x1 - kEps; }

} // namespace

// --- Room types --------------------------------------------------------------

const char* roomTypeName(RoomType t) {
    switch (t) {
        case RoomType::Wohnen:    return "Wohnen";
        case RoomType::Essen:     return "Essen";
        case RoomType::Kueche:    return "K\xC3\xBC" "che";
        case RoomType::Diele:     return "Diele / Flur";
        case RoomType::Bad:       return "Bad";
        case RoomType::GaesteBad: return "G\xC3\xA4ste-Bad";
        case RoomType::Schlafen:  return "Schlafen";
        case RoomType::Kind:      return "Kind";
        case RoomType::Buero:     return "B\xC3\xBCro";
        case RoomType::Gast:      return "Gast";
        case RoomType::HWR:       return "HWR / Technik";
        case RoomType::Abstell:   return "Abstellraum";
        case RoomType::Garderobe: return "Garderobe";
        case RoomType::Ankleide:  return "Ankleide";
        case RoomType::Count:     break;
    }
    return "Raum";
}

float defaultArea(RoomType t) {
    switch (t) {
        case RoomType::Wohnen:    return 30.0f;
        case RoomType::Essen:     return 14.0f;
        case RoomType::Kueche:    return 11.0f;
        case RoomType::Diele:     return 8.0f;
        case RoomType::Bad:       return 9.0f;
        case RoomType::GaesteBad: return 4.0f;
        case RoomType::Schlafen:  return 16.0f;
        case RoomType::Kind:      return 14.0f;
        case RoomType::Buero:     return 12.0f;
        case RoomType::Gast:      return 12.0f;
        case RoomType::HWR:       return 6.0f;
        case RoomType::Abstell:   return 4.0f;
        case RoomType::Garderobe: return 3.0f;
        case RoomType::Ankleide:  return 5.0f;
        case RoomType::Count:     break;
    }
    return 12.0f;
}

bool isHabitable(RoomType t) {
    return t == RoomType::Wohnen || t == RoomType::Essen || t == RoomType::Kueche ||
           t == RoomType::Schlafen || t == RoomType::Kind || t == RoomType::Buero ||
           t == RoomType::Gast;
}

bool isService(RoomType t) {
    return t == RoomType::Kueche || t == RoomType::Bad || t == RoomType::GaesteBad ||
           t == RoomType::HWR || t == RoomType::Abstell || t == RoomType::Garderobe ||
           t == RoomType::Ankleide;
}

// --- Presets -------------------------------------------------------------------

const char* presetName(Preset p) {
    switch (p) {
        case Preset::Classic:   return "Classic";
        case Preset::Bungalow:  return "Bungalow";
        case Preset::Townhouse: return "Townhouse";
        case Preset::SemiDetached: return "Semi-detached";
        case Preset::TownVilla:    return "Town villa";
        case Preset::CountryHouse: return "Country house";
        case Preset::LargeFamily:  return "Large family";
        case Preset::Cottage:      return "Cottage";
        case Preset::MultiGen:     return "Multi-generation";
        case Preset::Count:     break;
    }
    return "?";
}

void applyPreset(Params& p, Preset preset) {
    const glm::vec3 fc = p.facadeColor, pc = p.plinthColor, rc = p.roofColor,
                    frc = p.frameColor, flc = p.floorColor;
    p = Params{};   // every field back to its default; the colours survive
    p.facadeColor = fc; p.plinthColor = pc; p.roofColor = rc;
    p.frameColor = frc; p.floorColor = flc;
    for (auto& rs : p.storeyRooms) rs.clear();
    p.atticRooms.clear();
    using R = RoomType;
    switch (preset) {
        case Preset::Count:
        case Preset::Classic:
            // Ground floor: kitchen, living, hall, guest bath. First floor: two
            // bedrooms and the bath. Attic: the second child's room and an office.
            p.width = 10.0f; p.depth = 8.5f; p.storeys = 2; p.attic = true;
            p.serviceDepth = 3.335f; p.roofPitch = 40.0f; p.kneeWall = 1.0f;
            p.storeyRooms[0] = {{R::Kueche, "K\xC3\xBC" "che", 10.0f}, {R::Wohnen, "Wohnen / Essen", 40.0f},
                                {R::Diele, "Diele", 8.0f}, {R::GaesteBad, "G\xC3\xA4ste-Bad", 4.0f}};
            p.storeyRooms[1] = {{R::Kind, "Kind 1", 19.5f}, {R::Schlafen, "Eltern", 19.5f},
                                {R::Bad, "Bad", 10.0f}};
            p.atticRooms     = {{R::Kind, "Kind 2", 17.0f}, {R::Buero, "B\xC3\xBCro", 17.0f}};
            break;
        case Preset::Bungalow:
            // Everything on one level; the roof space stays an unheated loft.
            // Bedrooms west, the living room east under the kitchen, so the
            // kitchen opens straight into it.
            p.width = 13.0f; p.depth = 9.5f; p.storeys = 1; p.attic = false;
            p.serviceDepth = 3.4f; p.roofPitch = 28.0f; p.kneeWall = 0.4f;
            p.storeyRooms[0] = {{R::Diele, "Diele", 10.0f}, {R::Schlafen, "Schlafen", 18.0f},
                                {R::Kind, "Kind", 13.0f}, {R::Wohnen, "Wohnen / Essen", 38.0f},
                                {R::Kueche, "K\xC3\xBC" "che", 12.0f}, {R::Bad, "Bad", 8.0f},
                                {R::HWR, "HWR", 5.0f}};
            break;
        case Preset::Townhouse:
            // Narrow and tall: three full storeys and a studio under the roof.
            p.width = 8.0f; p.depth = 10.0f; p.storeys = 3; p.attic = true;
            p.serviceDepth = 3.4f; p.roofPitch = 45.0f; p.kneeWall = 0.8f;
            p.storeyRooms[0] = {{R::Diele, "Diele", 8.0f}, {R::Kueche, "K\xC3\xBC" "che", 8.0f},
                                {R::Wohnen, "Wohnen / Essen", 41.0f}};
            p.storeyRooms[1] = {{R::Schlafen, "Eltern", 19.5f}, {R::Kind, "Kind 1", 21.0f},
                                {R::Bad, "Bad", 8.0f}};
            p.storeyRooms[2] = {{R::Kind, "Kind 2", 19.5f}, {R::Buero, "B\xC3\xBCro", 21.0f},
                                {R::Bad, "Duschbad", 8.0f}};
            p.atticRooms     = {{R::Buero, "Studio", 41.0f}};
            break;
        case Preset::SemiDetached:
            // Doppelhaushaelfte: one party wall's worth of width, the depth
            // making up for it. Guest WC west of the stair, kitchen east.
            p.width = 8.5f; p.depth = 9.5f; p.storeys = 2; p.attic = true;
            p.serviceDepth = 3.4f; p.roofPitch = 42.0f; p.kneeWall = 0.8f;
            p.storeyRooms[0] = {{R::Diele, "Diele", 7.0f}, {R::Wohnen, "Wohnen / Essen", 40.0f},
                                {R::Kueche, "K\xC3\xBC" "che", 7.0f}, {R::GaesteBad, "G\xC3\xA4ste-WC", 4.0f}};
            p.storeyRooms[1] = {{R::Kind, "Kind 1", 18.5f}, {R::Schlafen, "Eltern", 21.0f},
                                {R::Bad, "Bad", 7.0f}};
            p.atticRooms     = {{R::Kind, "Kind 2", 19.0f}, {R::Buero, "B\xC3\xBCro", 21.0f}};
            break;
        case Preset::TownVilla:
            // Stadtvilla: square, two full storeys and a low roof over a loft --
            // every room at full height, which is the whole point of the type.
            p.width = 12.0f; p.depth = 10.5f; p.storeys = 2; p.attic = false;
            p.serviceDepth = 4.2f; p.roofPitch = 22.0f; p.kneeWall = 0.2f;
            p.eaveOverhang = 0.8f; p.gableOverhang = 0.6f;
            p.storeyRooms[0] = {{R::Diele, "Diele", 10.0f}, {R::Wohnen, "Wohnen", 37.0f},
                                {R::Essen, "Essen", 23.0f}, {R::Kueche, "K\xC3\xBC" "che", 12.0f},
                                {R::GaesteBad, "G\xC3\xA4ste-Bad", 4.5f}, {R::HWR, "HWR", 6.0f}};
            p.storeyRooms[1] = {{R::Kind, "Kind 1", 21.5f}, {R::Kind, "Kind 2", 15.0f},
                                {R::Schlafen, "Eltern", 23.0f}, {R::Bad, "Bad", 12.0f},
                                {R::Ankleide, "Ankleide", 10.5f}};
            break;
        case Preset::CountryHouse:
            // Landhaus, "anderthalbgeschossig": one full storey, and a steep roof
            // on a high knee wall that holds the whole sleeping floor.
            p.width = 12.0f; p.depth = 9.5f; p.storeys = 1; p.attic = true;
            p.serviceDepth = 3.6f; p.roofPitch = 45.0f; p.kneeWall = 1.4f;
            p.eaveOverhang = 0.7f;
            p.storeyRooms[0] = {{R::Diele, "Diele", 9.0f}, {R::Wohnen, "Wohnen", 32.0f},
                                {R::Essen, "Essen", 23.5f}, {R::Kueche, "K\xC3\xBC" "che", 11.5f},
                                {R::GaesteBad, "G\xC3\xA4ste-Bad", 4.0f}, {R::HWR, "HWR", 5.0f}};
            p.atticRooms     = {{R::Kind, "Kind 1", 19.5f}, {R::Kind, "Kind 2", 12.5f},
                                {R::Schlafen, "Eltern", 23.0f}, {R::Bad, "Bad", 11.5f}};
            break;
        case Preset::LargeFamily:
            // Five bedrooms, a guest room on the ground floor, a bath per storey.
            p.width = 11.5f; p.depth = 9.5f; p.storeys = 2; p.attic = true;
            p.serviceDepth = 3.6f; p.roofPitch = 38.0f; p.kneeWall = 1.0f;
            p.storeyRooms[0] = {{R::Diele, "Diele", 9.0f}, {R::Gast, "G\xC3\xA4stezimmer", 15.5f},
                                {R::Wohnen, "Wohnen / Essen", 37.5f}, {R::Kueche, "K\xC3\xBC" "che", 11.0f},
                                {R::GaesteBad, "G\xC3\xA4ste-Bad", 4.0f}, {R::Garderobe, "Garderobe", 2.5f}};
            p.storeyRooms[1] = {{R::Kind, "Kind 1", 18.0f}, {R::Kind, "Kind 2", 13.0f},
                                {R::Schlafen, "Eltern", 22.0f}, {R::Bad, "Bad", 11.0f},
                                {R::HWR, "HWR", 6.5f}};
            p.atticRooms     = {{R::Kind, "Kind 3", 28.0f}, {R::Buero, "B\xC3\xBCro", 25.0f},
                                {R::Bad, "Duschbad", 10.5f}};
            break;
        case Preset::Cottage:
            // Ferienhaus: a living kitchen and a shower room downstairs, two
            // bedrooms under a steep roof.
            p.width = 8.0f; p.depth = 7.5f; p.storeys = 1; p.attic = true;
            p.serviceDepth = 3.1f; p.roofPitch = 45.0f; p.kneeWall = 1.0f;
            p.storeyRooms[0] = {{R::Diele, "Diele", 5.0f}, {R::Wohnen, "Wohnen / Essen", 25.0f},
                                {R::Kueche, "K\xC3\xBC" "che", 5.0f}, {R::Bad, "Duschbad", 4.0f}};
            p.atticRooms     = {{R::Schlafen, "Schlafen", 13.0f}, {R::Kind, "Schlafen 2", 12.0f}};
            break;
        case Preset::MultiGen:
            // Mehrgenerationenhaus: the grandparents' bedroom and shower on the
            // ground floor, no stairs for them; the family above, a studio on top.
            p.width = 12.5f; p.depth = 10.0f; p.storeys = 2; p.attic = true;
            p.serviceDepth = 3.8f; p.roofPitch = 40.0f; p.kneeWall = 1.0f;
            p.storeyRooms[0] = {{R::Diele, "Diele", 10.0f}, {R::Schlafen, "Oma & Opa", 18.0f},
                                {R::Wohnen, "Wohnen / Essen", 43.0f}, {R::Kueche, "K\xC3\xBC" "che", 12.0f},
                                {R::Bad, "Duschbad", 7.0f}};
            p.storeyRooms[1] = {{R::Kind, "Kind 1", 20.0f}, {R::Kind, "Kind 2", 17.0f},
                                {R::Schlafen, "Eltern", 24.0f}, {R::Bad, "Bad", 12.0f},
                                {R::HWR, "HWR", 7.0f}};
            p.atticRooms     = {{R::Buero, "Studio", 37.5f}, {R::Gast, "G\xC3\xA4stezimmer", 24.0f}};
            break;
    }
}

Params::Params() {
    storeyRooms[0] = {{RoomType::Kueche, "K\xC3\xBC" "che", 10.0f},
                      {RoomType::Wohnen, "Wohnen / Essen", 40.0f},
                      {RoomType::Diele, "Diele", 8.0f},
                      {RoomType::GaesteBad, "G\xC3\xA4ste-Bad", 4.0f}};
    storeyRooms[1] = {{RoomType::Kind, "Kind 1", 19.5f},
                      {RoomType::Schlafen, "Eltern", 19.5f},
                      {RoomType::Bad, "Bad", 10.0f}};
    atticRooms     = {{RoomType::Kind, "Kind 2", 17.0f},
                      {RoomType::Buero, "B\xC3\xBCro", 17.0f}};
}

// --- Layout -------------------------------------------------------------------

float roofUnderside(const Plan& plan, float x) { return roofUnder(plan.params, x); }

Plan layout(const Params& in) {
    Plan plan;
    const Params p = sane(in);
    plan.params = p;

    const float W = p.width, D = p.depth, T = p.outerWall, B = p.bearingWall,
                Pt = p.partition, nd = p.serviceDepth;
    const float xi0 = T, xi1 = W - T;          // inside faces of the eave walls
    const float yN0 = T, yN1 = T + nd;         // service band
    const float yS0 = yN1 + B, yS1 = D - T;    // main band
    const int   nFull = p.storeys;
    const int   nLv   = nFull + 1;             // the attic is always a level
    const int   topLv = p.attic ? nFull : nFull - 1;   // highest level the stair reaches

    // --- Sort each level's programme into hall / west column / east room / main.
    struct Sorted {
        std::string           hallName;
        std::vector<RoomSpec> west, main;
        RoomSpec              east;
        bool                  hasEast = false;
    };
    std::vector<Sorted> sorted(nLv);
    for (int L = 0; L < nLv; ++L) {
        const std::vector<RoomSpec> empty;
        const std::vector<RoomSpec>& prog =
            (L < nFull) ? p.storeyRooms[L] : (p.attic ? p.atticRooms : empty);
        Sorted& s = sorted[L];
        std::vector<RoomSpec> service;
        for (const RoomSpec& r : prog) {
            if (r.type == RoomType::Diele) { if (s.hallName.empty()) s.hallName = displayName(r); }
            else if (isService(r.type)) service.push_back(r);
            else s.main.push_back(r);
        }
        // The room east of the stair: the kitchen if there is one (it opens onto
        // the main band below it), else the bath, so the wet rooms of all storeys
        // stack; with several service rooms and neither, the largest.
        int eastIdx = -1;
        for (RoomType pref : {RoomType::Kueche, RoomType::Bad})
            for (int i = 0; i < static_cast<int>(service.size()) && eastIdx < 0; ++i)
                if (service[i].type == pref) eastIdx = i;
        if (eastIdx < 0 && service.size() >= 2) {
            eastIdx = 0;
            for (int i = 1; i < static_cast<int>(service.size()); ++i)
                if (service[i].area > service[eastIdx].area) eastIdx = i;
        }
        for (int i = 0; i < static_cast<int>(service.size()); ++i) {
            if (i == eastIdx) { s.east = service[i]; s.hasEast = true; }
            else s.west.push_back(service[i]);
        }
        // The west column stacks its rooms north to south, each with a door onto
        // the hall; what does not fit with a usable depth goes to the main band.
        // Into the middle of the main band, not at an end: the end rooms sit under
        // the service columns, and only a room the hall reaches can have a door.
        const int maxWest = std::max(1, static_cast<int>((nd + Pt) / (1.25f + Pt)));
        while (static_cast<int>(s.west.size()) > maxWest) {
            s.main.insert(s.main.begin() + std::min<std::size_t>(1, s.main.size()), s.west.back());
            s.west.pop_back();
        }
    }

    std::vector<std::string> warn, info;

    // --- Column widths (shared by every storey, so the walls stand on walls)
    // and the main band's partitions, which have to agree: every main room needs
    // a door from the hall, so the first and the last room of each storey must
    // reach past the column above them by a door's width.
    const bool needStair = topLv >= 1;
    const float hallNeed = needStair ? kStairLen + kArrival : 2.0f;
    float ww = 0.0f, we = 0.0f, wwMax = 1e9f, weMax = 1e9f;
    std::vector<std::vector<std::pair<float, float>>> mainSpans(nLv);
    auto hallWidth = [&] {
        return (xi1 - xi0) - (ww > 0.0f ? ww + Pt : 0.0f) - (we > 0.0f ? we + B : 0.0f);
    };
    auto sizeColumns = [&] {
        ww = we = 0.0f;
        for (int L = 0; L < nLv; ++L) {
            const Sorted& s = sorted[L];
            float westA = 0.0f;
            for (const RoomSpec& r : s.west) westA += r.area;
            if (!s.west.empty())
                ww = std::max(ww, westA / nd + Pt * static_cast<float>(s.west.size() - 1) * 0.5f);
            if (s.hasEast) we = std::max(we, s.east.area / nd);
        }
        if (ww > 0.0f) ww = clampf(ww, 1.5f, 3.6f);
        if (we > 0.0f) we = clampf(we, 2.0f, 4.2f);
        // The end rooms give way to the columns first -- a bath has a smallest
        // useful size, a bedroom 30 cm wider than asked is still a bedroom -- but
        // only so far: past a third over its share, the column gives way instead.
        for (int L = 0; L < nLv; ++L) {
            const std::vector<RoomSpec>& m = sorted[L].main;
            std::vector<float> wts, mins(m.size(), 2.2f);
            float sum = 0.0f;
            for (const RoomSpec& r : m) { wts.push_back(r.area); sum += r.area; }
            if (m.size() >= 2) {
                const float avail = (xi1 - xi0) - Pt * static_cast<float>(m.size() - 1);
                auto cap = [&](float a) { return std::max(2.2f, 1.33f * avail * a / sum); };
                if (ww > 0.0f)
                    mins.front() = std::max(mins.front(),
                                            std::min(ww + Pt + kDoorReach, cap(m.front().area)));
                if (we > 0.0f)
                    mins.back() = std::max(mins.back(),
                                           std::min(we + B + kDoorReach, cap(m.back().area)));
            }
            mainSpans[L] = splitSpan(xi0, xi1, wts, Pt, mins);
        }
        // Whatever the band could not give, the columns give back.
        wwMax = weMax = 1e9f;
        for (int L = 0; L < nLv; ++L) {
            if (mainSpans[L].empty()) continue;
            wwMax = std::min(wwMax, mainSpans[L].front().second - kDoorReach - xi0 - Pt);
            weMax = std::min(weMax, xi1 - B - (mainSpans[L].back().first + kDoorReach));
        }
        if (ww > 0.0f) ww = std::max(1.2f, std::min(ww, wwMax));
        if (we > 0.0f) we = std::max(1.2f, std::min(we, weMax));
        // Too narrow for the stair: give back width, west column first.
        for (int it = 0; it < 2 && hallWidth() < hallNeed; ++it) {
            float& col = (it == 0) ? ww : we;
            if (col <= 0.0f) continue;
            col = std::max(1.2f, col - (hallNeed - hallWidth()));
        }
    };
    sizeColumns();
    // Still no room for the stair and a way to the front door: the west column
    // goes, and its rooms join the main band.
    if (hallWidth() < hallNeed - kEps && ww > 0.0f) {
        for (Sorted& s : sorted) {
            s.main.insert(s.main.begin() + std::min<std::size_t>(1, s.main.size()),
                          s.west.begin(), s.west.end());
            s.west.clear();
        }
        sizeColumns();
        info.push_back("Too narrow for a service column west of the stair: its "
                       "rooms moved to the south side.");
    }
    if (hallWidth() < hallNeed - kEps)
        warn.push_back("The house is too narrow for the stair and the service rooms "
                       "-- make it wider or move rooms out.");
    // A hall far wider than the stair needs is wasted floor: put a store room
    // west of it (on every storey) instead.
    if (ww <= 0.0f && hallWidth() > hallNeed + 2.4f) {
        const float cand = std::min(hallWidth() - hallNeed - 0.9f - Pt, std::min(wwMax, 2.4f));
        if (cand >= 1.4f) ww = cand;
    }
    const float hx0 = xi0 + (ww > 0.0f ? ww + Pt : 0.0f);
    const float hx1 = xi1 - (we > 0.0f ? we + B : 0.0f);

    plan.hasStair = needStair;
    Stair& st = plan.stair;
    st.r        = {hx1 - kStairLen, yN0, hx1, yN0 + kStairW};
    st.landingX = hx1 - kLanding;
    st.flight   = kFlightW;
    st.tread    = kTread;
    st.risers   = 2 * kStepsPerFlight;
    st.rise     = p.storeyHeight / static_cast<float>(st.risers);
    const float stairA = st.r.area();

    // --- The levels.
    static const char* kLabels[4][4] = {
        {"EG", "DG", "", ""}, {"EG", "OG", "DG", ""}, {"EG", "1. OG", "2. OG", "DG"}, {}};
    for (int L = 0; L < nLv; ++L) {
        LevelPlan lv;
        lv.label     = kLabels[nFull - 1][L];
        lv.z         = p.plinth + static_cast<float>(L) * p.storeyHeight;
        lv.attic     = (L == nFull);
        lv.loft      = lv.attic && !p.attic;
        lv.stairUp   = needStair && L < topLv;
        lv.stairDown = needStair && L >= 1 && L <= topLv;
        const bool hasStair = lv.stairUp || lv.stairDown;

        // Exterior walls: the long ones run the full width, the gables between.
        Wall wn{{0, 0, W, T}, true, Side::N, true};
        Wall ws{{0, D - T, W, D}, true, Side::S, true};
        Wall wwl{{0, T, T, D - T}, false, Side::W, true};
        Wall wel{{W - T, T, W, D - T}, false, Side::E, true};
        lv.walls = {wn, ws, wwl, wel};

        if (lv.loft) {
            PlanRoom r;
            r.type = RoomType::Abstell;
            r.name = "Dachboden";
            r.r    = {xi0, yN0, xi1, yS1};
            r.autoAdded = true;
            r.reachable = true;   // by a loft ladder, which is not the plan's business
            r.floorArea = r.r.area();
            lv.rooms.push_back(r);
            // A small window in each gable, under the ridge.
            for (const Wall* gw : {&lv.walls[0], &lv.walls[1]}) {
                const float head = std::min(1.9f, roofUnder(p, 0.5f * W - 0.4f) - lv.z - 0.2f);
                if (head - 1.1f > 0.4f)
                    lv.openings.push_back(makeWindow(*gw, 0.5f * W - 0.4f, 0.5f * W + 0.4f,
                                                     1.1f, head, Opening::Kind::Window, 0));
            }
            plan.levels.push_back(std::move(lv));
            continue;
        }

        const Sorted& s = sorted[L];
        auto addRoom = [&](RoomType t, std::string name, Rect r, float target, bool autoAdded) {
            PlanRoom pr;
            pr.type = t; pr.name = std::move(name); pr.r = r; pr.target = target;
            pr.autoAdded = autoAdded;
            lv.rooms.push_back(pr);
            return static_cast<int>(lv.rooms.size()) - 1;
        };

        // Hall with the stair.
        const int hallIdx = addRoom(RoomType::Diele,
                                    s.hallName.empty() ? (L == 0 ? "Diele" : "Flur") : s.hallName,
                                    {hx0, yN0, hx1, yN1}, 0.0f, false);
        lv.rooms[hallIdx].hall = true;
        lv.rooms[hallIdx].reachable = true;

        // Bearing spine between the bands.
        const Wall spine{{xi0, yN1, xi1, yS0}, true, Side::None, true};
        lv.walls.push_back(spine);

        // West column: its rooms stacked north to south, a door each into the hall.
        std::vector<int> westIdx;
        Wall westWall{};
        if (ww > 0.0f) {
            std::vector<RoomSpec> rs = s.west;
            const bool autoWest = rs.empty();
            if (autoWest) rs.push_back({RoomType::Abstell, "Abstellraum", ww * nd});
            std::vector<float> wts;
            for (const RoomSpec& r : rs) wts.push_back(r.area);
            const auto spans = splitSpan(yN0, yN1, wts, Pt, std::vector<float>(rs.size(), 1.2f));
            for (std::size_t i = 0; i < rs.size(); ++i) {
                westIdx.push_back(addRoom(rs[i].type, displayName(rs[i]),
                                          {xi0, spans[i].first, xi0 + ww, spans[i].second},
                                          autoWest ? 0.0f : rs[i].area, autoWest));
                if (i + 1 < rs.size())
                    lv.walls.push_back({{xi0, spans[i].second, xi0 + ww, spans[i + 1].first},
                                        true, Side::None, false});
            }
            westWall = {{xi0 + ww, yN0, xi0 + ww + Pt, yN1}, false, Side::None, false};
            lv.walls.push_back(westWall);
        }

        // East room, behind the bearing wall the stair stands against.
        int eastIdx = -1;
        Wall eastWall{};
        if (we > 0.0f) {
            const bool autoEast = !s.hasEast;
            const RoomSpec r = autoEast ? RoomSpec{RoomType::Abstell, "Abstellraum", we * nd} : s.east;
            eastIdx = addRoom(r.type, displayName(r), {xi1 - we, yN0, xi1, yN1},
                              autoEast ? 0.0f : r.area, autoEast);
            eastWall = {{xi1 - we - B, yN0, xi1 - we, yN1}, false, Side::None, true};
            lv.walls.push_back(eastWall);
        }

        // Main band.
        std::vector<int> mainIdx;
        if (s.main.empty()) {
            mainIdx.push_back(addRoom(lv.attic ? RoomType::Abstell : RoomType::Gast,
                                      lv.attic ? "Dachraum" : "Raum",
                                      {xi0, yS0, xi1, yS1}, 0.0f, true));
        } else {
            const auto& spans = mainSpans[L];
            for (std::size_t i = 0; i < s.main.size(); ++i) {
                mainIdx.push_back(addRoom(s.main[i].type, displayName(s.main[i]),
                                          {spans[i].first, yS0, spans[i].second, yS1},
                                          s.main[i].area, false));
                if (i + 1 < s.main.size())
                    lv.walls.push_back({{spans[i].second, yS0, spans[i + 1].first, yS1},
                                        false, Side::None, false});
            }
        }

        // --- Doors.
        const float doorW = 0.885f, wideDoorW = 1.01f, smallDoorW = 0.76f;
        for (int idx : westIdx) {
            PlanRoom& r = lv.rooms[idx];
            float dw = r.r.area() < 5.0f ? smallDoorW : doorW;
            dw = std::min(dw, r.r.h() - 0.2f);
            if (dw < 0.6f) continue;
            const float c = 0.5f * (r.r.y0 + r.r.y1);
            const int side = (ww >= dw + 0.25f) ? -1 : +1;   // into the room if it fits
            lv.openings.push_back(makeDoor(westWall, c - 0.5f * dw, c + 0.5f * dw, side,
                                           Opening::Kind::Door, 2.01f, true));
            r.reachable = true;
        }
        if (eastIdx >= 0) {
            PlanRoom& r = lv.rooms[eastIdx];
            // Beside the stair, not behind it: only the strip south of the U is hall.
            const float a = hasStair ? st.r.y1 + 0.10f : yN0 + 0.30f;
            const float b = yN1 - 0.10f;
            const float dw = std::min(doorW, b - a);
            if (dw >= 0.6f) {
                const float c = 0.5f * (a + b);
                lv.openings.push_back(makeDoor(eastWall, c - 0.5f * dw, c + 0.5f * dw, +1,
                                               Opening::Kind::Door, 2.01f, true));
                r.reachable = true;
            }
        }
        for (std::size_t i = 0; i < mainIdx.size(); ++i) {
            PlanRoom& r = lv.rooms[mainIdx[i]];
            const float a = std::max(r.r.x0, hx0) + 0.15f;
            const float b = std::min(r.r.x1, hx1) - 0.15f;
            const bool living = r.type == RoomType::Wohnen || r.type == RoomType::Essen;
            // The living room gets the wide door where there is room for one.
            const float dw = (living && b - a >= wideDoorW) ? wideDoorW : doorW;
            if (b - a >= dw) {
                const float c = 0.5f * (a + b);
                lv.openings.push_back(makeDoor(spine, c - 0.5f * dw, c + 0.5f * dw, +1,
                                               Opening::Kind::Door, 2.01f, true));
                r.reachable = true;
            }
        }
        // A main room the hall does not reach: through its neighbour, if both are
        // rooms one may walk through (living and dining).
        for (std::size_t i = 0; i < mainIdx.size(); ++i) {
            PlanRoom& r = lv.rooms[mainIdx[i]];
            if (r.reachable) continue;
            for (int nb : {static_cast<int>(i) - 1, static_cast<int>(i) + 1}) {
                if (nb < 0 || nb >= static_cast<int>(mainIdx.size())) continue;
                const PlanRoom& o = lv.rooms[mainIdx[nb]];
                const bool open = (r.type == RoomType::Wohnen || r.type == RoomType::Essen) &&
                                  (o.type == RoomType::Wohnen || o.type == RoomType::Essen);
                if (!o.reachable || !open) continue;
                const float px0 = nb < static_cast<int>(i) ? o.r.x1 : r.r.x1;
                const Wall part{{px0, yS0, px0 + Pt, yS1}, false, Side::None, false};
                const float c = 0.5f * (yS0 + yS1);
                lv.openings.push_back(makeDoor(part, c - 0.9f, c + 0.9f, +1,
                                               Opening::Kind::Pass, 2.26f, false));
                r.reachable = true;
                break;
            }
        }
        // Kitchen straight into the living/dining room below it: a pass without
        // a leaf, which is what makes the kitchen part of the family room.
        if (eastIdx >= 0 && lv.rooms[eastIdx].type == RoomType::Kueche) {
            const PlanRoom& k = lv.rooms[eastIdx];
            for (int mi : mainIdx) {
                const PlanRoom& m = lv.rooms[mi];
                if (m.type != RoomType::Wohnen && m.type != RoomType::Essen) continue;
                const float a = std::max(k.r.x0, m.r.x0) + 0.25f;
                const float b = std::min(k.r.x1, m.r.x1) - 0.25f;
                if (b - a < 1.01f) continue;
                lv.openings.push_back(makeDoor(spine, a, a + 1.01f, +1, Opening::Kind::Pass,
                                               2.01f, false));
                break;
            }
        }
        // Front door: north wall, in the free strip at the foot of the stair.
        if (L == 0) {
            const float a = hx0 + 0.05f;
            const float b = (hasStair ? st.r.x0 : hx1) - 0.05f;
            const float dw = std::min(1.01f, b - a);
            if (dw >= 0.8f) {
                const float c = hasStair ? 0.5f * (a + b) : 0.5f * (hx0 + hx1);
                lv.openings.push_back(makeDoor(lv.walls[0], c - 0.5f * dw, c + 0.5f * dw, +1,
                                               Opening::Kind::EntryDoor, 2.135f, true));
            }
        }

        // --- Windows.
        auto touches = [&](const PlanRoom& r, Side sd) {
            switch (sd) {
                case Side::N: return r.r.y0 <= T + kEps;
                case Side::S: return r.r.y1 >= D - T - kEps;
                case Side::W: return r.r.x0 <= T + kEps;
                case Side::E: return r.r.x1 >= W - T - kEps;
                default: return false;
            }
        };
        auto wallOf = [&](Side sd) -> const Wall& {
            return lv.walls[sd == Side::N ? 0 : sd == Side::S ? 1 : sd == Side::W ? 2 : 3];
        };
        // Tallest head a window may have on a gable at [a,b], under the roof.
        auto headAt = [&](Side sd, float a, float b, float head) {
            if (!lv.attic || (sd != Side::N && sd != Side::S)) return head;
            return std::min(head, std::min(roofUnder(p, a), roofUnder(p, b)) - lv.z - 0.15f);
        };
        // Place windows on side `sd` of room `ri` until `need` m^2 of opening
        // are there (or the wall is full). Returns what is still missing.
        auto glaze = [&](int ri, Side sd, float need, float sill, float head, int maxN) {
            const PlanRoom& r = lv.rooms[ri];
            if (need <= 0.05f || !touches(r, sd)) return need;
            if (lv.attic && (sd == Side::W || sd == Side::E)) return need;   // eaves
            const Wall& w = wallOf(sd);
            const float ra = (sd == Side::N || sd == Side::S) ? r.r.x0 : r.r.y0;
            const float rb = (sd == Side::N || sd == Side::S) ? r.r.x1 : r.r.y1;
            int placed = 0;
            while (placed < maxN && need > 0.05f) {
                auto [fa, fb] = largestFree(ra + 0.30f, rb - 0.30f, lv.openings, w, 0.45f);
                const float span = fb - fa;
                if (span < 0.6f) break;
                const float h = headAt(sd, 0.5f * (fa + fb) - 0.5f, 0.5f * (fa + fb) + 0.5f, head) - sill;
                if (h < 0.6f) break;
                // As few windows as carry the need at no more than 2.2 m each,
                // spread evenly over the free stretch with wall between them.
                int n = std::clamp(static_cast<int>(std::ceil(need / h / 2.2f)), 1, maxN - placed);
                while (n > 1 && span < static_cast<float>(2 * n - 1) * 0.6f) --n;
                float width = std::max(0.6f, std::min(need / h / static_cast<float>(n), 2.2f));
                width = std::min(width, (span - static_cast<float>(n - 1) * 0.6f) / static_cast<float>(n));
                int got = 0;
                for (int k = 0; k < n; ++k) {
                    const float c = fa + (static_cast<float>(k) + 0.5f) * span / static_cast<float>(n);
                    const float a = c - 0.5f * width, b = c + 0.5f * width;
                    const float hh = headAt(sd, a, b, head);
                    if (hh - sill < 0.6f) continue;
                    lv.openings.push_back(makeWindow(w, a, b, sill, hh, Opening::Kind::Window, ri));
                    need -= width * (hh - sill);
                    ++got;
                }
                if (got == 0) break;
                placed += n;
            }
            return need;
        };
        for (int ri = 0; ri < static_cast<int>(lv.rooms.size()); ++ri) {
            const PlanRoom r = lv.rooms[ri];
            if (r.hall) continue;
            float sill = 0.90f, head = 2.25f, need = 0.0f;
            switch (r.type) {
                case RoomType::Bad: case RoomType::GaesteBad:
                    sill = 1.40f; need = 0.65f; break;
                case RoomType::HWR:
                    sill = 1.20f; need = 0.70f; break;
                case RoomType::Kueche:
                    sill = 1.05f; need = r.r.area() / 8.0f * 1.2f; break;
                case RoomType::Abstell: case RoomType::Garderobe: case RoomType::Ankleide:
                    need = 0.0f; break;
                default:
                    need = r.r.area() / 8.0f * 1.2f; break;
            }
            if (need <= 0.0f) continue;
            const bool mainBand = r.r.y0 >= yS0 - kEps;
            // The garden door: the living room on the ground floor opens onto the
            // terrace through a glazed double door in the middle of its facade.
            if (L == 0 && r.type == RoomType::Wohnen && touches(r, Side::S)) {
                const float c = 0.5f * (r.r.x0 + r.r.x1), fw = std::min(2.0f, r.r.w() - 1.0f);
                if (fw >= 1.0f) {
                    lv.openings.push_back(makeWindow(wallOf(Side::S), c - 0.5f * fw, c + 0.5f * fw,
                                                     0.0f, head, Opening::Kind::FrenchDoor, ri));
                    need -= fw * head;
                }
            }
            const Side first  = mainBand ? Side::S : Side::N;
            need = glaze(ri, first, need, sill, head, 2);
            for (Side sd : {Side::W, Side::E, mainBand ? Side::N : Side::S})
                need = glaze(ri, sd, need, sill, head, 1);
            // Attic rooms against the eaves light through the roof.
            if (lv.attic && need > 0.05f) {
                for (Side sd : {Side::W, Side::E}) {
                    if (!touches(r, sd)) continue;
                    const float run = 1.40f * std::cos(glm::radians(p.roofPitch));
                    const float xa = sd == Side::W ? T + 0.25f : W - T - 0.25f - run;
                    for (int n = 0; n < 2 && need > 0.05f; ++n) {
                        const float slots = (r.r.h() - 0.4f) / 1.3f;
                        if (slots < static_cast<float>(n + 1)) break;
                        const float c = r.r.y0 + (static_cast<float>(n) + 0.5f) *
                                                     (r.r.h() / std::min(2.0f, std::floor(slots)));
                        RoofWindow rw;
                        rw.r    = {xa, c - 0.39f, xa + run, c + 0.39f};
                        rw.east = sd == Side::E;
                        rw.room = ri;
                        rw.area = 0.78f * 1.40f;
                        lv.roofWindows.push_back(rw);
                        need -= rw.area;
                    }
                }
            }
        }
        // The stairwell gets a window over the landing on the upper storeys.
        if (L >= 1 && hasStair) {
            const float c = 0.5f * (st.landingX + st.r.x1) - 0.35f, a = c - 0.45f, b = c + 0.45f;
            const float hh = headAt(Side::N, a, b, 2.25f);
            if (hh - 0.9f >= 0.6f)
                lv.openings.push_back(makeWindow(lv.walls[0], a, b, 0.9f, hh,
                                                 Opening::Kind::Window, hallIdx));
        }

        plan.levels.push_back(std::move(lv));
    }

    // --- Areas and checks.
    for (LevelPlan& lv : plan.levels) {
        const bool hasStair = lv.stairUp || lv.stairDown;
        for (int ri = 0; ri < static_cast<int>(lv.rooms.size()); ++ri) {
            PlanRoom& r = lv.rooms[ri];
            r.floorArea = r.r.area();
            if (lv.loft) { r.livingArea = 0.0f; continue; }
            r.livingArea = lv.attic ? atticLivingArea(p, r.r) : r.floorArea;
            if (r.hall && hasStair) {
                const float stairLiving = lv.attic ? atticLivingArea(p, st.r) : stairA;
                r.livingArea = std::max(0.0f, r.livingArea - stairLiving);
            }
            r.daylight = 0.0f;
            for (const Opening& o : lv.openings)
                if (o.room == ri && (o.kind == Opening::Kind::Window || o.kind == Opening::Kind::FrenchDoor))
                    r.daylight += o.area();
            for (const RoofWindow& rw : lv.roofWindows)
                if (rw.room == ri) r.daylight += rw.area;
        }
        lv.living = 0.0f;
        for (const PlanRoom& r : lv.rooms) lv.living += r.livingArea;
        plan.living += lv.living;
    }
    plan.grossFloor = W * D * static_cast<float>(nFull + (p.attic ? 1 : 0));
    plan.eaveZ  = roofUnder(p, -p.eaveOverhang) + roofThicknessV(p);
    plan.ridgeZ = roofUnder(p, 0.5f * W) + roofThicknessV(p);

    for (const LevelPlan& lv : plan.levels) {
        if (lv.loft) continue;
        for (const PlanRoom& r : lv.rooms) {
            const std::string who = lv.label + " " + r.name;
            if (!r.reachable)
                warn.push_back(who + ": no door to the hall.");
            if (isHabitable(r.type) && r.daylight < r.floorArea / 8.0f - 0.01f)
                warn.push_back(who + ": windows " + fmt1(r.daylight) + " m\xC2\xB2 -- below 1/8 of " +
                               fmt1(r.floorArea) + " m\xC2\xB2 floor.");
            if (r.target > 0.0f && !r.hall &&
                std::abs(r.floorArea - r.target) > 0.30f * r.target)
                info.push_back(who + ": " + fmt1(r.floorArea) + " m\xC2\xB2 instead of " +
                               fmt1(r.target) + " m\xC2\xB2.");
            if (r.autoAdded && !r.hall)
                info.push_back(who + " was added to fill the plan.");
        }
    }
    if (needStair) {
        const float blondel = 2.0f * st.rise * 100.0f + kTread * 100.0f;   // cm
        if (blondel < 59.0f || blondel > 65.0f)
            warn.push_back("Stair: 2 risers + 1 tread = " + fmt1(blondel) +
                           " cm, outside the comfortable 59..65 cm.");
    }
    plan.notes.insert(plan.notes.end(), warn.begin(), warn.end());
    plan.warnings = static_cast<int>(plan.notes.size());
    plan.notes.insert(plan.notes.end(), info.begin(), info.end());
    return plan;
}

// --- Materials ---------------------------------------------------------------

namespace {

AssetId ensureMaterial(std::vector<MaterialDef>& mats, const std::string& name,
                       glm::vec3 albedo, float refl, float rough, bool glass = false,
                       float opacity = 1.0f) {
    for (MaterialDef& m : mats) {
        if (m.name != name) continue;
        m.albedo = albedo; m.reflectivity = refl; m.roughness = rough;
        m.glass = glass; m.opacity = opacity;
        if (!m.assetId.valid()) m.assetId = AssetId::generate();
        return m.assetId;
    }
    MaterialDef md;
    md.assetId      = AssetId::generate();
    md.name         = name;
    md.albedo       = albedo;
    md.reflectivity = refl;
    md.roughness    = rough;
    md.glass        = glass;
    md.opacity      = opacity;
    mats.push_back(md);
    return md.assetId;
}

} // namespace

Palette ensurePalette(std::vector<MaterialDef>& mats, const Params& p) {
    Palette pal;
    pal.facade   = ensureMaterial(mats, "House Facade", p.facadeColor, 0.0f, 0.85f);
    pal.plinth   = ensureMaterial(mats, "House Plinth", p.plinthColor, 0.0f, 0.80f);
    pal.roof     = ensureMaterial(mats, "House Roof", p.roofColor, 0.0f, 0.70f);
    pal.timber   = ensureMaterial(mats, "House Timber", {0.42f, 0.29f, 0.18f}, 0.0f, 0.65f);
    pal.interior = ensureMaterial(mats, "House Interior", {0.90f, 0.89f, 0.86f}, 0.0f, 0.90f);
    pal.floor    = ensureMaterial(mats, "House Floor", p.floorColor, 0.0f, 0.45f);
    pal.frame    = ensureMaterial(mats, "House Frame", p.frameColor, 0.2f, 0.40f);
    // The same glass the project seeds by default: the glass flag gives the
    // Fresnel alpha, the low opacity keeps the pane clear head-on.
    pal.glass    = ensureMaterial(mats, "House Glass", {0.85f, 0.92f, 0.95f}, 0.5f, 0.03f,
                                  true, 0.28f);
    pal.door     = ensureMaterial(mats, "House Door", {0.93f, 0.93f, 0.91f}, 0.0f, 0.35f);
    pal.stair    = ensureMaterial(mats, "House Stair", p.floorColor * 0.85f, 0.0f, 0.50f);
    return pal;
}

// --- Geometry ----------------------------------------------------------------

namespace {

using MatFn = std::function<AssetId(const glm::vec3& n)>;

// Collects faces into one EditMesh in the house's local frame, in PLAN
// coordinates on the way in (x east, y south, z up) so the rest of this file can
// think in plan metres.
struct Builder {
    EditMesh m;
    float    W = 0.0f, D = 0.0f;

    glm::vec3 P(float x, float y, float z) const { return {x - 0.5f * W, z, y - 0.5f * D}; }

    // One convex polygon, wound so it faces `outward` (CCW seen from outside is
    // what EditMesh -- and the back-face culling behind it -- expects).
    void face(std::vector<glm::vec3> pts, const glm::vec3& outward, AssetId mat) {
        std::vector<glm::vec3> clean;
        for (const glm::vec3& v : pts)
            if (clean.empty() || glm::length(v - clean.back()) > 1e-5f) clean.push_back(v);
        while (clean.size() > 1 && glm::length(clean.front() - clean.back()) <= 1e-5f)
            clean.pop_back();
        if (clean.size() < 3) return;
        glm::vec3 n(0.0f);
        for (std::size_t i = 0; i < clean.size(); ++i) {
            const glm::vec3& a = clean[i];
            const glm::vec3& b = clean[(i + 1) % clean.size()];
            n += glm::vec3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x),
                           (a.x - b.x) * (a.y + b.y));
        }
        if (glm::dot(n, n) < 1e-12f) return;
        if (glm::dot(n, outward) < 0.0f) std::reverse(clean.begin(), clean.end());
        std::vector<int> f;
        for (const glm::vec3& v : clean) {
            f.push_back(static_cast<int>(m.verts.size()));
            m.verts.push_back(v);
        }
        m.faces.push_back(std::move(f));
        m.faceMat.push_back(mat);
    }

    // A convex hexahedron: `b` the bottom ring, `t` the top ring above it, in
    // the same order. Six faces, each wound outward from the solid's centre and
    // dressed by `mat` from its outward direction.
    void hexa(const std::array<glm::vec3, 4>& b, const std::array<glm::vec3, 4>& t,
              const MatFn& mat) {
        glm::vec3 c(0.0f);
        for (int i = 0; i < 4; ++i) c += b[i] + t[i];
        c *= 0.125f;
        auto put = [&](std::vector<glm::vec3> q) {
            glm::vec3 fc(0.0f);
            for (const glm::vec3& v : q) fc += v;
            fc /= static_cast<float>(q.size());
            glm::vec3 out = fc - c;
            if (glm::dot(out, out) < 1e-12f) return;
            // The material follows the face's true normal, not the centre offset:
            // on a sloped top the two differ, and it is the slope that is "roof".
            glm::vec3 n(0.0f);
            for (std::size_t i = 0; i < q.size(); ++i) {
                const glm::vec3& a = q[i];
                const glm::vec3& bq = q[(i + 1) % q.size()];
                n += glm::vec3((a.y - bq.y) * (a.z + bq.z), (a.z - bq.z) * (a.x + bq.x),
                               (a.x - bq.x) * (a.y + bq.y));
            }
            if (glm::dot(n, n) < 1e-12f) return;
            n = glm::normalize(n);
            if (glm::dot(n, out) < 0.0f) n = -n;
            face(std::move(q), out, mat(n));
        };
        put({b[0], b[1], b[2], b[3]});
        put({t[0], t[1], t[2], t[3]});
        for (int i = 0; i < 4; ++i) {
            const int j = (i + 1) % 4;
            put({b[i], b[j], t[j], t[i]});
        }
    }

    // An axis-aligned box in plan coordinates.
    void box(float x0, float x1, float y0, float y1, float z0, float z1, const MatFn& mat) {
        if (x1 - x0 < 1e-4f || y1 - y0 < 1e-4f || z1 - z0 < 1e-4f) return;
        hexa({P(x0, y0, z0), P(x1, y0, z0), P(x1, y1, z0), P(x0, y1, z0)},
             {P(x0, y0, z1), P(x1, y0, z1), P(x1, y1, z1), P(x0, y1, z1)}, mat);
    }

    // A box whose top follows `top(x)` corner by corner (a wall under a roof).
    void slopedBox(float x0, float x1, float y0, float y1, float z0,
                   const std::function<float(float)>& top, const MatFn& mat) {
        const float t0 = std::max(top(x0), z0), t1 = std::max(top(x1), z0);
        if (t0 - z0 < 0.01f && t1 - z0 < 0.01f) return;
        hexa({P(x0, y0, z0), P(x1, y0, z0), P(x1, y1, z0), P(x0, y1, z0)},
             {P(x0, y0, t0), P(x1, y0, t1), P(x1, y1, t1), P(x0, y1, t0)}, mat);
    }
};

// World direction pointing out of the house through side `s`.
glm::vec3 sideOut(Side s) {
    switch (s) {
        case Side::N: return {0, 0, -1};
        case Side::S: return {0, 0, 1};
        case Side::W: return {-1, 0, 0};
        case Side::E: return {1, 0, 0};
        default: return {0, 0, 0};
    }
}

bool inWall(const Opening& o, const Wall& w) {
    if (o.alongX != w.alongX) return false;
    if (w.alongX)
        return o.r.y0 >= w.r.y0 - kEps && o.r.y1 <= w.r.y1 + kEps &&
               o.r.x0 >= w.r.x0 - kEps && o.r.x1 <= w.r.x1 + kEps;
    return o.r.x0 >= w.r.x0 - kEps && o.r.x1 <= w.r.x1 + kEps &&
           o.r.y0 >= w.r.y0 - kEps && o.r.y1 <= w.r.y1 + kEps;
}

// One wall of one storey, cut around its openings. The wall is sliced along
// its length at every opening edge (and wherever its top changes slope); each
// slice is solid from the floor up, minus the opening that spans it -- a
// parapet below the sill and a lintel above the head. The slices are convex,
// which is all an EditMesh face can be.
void buildWall(Builder& bd, const Wall& w, const std::vector<Opening>& ops, float z0,
               const std::function<float(float)>& top, std::vector<float> breaks,
               const MatFn& mat) {
    const float s0 = w.alongX ? w.r.x0 : w.r.y0, s1 = w.alongX ? w.r.x1 : w.r.y1;
    std::vector<const Opening*> mine;
    for (const Opening& o : ops) if (inWall(o, w)) mine.push_back(&o);
    breaks.push_back(s0);
    breaks.push_back(s1);
    for (const Opening* o : mine) {
        breaks.push_back(w.alongX ? o->r.x0 : o->r.y0);
        breaks.push_back(w.alongX ? o->r.x1 : o->r.y1);
    }
    std::sort(breaks.begin(), breaks.end());
    std::vector<float> cuts;
    for (float b : breaks)
        if (b >= s0 - kEps && b <= s1 + kEps && (cuts.empty() || b - cuts.back() > 1e-3f))
            cuts.push_back(clampf(b, s0, s1));
    for (std::size_t i = 0; i + 1 < cuts.size(); ++i) {
        const float a = cuts[i], b = cuts[i + 1], mid = 0.5f * (a + b);
        const Opening* hole = nullptr;
        for (const Opening* o : mine) {
            const float oa = w.alongX ? o->r.x0 : o->r.y0, ob = w.alongX ? o->r.x1 : o->r.y1;
            if (mid > oa && mid < ob) { hole = o; break; }
        }
        auto piece = [&](float zlo, bool sloped, float zhi) {
            const float x0 = w.alongX ? a : w.r.x0, x1 = w.alongX ? b : w.r.x1;
            const float y0 = w.alongX ? w.r.y0 : a, y1 = w.alongX ? w.r.y1 : b;
            if (sloped) bd.slopedBox(x0, x1, y0, y1, zlo, top, mat);
            else if (zhi - zlo > 0.005f) bd.box(x0, x1, y0, y1, zlo, zhi, mat);
        };
        if (!hole) { piece(z0, true, 0.0f); continue; }
        if (hole->sill > 0.005f) piece(z0, false, z0 + hole->sill);
        piece(z0 + hole->head, true, 0.0f);
    }
}

// Split `r` minus `hole` into up to four rectangles (a slab with a stairwell).
std::vector<Rect> minusHole(const Rect& r, const Rect* hole) {
    if (!hole) return {r};
    std::vector<Rect> out;
    if (hole->y0 > r.y0) out.push_back({r.x0, r.y0, r.x1, hole->y0});
    if (hole->y1 < r.y1) out.push_back({r.x0, hole->y1, r.x1, r.y1});
    if (hole->x0 > r.x0) out.push_back({r.x0, hole->y0, hole->x0, hole->y1});
    if (hole->x1 < r.x1) out.push_back({hole->x1, hole->y0, r.x1, hole->y1});
    return out;
}

struct Meshes {
    Builder slabs, stair, roof, windows, doors, outside;
    std::vector<std::pair<std::string, Builder>> walls;
};

Meshes buildMeshes(const Plan& plan, const Palette& pal) {
    const Params& p = plan.params;
    const float W = p.width, D = p.depth, T = p.outerWall;
    const float slabT = p.storeyHeight - p.clearHeight;
    const int   nLv = static_cast<int>(plan.levels.size());
    const int   atticLv = nLv - 1;
    Meshes ms;
    for (Builder* b : {&ms.slabs, &ms.stair, &ms.roof, &ms.windows, &ms.doors, &ms.outside}) {
        b->W = W; b->D = D;
    }
    const auto flat = [](AssetId id) { return [id](const glm::vec3&) { return id; }; };
    const Stair& st = plan.stair;

    // --- Slabs: the ground slab, one per upper floor, the attic ceiling.
    ms.slabs.box(0, W, 0, D, -0.30f, p.plinth, [&](const glm::vec3& n) {
        return n.y > 0.5f ? pal.floor : pal.plinth;
    });
    for (int L = 1; L < nLv; ++L) {
        if (L > p.cutLevel) break;
        const LevelPlan& lv = plan.levels[L];
        const Rect* hole = (plan.hasStair && lv.stairDown) ? &st.r : nullptr;
        for (const Rect& r : minusHole({0, 0, W, D}, hole)) {
            ms.slabs.box(r.x0, r.x1, r.y0, r.y1, lv.z - slabT, lv.z, [&](const glm::vec3& n) {
                if (n.y > 0.5f) return pal.floor;
                if (n.y < -0.5f) return pal.interior;
                return pal.facade;   // the edge band on the facade; the stairwell
            });                      // faces are hidden by the stair and railing
        }
    }
    const LevelPlan& attic = plan.levels[atticLv];
    const float tanP = roofTan(p);
    const float kinkD = (p.clearHeight - p.kneeWall) / tanP;   // eave to full height
    if (!attic.loft && atticLv <= p.cutLevel && kinkD > 0.0f) {
        const float z = attic.z + p.clearHeight;
        ms.slabs.box(T + kinkD, W - T - kinkD, T, D - T, z, z + 0.20f, flat(pal.interior));
    }

    // --- Walls, storey by storey.
    for (int L = 0; L < nLv && L <= p.cutLevel; ++L) {
        const LevelPlan& lv = plan.levels[L];
        Builder bd;
        bd.W = W; bd.D = D;
        const float z0 = lv.z;
        for (const Wall& w : lv.walls) {
            std::function<float(float)> top;
            std::vector<float> breaks;
            if (!lv.attic) {
                const float zt = z0 + p.clearHeight;
                top = [zt](float) { return zt; };
            } else if (w.exterior != Side::None) {
                top = [&p](float x) { return roofUnder(p, x); };
                if (w.alongX) breaks.push_back(0.5f * W);
            } else {
                const float ceil = z0 + p.clearHeight;
                top = [&p, ceil](float x) { return std::min(ceil, roofUnder(p, x)); };
                if (w.alongX) {
                    breaks.push_back(0.5f * W);
                    breaks.push_back(T + kinkD);
                    breaks.push_back(W - T - kinkD);
                }
            }
            const glm::vec3 outDir = sideOut(w.exterior);
            const bool ext = w.exterior != Side::None;
            buildWall(bd, w, lv.openings, z0, top, breaks, [&](const glm::vec3& n) {
                if (!ext) return pal.interior;
                return glm::dot(n, outDir) < -0.5f ? pal.interior : pal.facade;
            });
        }
        ms.walls.push_back({"Walls " + lv.label, std::move(bd)});

        // Windows and glazed doors: frame, glass and an outside sill, set back
        // 12 cm from the facade.
        for (const Opening& o : lv.openings) {
            const bool glazed = o.kind == Opening::Kind::Window || o.kind == Opening::Kind::FrenchDoor;
            if (!glazed || o.exterior == Side::None) continue;
            const Side sd = o.exterior;
            // Wall-frame helper: s along the wall, t through it (0 = outside face).
            const float outer = sd == Side::N ? o.r.y0 : sd == Side::S ? o.r.y1
                              : sd == Side::W ? o.r.x0 : o.r.x1;
            const float inward = (sd == Side::N || sd == Side::W) ? 1.0f : -1.0f;
            const float sa = o.alongX ? o.r.x0 : o.r.y0, sb = o.alongX ? o.r.x1 : o.r.y1;
            auto wbox = [&](Builder& b, float s0, float s1, float t0, float t1, float z0b,
                            float z1b, const MatFn& mf) {
                const float n0 = outer + inward * t0, n1 = outer + inward * t1;
                const float lo = std::min(n0, n1), hi = std::max(n0, n1);
                if (o.alongX) b.box(s0, s1, lo, hi, z0b, z1b, mf);
                else          b.box(lo, hi, s0, s1, z0b, z1b, mf);
            };
            const float fw = 0.07f, t0 = 0.12f, t1 = 0.19f;
            const float zs = z0 + o.sill, zh = z0 + o.head;
            const auto fr = flat(pal.frame);
            wbox(ms.windows, sa, sa + fw, t0, t1, zs, zh, fr);
            wbox(ms.windows, sb - fw, sb, t0, t1, zs, zh, fr);
            wbox(ms.windows, sa + fw, sb - fw, t0, t1, zh - fw, zh, fr);
            const bool door = o.kind == Opening::Kind::FrenchDoor;
            if (!door) wbox(ms.windows, sa + fw, sb - fw, t0, t1, zs, zs + fw, fr);
            const float gz0 = door ? zs + 0.02f : zs + fw;
            if (sb - sa > 1.3f) {
                const float c = 0.5f * (sa + sb);
                wbox(ms.windows, c - 0.5f * fw, c + 0.5f * fw, t0, t1, gz0, zh - fw, fr);
            }
            wbox(ms.windows, sa + fw, sb - fw, 0.15f, 0.162f, gz0, zh - fw, flat(pal.glass));
            if (o.sill > 0.05f)   // Fensterbank, 5 cm proud of the render
                wbox(ms.windows, sa - 0.03f, sb + 0.03f, -0.05f, t0, zs - 0.03f, zs, fr);
        }
        // Door leaves, standing open at 90 degrees.
        for (const Opening& o : lv.openings) {
            if (!o.hasLeaf) continue;
            const AssetId m = o.kind == Opening::Kind::EntryDoor ? pal.timber : pal.door;
            const float zt = z0 + o.head - 0.01f;
            if (std::abs(o.hinge.x - o.open.x) < 1e-4f) {
                const float y0 = std::min(o.hinge.y, o.open.y), y1 = std::max(o.hinge.y, o.open.y);
                ms.doors.box(o.hinge.x - 0.02f, o.hinge.x + 0.02f, y0 + 0.01f, y1 - 0.01f, z0 + 0.01f,
                             zt, flat(m));
            } else {
                const float x0 = std::min(o.hinge.x, o.open.x), x1 = std::max(o.hinge.x, o.open.x);
                ms.doors.box(x0 + 0.01f, x1 - 0.01f, o.hinge.y - 0.02f, o.hinge.y + 0.02f, z0 + 0.01f,
                             zt, flat(m));
            }
        }
        // Entrance: canopy and two steps up to the plinth.
        for (const Opening& o : lv.openings) {
            if (o.kind != Opening::Kind::EntryDoor) continue;
            const float x0 = o.r.x0 - 0.35f, x1 = o.r.x1 + 0.35f;
            ms.outside.box(x0, x1, -1.10f, 0.0f, z0 + 2.45f, z0 + 2.55f, flat(pal.frame));
            if (p.plinth > 0.05f) {
                ms.outside.box(x0, x1, -1.10f, 0.0f, 0.0f, 0.5f * p.plinth, flat(pal.plinth));
                ms.outside.box(x0, x1, -0.55f, 0.0f, 0.5f * p.plinth, p.plinth, flat(pal.plinth));
            }
        }
        // Terrace in front of the garden door.
        if (L == 0 && p.terrace)
            for (const Opening& o : lv.openings)
                if (o.kind == Opening::Kind::FrenchDoor) {
                    const float x0 = std::max(0.3f, o.r.x0 - 1.6f), x1 = std::min(W - 0.3f, o.r.x1 + 1.6f);
                    ms.outside.box(x0, x1, D, D + 2.8f, 0.0f, std::max(0.05f, p.plinth - 0.02f),
                                   flat(pal.timber));
                    break;
                }
    }

    // --- Stair: 8 + 8 risers per storey, steps as 18 cm slabs under each tread
    // so a flight overhead leaves the headroom of the one beneath it.
    if (plan.hasStair) {
        const auto sm = flat(pal.stair);
        for (int L = 0; L < nLv; ++L) {
            const LevelPlan& lv = plan.levels[L];
            if (L > p.cutLevel) break;
            if (lv.stairUp) {
                const float z = lv.z, rise = st.rise;
                const float yS0 = st.r.y1 - kFlightW, yS1 = st.r.y1;     // first flight
                const float yN0 = st.r.y0, yN1 = st.r.y0 + kFlightW;     // second flight
                for (int i = 0; i < kStepsPerFlight; ++i) {
                    const float top = z + static_cast<float>(i + 1) * rise;
                    const float x0 = st.r.x0 + static_cast<float>(i) * kTread;
                    ms.stair.box(x0, x0 + kTread, yS0, yS1, std::max(z, top - rise - 0.18f), top, sm);
                }
                const float land = z + static_cast<float>(kStepsPerFlight) * rise;
                ms.stair.box(st.landingX, st.r.x1, st.r.y0, st.r.y1, land - 0.20f, land, sm);
                for (int j = 0; j < kStepsPerFlight; ++j) {
                    const float top = land + static_cast<float>(j + 1) * rise;
                    const float x1 = st.landingX - static_cast<float>(j) * kTread;
                    ms.stair.box(x1 - kTread, x1, yN0, yN1, top - rise - 0.18f, top, sm);
                }
            }
            // Railing round the stairwell on every floor the stair arrives at.
            if (lv.stairDown) {
                const float z = lv.z;
                const auto fr = flat(pal.frame);
                auto rail = [&](float x0, float x1, float y0, float y1) {
                    const bool alongX = (x1 - x0) > (y1 - y0);
                    ms.stair.box(x0, x1, y0, y1, z + 0.95f, z + 1.00f, fr);
                    ms.stair.box(x0, x1, y0, y1, z + 0.48f, z + 0.52f, fr);
                    const float len = alongX ? x1 - x0 : y1 - y0;
                    const int posts = std::max(2, static_cast<int>(std::ceil(len / 1.1f)) + 1);
                    for (int k = 0; k < posts; ++k) {
                        const float t = static_cast<float>(k) / static_cast<float>(posts - 1);
                        const float c = (alongX ? x0 : y0) + t * (len - 0.04f);
                        if (alongX) ms.stair.box(c, c + 0.04f, y0, y1, z, z + 0.95f, fr);
                        else        ms.stair.box(x0, x1, c, c + 0.04f, z, z + 0.95f, fr);
                    }
                };
                rail(st.r.x0, st.r.x1, st.r.y1 - 0.04f, st.r.y1);
                if (!lv.stairUp)   // top floor: the first flight's well is open too
                    rail(st.r.x0, st.r.x0 + 0.04f, st.r.y1 - kFlightW, st.r.y1 - 0.04f);
            }
        }
    }

    // --- Roof: two slabs meeting at the ridge, gutters, downpipes, roof windows.
    if (p.roof && atticLv <= p.cutLevel) {
        const float thk = roofThicknessV(p);
        const float y0 = -p.gableOverhang, y1 = D + p.gableOverhang;
        const auto roofMat = [&](const glm::vec3& n) {
            if (n.y > 0.3f) return pal.roof;
            return pal.timber;   // soffit, fascia and verge boards
        };
        for (int side = 0; side < 2; ++side) {
            const float xa = side == 0 ? -p.eaveOverhang : 0.5f * W;
            const float xb = side == 0 ? 0.5f * W : W + p.eaveOverhang;
            const float ua = roofUnder(p, xa), ub = roofUnder(p, xb);
            ms.roof.hexa({ms.roof.P(xa, y0, ua), ms.roof.P(xb, y0, ub), ms.roof.P(xb, y1, ub),
                          ms.roof.P(xa, y1, ua)},
                         {ms.roof.P(xa, y0, ua + thk), ms.roof.P(xb, y0, ub + thk),
                          ms.roof.P(xb, y1, ub + thk), ms.roof.P(xa, y1, ua + thk)},
                         roofMat);
            // Gutter along the eave and a downpipe at each end of it.
            const float xe  = side == 0 ? -p.eaveOverhang : W + p.eaveOverhang;
            const float gz  = roofUnder(p, xe);
            const float dir = side == 0 ? -1.0f : 1.0f;
            const auto fr = flat(pal.frame);
            ms.roof.box(std::min(xe, xe + dir * 0.14f), std::max(xe, xe + dir * 0.14f), y0, y1,
                        gz - 0.10f, gz + 0.04f, fr);
            for (float yp : {0.25f, D - 0.25f}) {
                const float wallX = side == 0 ? -0.04f : W + 0.04f;
                const float px0 = side == 0 ? wallX - 0.09f : wallX, px1 = px0 + 0.09f;
                ms.roof.box(px0, px1, yp - 0.045f, yp + 0.045f, 0.0f, gz - 0.22f, fr);
                const float cx0 = std::min(xe + dir * 0.07f, wallX), cx1 = std::max(xe + dir * 0.07f, wallX);
                ms.roof.box(cx0, cx1, yp - 0.045f, yp + 0.045f, gz - 0.30f, gz - 0.22f, fr);
            }
        }
        const float rz = roofUnder(p, 0.5f * W) + thk;
        ms.roof.box(0.5f * W - 0.13f, 0.5f * W + 0.13f, y0, y1, rz - 0.06f, rz + 0.07f,
                    flat(pal.roof));
        for (const RoofWindow& rw : attic.roofWindows) {
            auto onRoof = [&](float x) { return roofUnder(p, x) + thk; };
            const auto fr = flat(pal.frame), gl = flat(pal.glass);
            const Rect& r = rw.r;
            ms.roof.hexa({ms.roof.P(r.x0, r.y0, onRoof(r.x0)), ms.roof.P(r.x1, r.y0, onRoof(r.x1)),
                          ms.roof.P(r.x1, r.y1, onRoof(r.x1)), ms.roof.P(r.x0, r.y1, onRoof(r.x0))},
                         {ms.roof.P(r.x0, r.y0, onRoof(r.x0) + 0.06f), ms.roof.P(r.x1, r.y0, onRoof(r.x1) + 0.06f),
                          ms.roof.P(r.x1, r.y1, onRoof(r.x1) + 0.06f), ms.roof.P(r.x0, r.y1, onRoof(r.x0) + 0.06f)},
                         fr);
            const float ix0 = r.x0 + 0.06f, ix1 = r.x1 - 0.06f, iy0 = r.y0 + 0.06f, iy1 = r.y1 - 0.06f;
            ms.windows.hexa({ms.roof.P(ix0, iy0, onRoof(ix0) + 0.06f), ms.roof.P(ix1, iy0, onRoof(ix1) + 0.06f),
                             ms.roof.P(ix1, iy1, onRoof(ix1) + 0.06f), ms.roof.P(ix0, iy1, onRoof(ix0) + 0.06f)},
                            {ms.roof.P(ix0, iy0, onRoof(ix0) + 0.075f), ms.roof.P(ix1, iy0, onRoof(ix1) + 0.075f),
                             ms.roof.P(ix1, iy1, onRoof(ix1) + 0.075f), ms.roof.P(ix0, iy1, onRoof(ix0) + 0.075f)},
                            gl);
        }
    }
    return ms;
}

} // namespace

std::vector<Entity> generate(const Params& pIn, const Palette& pal, int& counter,
                             const glm::vec3& groundPos) {
    const Plan plan = layout(pIn);
    Meshes ms = buildMeshes(plan, pal);

    std::vector<Entity> out;
    Entity root;
    root.type        = EntityType::Empty;
    root.name        = "House";
    root.id          = counter++;
    root.parent      = -1;
    root.half        = glm::vec3(0.5f);
    root.localCenter = root.center = groundPos;
    {
        auto hc = std::make_unique<HouseComponent>();
        hc->params = plan.params;
        root.components.items.push_back(std::move(hc));
    }
    out.push_back(std::move(root));
    const int rootId = out.front().id;

    // One child per part. The mesh is re-centred on its own bounds and the
    // entity placed at that centre with those half-extents -- the invariant the
    // Modeling panel keeps too, so fitScale() is exactly 1 and the pick box, the
    // gizmo and the collider all describe the shape that is there.
    auto add = [&](std::string name, Builder&& b, bool collide) {
        EditMesh& m = b.m;
        if (m.faces.empty()) return;
        // The object's own material is the one most faces wear; only the rest
        // are stored per face (a scene file then carries GUIDs only where they
        // say something).
        std::map<std::string, std::pair<int, AssetId>> tally;
        for (const AssetId& id : m.faceMat) {
            auto& e = tally[id.toString()];
            ++e.first;
            e.second = id;
        }
        AssetId own;
        int best = -1;
        for (const auto& [k, v] : tally) if (v.first > best) { best = v.first; own = v.second; }
        for (AssetId& id : m.faceMat) if (id == own) id = AssetId{};
        if (!m.dressed()) m.faceMat.clear();

        const glm::vec3 shift = editmesh::recenter(m);
        glm::vec3 mn, mx;
        m.bounds(mn, mx);
        Entity e;
        e.type          = EntityType::Box;
        e.name          = std::move(name);
        e.id            = counter++;
        e.parent        = rootId;
        e.half          = glm::max(0.5f * (mx - mn), glm::vec3(0.005f));
        e.localCenter   = shift;
        e.center        = groundPos + shift;
        auto mc = std::make_unique<MeshComponent>();
        mc->mesh = std::move(m);
        mc->touch();
        e.components.items.push_back(std::move(mc));
        if (own.valid()) {
            auto mat = std::make_unique<MaterialComponent>();
            mat->material = own;
            e.components.items.push_back(std::move(mat));
        }
        if (collide && plan.params.collider) {
            auto pc = std::make_unique<PhysicsComponent>();
            pc->dynamic = false;
            e.components.items.push_back(std::move(pc));
        }
        out.push_back(std::move(e));
    };
    add("Slabs", std::move(ms.slabs), true);
    for (auto& [name, b] : ms.walls) add(name, std::move(b), true);
    add("Stair", std::move(ms.stair), true);
    add("Roof", std::move(ms.roof), true);
    add("Windows", std::move(ms.windows), true);
    add("Doors", std::move(ms.doors), false);   // open leaves: walk past, not into
    add("Outside", std::move(ms.outside), true);
    return out;
}

// --- Persistence ---------------------------------------------------------------

namespace {

nlohmann::json vec3Json(const glm::vec3& v) { return nlohmann::json::array({v.x, v.y, v.z}); }
glm::vec3 jsonVec3(const nlohmann::json& j, const glm::vec3& def) {
    if (!j.is_array() || j.size() != 3) return def;
    return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
}
nlohmann::json roomsJson(const std::vector<RoomSpec>& rs) {
    nlohmann::json a = nlohmann::json::array();
    for (const RoomSpec& r : rs)
        a.push_back({{"type", static_cast<int>(r.type)}, {"name", r.name}, {"area", r.area}});
    return a;
}
std::vector<RoomSpec> jsonRooms(const nlohmann::json& a) {
    std::vector<RoomSpec> out;
    if (!a.is_array()) return out;
    for (const nlohmann::json& j : a) {
        RoomSpec r;
        const int t = std::clamp(j.value("type", 0), 0, static_cast<int>(RoomType::Count) - 1);
        r.type = static_cast<RoomType>(t);
        r.name = j.value("name", std::string());
        r.area = j.value("area", defaultArea(r.type));
        out.push_back(r);
    }
    return out;
}

} // namespace

void saveParams(nlohmann::json& j, const Params& p) {
    j["width"] = p.width;               j["depth"] = p.depth;
    j["storeys"] = p.storeys;           j["attic"] = p.attic;
    j["storeyHeight"] = p.storeyHeight; j["clearHeight"] = p.clearHeight;
    j["plinth"] = p.plinth;             j["serviceDepth"] = p.serviceDepth;
    j["outerWall"] = p.outerWall;       j["bearingWall"] = p.bearingWall;
    j["partition"] = p.partition;       j["terrace"] = p.terrace;
    j["roofPitch"] = p.roofPitch;       j["kneeWall"] = p.kneeWall;
    j["eaveOverhang"] = p.eaveOverhang; j["gableOverhang"] = p.gableOverhang;
    j["roof"] = p.roof;                 j["cutLevel"] = p.cutLevel;
    j["collider"] = p.collider;
    j["facadeColor"] = vec3Json(p.facadeColor);
    j["plinthColor"] = vec3Json(p.plinthColor);
    j["roofColor"]   = vec3Json(p.roofColor);
    j["frameColor"]  = vec3Json(p.frameColor);
    j["floorColor"]  = vec3Json(p.floorColor);
    nlohmann::json st = nlohmann::json::array();
    for (const auto& rs : p.storeyRooms) st.push_back(roomsJson(rs));
    j["storeyRooms"] = st;
    j["atticRooms"]  = roomsJson(p.atticRooms);
}

void loadParams(const nlohmann::json& j, Params& p) {
    const Params d;   // missing fields keep the defaults
    p.width = j.value("width", d.width);                 p.depth = j.value("depth", d.depth);
    p.storeys = j.value("storeys", d.storeys);           p.attic = j.value("attic", d.attic);
    p.storeyHeight = j.value("storeyHeight", d.storeyHeight);
    p.clearHeight = j.value("clearHeight", d.clearHeight);
    p.plinth = j.value("plinth", d.plinth);
    p.serviceDepth = j.value("serviceDepth", d.serviceDepth);
    p.outerWall = j.value("outerWall", d.outerWall);
    p.bearingWall = j.value("bearingWall", d.bearingWall);
    p.partition = j.value("partition", d.partition);
    p.terrace = j.value("terrace", d.terrace);
    p.roofPitch = j.value("roofPitch", d.roofPitch);
    p.kneeWall = j.value("kneeWall", d.kneeWall);
    p.eaveOverhang = j.value("eaveOverhang", d.eaveOverhang);
    p.gableOverhang = j.value("gableOverhang", d.gableOverhang);
    p.roof = j.value("roof", d.roof);
    p.cutLevel = j.value("cutLevel", d.cutLevel);
    p.collider = j.value("collider", d.collider);
    p.facadeColor = jsonVec3(j.value("facadeColor", nlohmann::json()), d.facadeColor);
    p.plinthColor = jsonVec3(j.value("plinthColor", nlohmann::json()), d.plinthColor);
    p.roofColor   = jsonVec3(j.value("roofColor", nlohmann::json()), d.roofColor);
    p.frameColor  = jsonVec3(j.value("frameColor", nlohmann::json()), d.frameColor);
    p.floorColor  = jsonVec3(j.value("floorColor", nlohmann::json()), d.floorColor);
    if (j.contains("storeyRooms") && j["storeyRooms"].is_array()) {
        for (auto& rs : p.storeyRooms) rs.clear();
        const auto& st = j["storeyRooms"];
        for (std::size_t i = 0; i < st.size() && i < p.storeyRooms.size(); ++i)
            p.storeyRooms[i] = jsonRooms(st[i]);
    } else {
        p.storeyRooms = d.storeyRooms;
    }
    p.atticRooms = j.contains("atticRooms") ? jsonRooms(j["atticRooms"]) : d.atticRooms;
    p = sane(p);
}

int faceCount(const Params& p) {
    Meshes ms = buildMeshes(layout(p), Palette{});
    std::size_t n = ms.slabs.m.faces.size() + ms.stair.m.faces.size() +
                    ms.roof.m.faces.size() + ms.windows.m.faces.size() +
                    ms.doors.m.faces.size() + ms.outside.m.faces.size();
    for (const auto& w : ms.walls) n += w.second.m.faces.size();
    return static_cast<int>(n);
}

namespace {
// Scene files name components by type id; this is what lets a saved house come
// back as a house (and not lose its parameters to an unknown-component warning).
struct RegisterHouse {
    RegisterHouse() {
        components::registerType({"house", "House",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<HouseComponent>()); },
            /*addable=*/false});
    }
} g_registerHouse;
} // namespace

} // namespace housegen
