#include "VideoSeq.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace vseq {

using json = nlohmann::json;
namespace fs = std::filesystem;

const char* kindName(ClipKind k) {
    switch (k) {
    case ClipKind::Video: return "video";
    case ClipKind::Image: return "image";
    case ClipKind::Audio: return "audio";
    case ClipKind::Text:  return "text";
    case ClipKind::Color: return "color";
    }
    return "video";
}

const char* fitName(Fit f) {
    switch (f) {
    case Fit::Fit:     return "fit";
    case Fit::Fill:    return "fill";
    case Fit::Stretch: return "stretch";
    }
    return "fit";
}

namespace {

ClipKind clipKindFrom(const std::string& s) {
    if (s == "image") return ClipKind::Image;
    if (s == "audio") return ClipKind::Audio;
    if (s == "text") return ClipKind::Text;
    if (s == "color") return ClipKind::Color;
    return ClipKind::Video;
}

Fit fitFrom(const std::string& s) {
    if (s == "fill") return Fit::Fill;
    if (s == "stretch") return Fit::Stretch;
    return Fit::Fit;
}

std::string lowerExt(const std::string& path) {
    std::string e = fs::path(path).extension().string();
    for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

json rgbaJson(const Rgba& c) { return json::array({c.r, c.g, c.b, c.a}); }
Rgba rgbaFrom(const json& j, Rgba def) {
    if (!j.is_array() || j.size() < 3) return def;
    Rgba c;
    c.r = j[0].get<float>();
    c.g = j[1].get<float>();
    c.b = j[2].get<float>();
    c.a = j.size() > 3 ? j[3].get<float>() : 1.0f;
    return c;
}

json lookJson(const Look& l) {
    return {{"x", l.x}, {"y", l.y}, {"scale", l.scale}, {"rotation", l.rotation}, {"opacity", l.opacity}};
}
Look lookFrom(const json& j) {
    Look l;
    if (!j.is_object()) return l;
    l.x        = j.value("x", 0.0f);
    l.y        = j.value("y", 0.0f);
    l.scale    = j.value("scale", 1.0f);
    l.rotation = j.value("rotation", 0.0f);
    l.opacity  = j.value("opacity", 1.0f);
    return l;
}

double finiteOr(double v, double def) { return std::isfinite(v) ? v : def; }

} // namespace

Look mix(const Look& a, const Look& b, float f) {
    auto m = [f](float x, float y) { return x + (y - x) * f; };
    return {m(a.x, b.x), m(a.y, b.y), m(a.scale, b.scale), m(a.rotation, b.rotation), m(a.opacity, b.opacity)};
}

Look Clip::lookAt(double t) const {
    if (!animate || length <= 0.0) return look;
    const float f = static_cast<float>(std::clamp((t - start) / length, 0.0, 1.0));
    return mix(look, lookEnd, f);
}

// --- Film ------------------------------------------------------------------------------

Film Film::starter() {
    Film f;
    f.addTrack(TrackKind::Video, "V1");
    f.addTrack(TrackKind::Audio, "A1");
    return f;
}

double Film::duration() const {
    double d = 0.0;
    for (const Track& t : tracks)
        for (const Clip& c : t.clips) d = std::max(d, c.end());
    return d;
}

double Film::snap(double t) const {
    const double f = fps > 0.0 ? fps : 30.0;
    return std::round(t * f) / f;
}

std::int64_t Film::frameOf(double t) const {
    const double f = fps > 0.0 ? fps : 30.0;
    // A hair under the frame's own time still counts as that frame: seconds
    // that went through a division come back a rounding error short.
    return static_cast<std::int64_t>(std::floor(t * f + 1e-6));
}

Track* Film::track(int uid) {
    for (Track& t : tracks)
        if (t.uid == uid) return &t;
    return nullptr;
}
const Track* Film::track(int uid) const { return const_cast<Film*>(this)->track(uid); }

Clip* Film::clip(int uid) {
    for (Track& t : tracks)
        for (Clip& c : t.clips)
            if (c.uid == uid) return &c;
    return nullptr;
}
const Clip* Film::clip(int uid) const { return const_cast<Film*>(this)->clip(uid); }

Track* Film::trackOf(int clipUid) {
    for (Track& t : tracks)
        for (const Clip& c : t.clips)
            if (c.uid == clipUid) return &t;
    return nullptr;
}
const Track* Film::trackOf(int clipUid) const { return const_cast<Film*>(this)->trackOf(clipUid); }

int Film::rowOf(int trackUid) const {
    for (std::size_t i = 0; i < tracks.size(); ++i)
        if (tracks[i].uid == trackUid) return static_cast<int>(i);
    return -1;
}

int Film::addTrack(TrackKind kind, const std::string& name) {
    Track t;
    t.uid  = newUid();
    t.kind = kind;
    int count = 1;
    for (const Track& o : tracks) count += o.kind == kind ? 1 : 0;
    t.name = name.empty() ? std::string(kind == TrackKind::Video ? "V" : "A") + std::to_string(count) : name;
    if (kind == TrackKind::Video) {
        // On top of the pile: before the first video track.
        auto it = std::find_if(tracks.begin(), tracks.end(), [](const Track& o) { return o.kind == TrackKind::Video; });
        if (it == tracks.end())
            it = std::find_if(tracks.begin(), tracks.end(), [](const Track& o) { return o.kind == TrackKind::Audio; });
        tracks.insert(it, t);
    } else {
        tracks.push_back(t);
    }
    return t.uid;
}

int Film::addClip(int trackUid, Clip c) {
    Track* t = track(trackUid);
    if (!t) return -1;
    if (c.uid <= 0) c.uid = newUid();
    nextUid = std::max(nextUid, c.uid + 1);
    const int uid = c.uid;
    t->clips.push_back(std::move(c));
    sortClips();
    return uid;
}

bool Film::removeClip(int uid) {
    for (Track& t : tracks) {
        auto it = std::find_if(t.clips.begin(), t.clips.end(), [uid](const Clip& c) { return c.uid == uid; });
        if (it != t.clips.end()) {
            t.clips.erase(it);
            return true;
        }
    }
    return false;
}

void Film::sortClips() {
    for (Track& t : tracks)
        std::stable_sort(t.clips.begin(), t.clips.end(), [](const Clip& a, const Clip& b) { return a.start < b.start; });
}

std::vector<Film::Active> Film::activeAt(std::size_t row, double t) const {
    std::vector<Active> out;
    if (row >= tracks.size()) return out;
    const double eps = 1e-6;
    for (const Clip& c : tracks[row].clips) {
        if (c.start > t + eps) break;
        if (t + eps < c.start || t >= c.end() - eps) continue;
        float dissolve = 1.0f;
        if (!out.empty()) {
            // Coming up over the clip under it, across the overlap the two share.
            const Clip& p = *out.back().clip;
            const double ov = p.end() - c.start;
            if (ov > eps) dissolve = static_cast<float>(std::clamp((t - c.start) / ov, 0.0, 1.0));
        }
        out.push_back({&c, dissolve});
    }
    return out;
}

int Film::split(int uid, double t) {
    Track* tr = trackOf(uid);
    Clip* c = clip(uid);
    if (!tr || !c) return -1;
    const double half = frame() * 0.5;
    if (t <= c->start + half || t >= c->end() - half) return -1;
    Clip right = *c;
    right.uid    = newUid();
    right.start  = t;
    right.length = c->end() - t;
    right.in     = c->in + (t - c->start) * c->speed;
    right.fadeIn = 0.0f;
    if (c->animate) {
        const Look mid = c->lookAt(t);
        right.look   = mid;
        c->lookEnd   = mid;
    }
    c->length  = t - c->start;
    c->fadeOut = 0.0f;
    c->fadeIn  = std::min(c->fadeIn, static_cast<float>(c->length));
    right.fadeOut = std::min(right.fadeOut, static_cast<float>(right.length));
    const int ru = right.uid;
    tr->clips.push_back(std::move(right));
    sortClips();
    return ru;
}

void Film::rippleDelete(const std::vector<int>& uids) {
    // Latest first, so each gap is measured against what is left of the track.
    std::vector<std::pair<double, int>> order;
    for (int u : uids)
        if (const Clip* c = clip(u)) order.push_back({c->start, u});
    std::sort(order.rbegin(), order.rend());
    const double eps = 1e-6;
    for (const auto& [startAt, uid] : order) {
        Track* tr = trackOf(uid);
        if (!tr) continue;
        const Clip gone = *clip(uid);
        removeClip(uid);
        double gapStart = gone.start;
        double laterStart = -1.0;
        for (const Clip& o : tr->clips) {
            if (o.start < gone.start - eps) gapStart = std::max(gapStart, o.end());
            if (o.start >= gone.end() - eps && (laterStart < 0.0 || o.start < laterStart)) laterStart = o.start;
        }
        if (laterStart < 0.0) continue;
        const double shift = std::clamp(laterStart - gapStart, 0.0, gone.end() - gapStart);
        if (shift <= eps) continue;
        for (Clip& o : tr->clips)
            if (o.start >= gone.end() - eps) o.start = snap(o.start - shift);
    }
    sortClips();
}

std::vector<double> Film::editPoints() const {
    std::vector<double> p{0.0};
    for (const Track& t : tracks)
        for (const Clip& c : t.clips) {
            p.push_back(c.start);
            p.push_back(c.end());
        }
    std::sort(p.begin(), p.end());
    p.erase(std::unique(p.begin(), p.end(), [](double a, double b) { return std::abs(a - b) < 1e-6; }), p.end());
    return p;
}

// --- Files -----------------------------------------------------------------------------

std::string Film::toJson() const {
    json j;
    j["format"]     = "fitzel-film";
    j["version"]    = 1;
    j["name"]       = name;
    j["width"]      = width;
    j["height"]     = height;
    j["fps"]        = fps;
    j["background"] = rgbaJson(background);
    j["markIn"]     = markIn;
    j["markOut"]    = markOut;
    j["nextUid"]    = nextUid;
    j["media"]      = media;
    json ts = json::array();
    for (const Track& t : tracks) {
        json jt;
        jt["uid"]    = t.uid;
        jt["kind"]   = t.kind == TrackKind::Video ? "video" : "audio";
        jt["name"]   = t.name;
        jt["muted"]  = t.muted;
        jt["hidden"] = t.hidden;
        jt["locked"] = t.locked;
        jt["volume"] = t.volume;
        json cs = json::array();
        for (const Clip& c : t.clips) {
            json jc;
            jc["uid"]    = c.uid;
            jc["kind"]   = kindName(c.kind);
            jc["name"]   = c.name;
            if (!c.file.empty()) jc["file"] = c.file;
            jc["start"]  = c.start;
            jc["length"] = c.length;
            jc["in"]     = c.in;
            jc["speed"]  = c.speed;
            jc["fadeIn"] = c.fadeIn;
            jc["fadeOut"] = c.fadeOut;
            if (c.kind == ClipKind::Video || c.kind == ClipKind::Audio) {
                jc["volume"] = c.volume;
                jc["muted"]  = c.muted;
            }
            if (c.visual()) {
                jc["look"] = lookJson(c.look);
                jc["fit"]  = fitName(c.fit);
                if (c.animate) jc["lookEnd"] = lookJson(c.lookEnd);
            }
            if (c.kind == ClipKind::Text) {
                const TextStyle& s = c.text;
                jc["text"] = {{"text", s.text}, {"font", s.font}, {"size", s.size}, {"color", rgbaJson(s.color)},
                              {"align", s.align}, {"bold", s.bold}, {"outline", s.outline},
                              {"outlineColor", rgbaJson(s.outlineColor)}, {"shadow", s.shadow}, {"box", s.box},
                              {"boxColor", rgbaJson(s.boxColor)}};
            }
            if (c.kind == ClipKind::Color) jc["color"] = rgbaJson(c.color);
            cs.push_back(std::move(jc));
        }
        jt["clips"] = std::move(cs);
        ts.push_back(std::move(jt));
    }
    j["tracks"] = std::move(ts);
    return j.dump(1, '\t');
}

bool Film::fromJson(const std::string& text, Film& out, std::string* error) {
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        if (error) *error = "not JSON";
        return false;
    }
    if (j.value("format", std::string()) != "fitzel-film") {
        if (error) *error = "not a film of the Video Sequencer";
        return false;
    }
    try {
        Film f;
        f.name       = j.value("name", std::string("Untitled"));
        f.width      = std::clamp(j.value("width", 1920), 16, 8192) & ~1;
        f.height     = std::clamp(j.value("height", 1080), 16, 8192) & ~1;
        f.fps        = std::clamp(finiteOr(j.value("fps", 30.0), 30.0), 1.0, 240.0);
        f.background = rgbaFrom(j.value("background", json()), Rgba{0, 0, 0, 1});
        f.markIn     = finiteOr(j.value("markIn", -1.0), -1.0);
        f.markOut    = finiteOr(j.value("markOut", -1.0), -1.0);
        f.nextUid    = std::max(1, j.value("nextUid", 1));
        if (j.contains("media") && j["media"].is_array())
            for (const auto& m : j["media"])
                if (m.is_string()) f.media.push_back(m.get<std::string>());
        int maxUid = 0;
        if (j.contains("tracks") && j["tracks"].is_array())
            for (const auto& jt : j["tracks"]) {
                Track t;
                t.uid    = jt.value("uid", 0);
                t.kind   = jt.value("kind", std::string("video")) == "audio" ? TrackKind::Audio : TrackKind::Video;
                t.name   = jt.value("name", std::string());
                t.muted  = jt.value("muted", false);
                t.hidden = jt.value("hidden", false);
                t.locked = jt.value("locked", false);
                t.volume = std::clamp(jt.value("volume", 1.0f), 0.0f, 4.0f);
                maxUid   = std::max(maxUid, t.uid);
                if (jt.contains("clips") && jt["clips"].is_array())
                    for (const auto& jc : jt["clips"]) {
                        Clip c;
                        c.uid     = jc.value("uid", 0);
                        c.kind    = clipKindFrom(jc.value("kind", std::string("video")));
                        c.name    = jc.value("name", std::string());
                        c.file    = jc.value("file", std::string());
                        c.start   = std::max(0.0, finiteOr(jc.value("start", 0.0), 0.0));
                        c.length  = std::max(1e-3, finiteOr(jc.value("length", 1.0), 1.0));
                        c.in      = std::max(0.0, finiteOr(jc.value("in", 0.0), 0.0));
                        c.speed   = std::clamp(finiteOr(jc.value("speed", 1.0), 1.0), 0.05, 20.0);
                        c.fadeIn  = std::max(0.0f, jc.value("fadeIn", 0.0f));
                        c.fadeOut = std::max(0.0f, jc.value("fadeOut", 0.0f));
                        c.volume  = std::clamp(jc.value("volume", 1.0f), 0.0f, 4.0f);
                        c.muted   = jc.value("muted", false);
                        c.look    = lookFrom(jc.value("look", json()));
                        c.fit     = fitFrom(jc.value("fit", std::string("fit")));
                        if (jc.contains("lookEnd")) {
                            c.animate = true;
                            c.lookEnd = lookFrom(jc["lookEnd"]);
                        } else {
                            c.lookEnd = c.look;
                        }
                        if (jc.contains("text") && jc["text"].is_object()) {
                            const auto& js = jc["text"];
                            TextStyle& s   = c.text;
                            s.text         = js.value("text", std::string());
                            s.font         = js.value("font", std::string());
                            s.size         = std::clamp(js.value("size", 0.08f), 0.005f, 1.0f);
                            s.color        = rgbaFrom(js.value("color", json()), Rgba{});
                            s.align        = std::clamp(js.value("align", 1), 0, 2);
                            s.bold         = js.value("bold", false);
                            s.outline      = std::clamp(js.value("outline", 0.0f), 0.0f, 0.5f);
                            s.outlineColor = rgbaFrom(js.value("outlineColor", json()), Rgba{0, 0, 0, 1});
                            s.shadow       = js.value("shadow", true);
                            s.box          = std::clamp(js.value("box", 0.0f), 0.0f, 1.0f);
                            s.boxColor     = rgbaFrom(js.value("boxColor", json()), Rgba{0, 0, 0, 1});
                        }
                        c.color = rgbaFrom(jc.value("color", json()), Rgba{0, 0, 0, 1});
                        maxUid  = std::max(maxUid, c.uid);
                        t.clips.push_back(std::move(c));
                    }
                f.tracks.push_back(std::move(t));
            }
        // Uids that went missing or repeat (a file put together by hand) get
        // fresh ones: everything the panel does is by uid.
        f.nextUid = std::max(f.nextUid, maxUid + 1);
        std::vector<int> seen;
        auto fresh = [&](int& uid) {
            if (uid <= 0 || std::find(seen.begin(), seen.end(), uid) != seen.end()) uid = f.nextUid++;
            seen.push_back(uid);
        };
        for (Track& t : f.tracks) {
            fresh(t.uid);
            for (Clip& c : t.clips) fresh(c.uid);
        }
        f.sortClips();
        out = std::move(f);
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

bool Film::save(const std::string& path, std::string* error) const {
    std::error_code ec;
    if (fs::path(path).has_parent_path()) fs::create_directories(fs::path(path).parent_path(), ec);
    // Through a temporary file: a save that dies half-way leaves the old film.
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            if (error) *error = "cannot write " + path;
            return false;
        }
        f << toJson();
        if (!f) {
            if (error) *error = "writing " + path + " failed";
            return false;
        }
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(path, ec);
        fs::rename(tmp, path, ec);
    }
    if (ec && error) *error = ec.message();
    return !ec;
}

bool Film::load(const std::string& path, Film& out, std::string* error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (error) *error = "cannot read " + path;
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return fromJson(ss.str(), out, error);
}

// --- Sources -------------------------------------------------------------------------

bool isVideoFile(const std::string& path) {
    const std::string e = lowerExt(path);
    return e == ".mp4" || e == ".mov" || e == ".mkv" || e == ".webm" || e == ".avi" || e == ".m4v" || e == ".mpg" ||
           e == ".mpeg" || e == ".wmv" || e == ".gif" || e == ".flv" || e == ".ts" || e == ".mts";
}

bool isImageFile(const std::string& path) {
    const std::string e = lowerExt(path);
    return e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".bmp" || e == ".tga" || e == ".psd";
}

bool isSongFile(const std::string& path) {
    if (lowerExt(path) != ".json") return false;
    const fs::path p(path);
    return p.has_parent_path() && p.parent_path().filename() == "songs";
}

bool isAudioFile(const std::string& path) {
    const std::string e = lowerExt(path);
    return e == ".wav" || e == ".ogg" || e == ".mp3" || e == ".flac" || e == ".m4a" || e == ".aac" ||
           e == ".opus" || e == ".aiff" || e == ".aif" || isSongFile(path);
}

} // namespace vseq
