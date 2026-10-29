#include "fitzel/audio/Vst3.hpp"

// The VST3 host (see fitzel/audio/Vst3.hpp). Written against Steinberg's
// interface headers alone (pluginterfaces, MIT): no SDK hosting helpers, no
// framework. What a host has to provide is small -- a host application that can
// make messages, a component handler for edits coming out of the plugin's own
// editor, a frame for that editor and, on Linux, a run loop for the editor's
// timers and sockets -- and every piece of it is here.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if FITZEL_VST3

#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <type_traits>

#include <dlfcn.h>
#include <poll.h>

#include <nlohmann/json.hpp>

// The one file that includes the interface headers defines their ids.
#define INIT_CLASS_IID
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/base/smartpointer.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstattributes.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/vstspeaker.h"

// Last: Xlib defines None, Bool, Status and friends as macros.
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#endif

namespace fitzel::vst3 {

#if !FITZEL_VST3

// --- No host on this platform ---------------------------------------------------
struct Instance::Impl {};

bool supported() { return false; }
std::vector<std::string> searchPaths() { return {}; }
const std::vector<PluginInfo>& plugins(bool) { static const std::vector<PluginInfo> none; return none; }
const PluginInfo* find(const std::string&) { return nullptr; }
std::shared_ptr<Instance> Instance::create(const PluginInfo&, double, std::string* error) {
    if (error) *error = "VST3 plugins are not supported on this platform";
    return nullptr;
}
Instance::Instance(std::unique_ptr<Impl> impl) : m(std::move(impl)) {}
Instance::~Instance() = default;
const PluginInfo& Instance::info() const { static const PluginInfo none; return none; }
const std::vector<ParamInfo>& Instance::params() const { static const std::vector<ParamInfo> none; return none; }
double Instance::param(uint32_t) const { return 0.0; }
void Instance::setParam(uint32_t, double) {}
std::string Instance::display(uint32_t, double) const { return {}; }
std::string Instance::state() const { return {}; }
bool Instance::setState(const std::string&) { return false; }
bool Instance::hasEditor() const { return false; }
bool Instance::editorOpen() const { return false; }
bool Instance::openEditor(const std::string&) { return false; }
void Instance::closeEditor() {}
void Instance::process(float*, unsigned, unsigned) {}
void idle() {}

#else

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

constexpr int32 kMaxBlock = 1024;   // what setupProcessing promises; longer blocks are cut up

// --- Small things -----------------------------------------------------------------
std::string utf8(const char16* s, std::size_t max) {
    std::string out;
    for (std::size_t i = 0; i < max && s[i]; ++i) {
        uint32_t c = static_cast<uint16_t>(s[i]);
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < max && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (static_cast<uint16_t>(s[i + 1]) - 0xDC00);
            ++i;
        }
        if (c < 0x80) out += static_cast<char>(c);
        else if (c < 0x800) { out += static_cast<char>(0xC0 | (c >> 6)); out += static_cast<char>(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

std::string uidString(const TUID id) {
    static const char* hex = "0123456789ABCDEF";
    std::string s;
    for (int i = 0; i < 16; ++i) {
        const auto b = static_cast<unsigned char>(id[i]);
        s += hex[b >> 4];
        s += hex[b & 15];
    }
    return s;
}

bool parseUid(const std::string& s, TUID out) {
    if (s.size() != 32) return false;
    for (int i = 0; i < 16; ++i) {
        unsigned v = 0;
        for (int k = 0; k < 2; ++k) {
            const char c = s[i * 2 + k];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
            else return false;
        }
        out[i] = static_cast<char8>(v);
    }
    return true;
}

bool sameIid(const TUID a, const FUID& b) { return FUnknownPrivate::iidEqual(a, b.toTUID()); }

int channelsOf(SpeakerArrangement a) {
    int n = 0;
    for (; a; a &= a - 1) ++n;
    return n;
}

// A COM object of the host's that lives as long as the host does (statics, or
// members of something that outlives every plugin holding it): counting is
// pretended, nothing is ever freed through release().
#define FZ_STATIC_REFCOUNT                                   \
    uint32 PLUGIN_API addRef() override { return 1000; }    \
    uint32 PLUGIN_API release() override { return 1000; }

// One that the plugin may hold on to: freed when the last reference goes.
#define FZ_REFCOUNT                                                        \
    std::atomic<uint32> refs{1};                                           \
    uint32 PLUGIN_API addRef() override { return ++refs; }                 \
    uint32 PLUGIN_API release() override {                                 \
        const uint32 r = --refs;                                           \
        if (r == 0) delete this;                                           \
        return r;                                                          \
    }

// --- Messages: the plugin's component and controller talk through them ---------
class AttributeList final : public IAttributeList {
public:
    FZ_REFCOUNT
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (sameIid(iid, IAttributeList::iid) || sameIid(iid, FUnknown::iid)) {
            addRef(); *obj = static_cast<IAttributeList*>(this); return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    tresult PLUGIN_API setInt(AttrID id, int64 v) override { ints[id] = v; return kResultOk; }
    tresult PLUGIN_API getInt(AttrID id, int64& v) override {
        const auto it = ints.find(id);
        if (it == ints.end()) return kResultFalse;
        v = it->second;
        return kResultOk;
    }
    tresult PLUGIN_API setFloat(AttrID id, double v) override { floats[id] = v; return kResultOk; }
    tresult PLUGIN_API getFloat(AttrID id, double& v) override {
        const auto it = floats.find(id);
        if (it == floats.end()) return kResultFalse;
        v = it->second;
        return kResultOk;
    }
    tresult PLUGIN_API setString(AttrID id, const TChar* s) override {
        std::u16string v;
        for (; s && *s; ++s) v += static_cast<char16_t>(*s);
        strings[id] = v;
        return kResultOk;
    }
    tresult PLUGIN_API getString(AttrID id, TChar* s, uint32 bytes) override {
        const auto it = strings.find(id);
        if (it == strings.end() || bytes < sizeof(TChar)) return kResultFalse;
        const std::size_t n = std::min<std::size_t>(it->second.size(), bytes / sizeof(TChar) - 1);
        for (std::size_t i = 0; i < n; ++i) s[i] = static_cast<TChar>(it->second[i]);
        s[n] = 0;
        return kResultOk;
    }
    tresult PLUGIN_API setBinary(AttrID id, const void* data, uint32 bytes) override {
        binaries[id].assign(static_cast<const char*>(data), bytes);
        return kResultOk;
    }
    tresult PLUGIN_API getBinary(AttrID id, const void*& data, uint32& bytes) override {
        const auto it = binaries.find(id);
        if (it == binaries.end()) return kResultFalse;
        data  = it->second.data();
        bytes = static_cast<uint32>(it->second.size());
        return kResultOk;
    }

private:
    std::map<std::string, int64>          ints;
    std::map<std::string, double>         floats;
    std::map<std::string, std::u16string> strings;
    std::map<std::string, std::string>    binaries;
};

class Message final : public IMessage {
public:
    FZ_REFCOUNT
    Message() : attrs(new AttributeList) {}
    ~Message() { attrs->release(); }
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (sameIid(iid, IMessage::iid) || sameIid(iid, FUnknown::iid)) {
            addRef(); *obj = static_cast<IMessage*>(this); return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    FIDString PLUGIN_API getMessageID() override { return id.c_str(); }
    void PLUGIN_API setMessageID(FIDString s) override { id = s ? s : ""; }
    IAttributeList* PLUGIN_API getAttributes() override { return attrs; }

private:
    std::string    id;
    AttributeList* attrs;
};

class HostApp final : public IHostApplication {
public:
    FZ_STATIC_REFCOUNT
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (sameIid(iid, IHostApplication::iid) || sameIid(iid, FUnknown::iid)) {
            *obj = static_cast<IHostApplication*>(this); return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    tresult PLUGIN_API getName(String128 name) override {
        const char* n = "Fitzel";
        int i = 0;
        for (; n[i] && i < 127; ++i) name[i] = static_cast<char16>(n[i]);
        name[i] = 0;
        return kResultOk;
    }
    tresult PLUGIN_API createInstance(TUID cid, TUID iid, void** obj) override {
        if (sameIid(cid, IMessage::iid) && sameIid(iid, IMessage::iid)) {
            *obj = static_cast<IMessage*>(new Message);
            return kResultOk;
        }
        if (sameIid(cid, IAttributeList::iid) && sameIid(iid, IAttributeList::iid)) {
            *obj = static_cast<IAttributeList*>(new AttributeList);
            return kResultOk;
        }
        *obj = nullptr;
        return kResultFalse;
    }
};
HostApp g_host;

// --- State as a stream of bytes -------------------------------------------------
class MemoryStream final : public IBStream {
public:
    FZ_STATIC_REFCOUNT   // lives on the stack of whoever asks for a state
    explicit MemoryStream(std::string initial = {}) : data(std::move(initial)) {}
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (sameIid(iid, IBStream::iid) || sameIid(iid, FUnknown::iid)) {
            *obj = static_cast<IBStream*>(this); return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    tresult PLUGIN_API read(void* buf, int32 n, int32* got) override {
        const int64 left = static_cast<int64>(data.size()) - pos;
        const int32 k = static_cast<int32>(std::max<int64>(0, std::min<int64>(n, left)));
        if (k > 0) std::memcpy(buf, data.data() + pos, static_cast<std::size_t>(k));
        pos += k;
        if (got) *got = k;
        return kResultOk;
    }
    tresult PLUGIN_API write(void* buf, int32 n, int32* put) override {
        if (n < 0) return kInvalidArgument;
        if (pos + n > static_cast<int64>(data.size())) data.resize(static_cast<std::size_t>(pos + n));
        std::memcpy(data.data() + pos, buf, static_cast<std::size_t>(n));
        pos += n;
        if (put) *put = n;
        return kResultOk;
    }
    tresult PLUGIN_API seek(int64 p, int32 mode, int64* result) override {
        int64 to = p;
        if (mode == kIBSeekCur) to = pos + p;
        else if (mode == kIBSeekEnd) to = static_cast<int64>(data.size()) + p;
        if (to < 0) return kResultFalse;
        pos = to;
        if (result) *result = pos;
        return kResultOk;
    }
    tresult PLUGIN_API tell(int64* p) override {
        if (p) *p = pos;
        return kResultOk;
    }
    void rewind() { pos = 0; }

    std::string data;
    int64       pos = 0;
};

// --- Parameter changes and events: fixed size, nothing allocated per block -------
class ParamQueue final : public IParamValueQueue {
public:
    FZ_STATIC_REFCOUNT
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (sameIid(iid, IParamValueQueue::iid) || sameIid(iid, FUnknown::iid)) {
            *obj = static_cast<IParamValueQueue*>(this); return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    ParamID PLUGIN_API getParameterId() override { return id; }
    int32 PLUGIN_API getPointCount() override { return count; }
    tresult PLUGIN_API getPoint(int32 i, int32& offset, ParamValue& v) override {
        if (i < 0 || i >= count) return kResultFalse;
        offset = points[i].offset;
        v      = points[i].value;
        return kResultOk;
    }
    tresult PLUGIN_API addPoint(int32 offset, ParamValue v, int32& index) override {
        // One per offset, in order; the newest value for an offset wins.
        for (int32 i = 0; i < count; ++i)
            if (points[i].offset == offset) { points[i].value = v; index = i; return kResultOk; }
        if (count >= kPoints) return kResultFalse;
        int32 at = count;
        while (at > 0 && points[at - 1].offset > offset) { points[at] = points[at - 1]; --at; }
        points[at] = {offset, v};
        ++count;
        index = at;
        return kResultOk;
    }

    static constexpr int32 kPoints = 16;
    struct Point { int32 offset; ParamValue value; };
    ParamID id = 0;
    int32   count = 0;
    Point   points[kPoints] = {};
};

class ParamChanges final : public IParameterChanges {
public:
    FZ_STATIC_REFCOUNT
    explicit ParamChanges(int32 capacity) : queues(static_cast<std::size_t>(capacity)) {}
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (sameIid(iid, IParameterChanges::iid) || sameIid(iid, FUnknown::iid)) {
            *obj = static_cast<IParameterChanges*>(this); return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    int32 PLUGIN_API getParameterCount() override { return used; }
    IParamValueQueue* PLUGIN_API getParameterData(int32 i) override {
        return i >= 0 && i < used ? &queues[static_cast<std::size_t>(i)] : nullptr;
    }
    IParamValueQueue* PLUGIN_API addParameterData(const ParamID& id, int32& index) override {
        for (int32 i = 0; i < used; ++i)
            if (queues[static_cast<std::size_t>(i)].id == id) { index = i; return &queues[static_cast<std::size_t>(i)]; }
        if (used >= static_cast<int32>(queues.size())) return nullptr;
        ParamQueue& q = queues[static_cast<std::size_t>(used)];
        q.id    = id;
        q.count = 0;
        index   = used++;
        return &q;
    }
    void clear() { used = 0; }

private:
    std::vector<ParamQueue> queues;
    int32                   used = 0;
};

// Inserts get no notes; what an arpeggiator or the like sends out is dropped.
class EventList final : public IEventList {
public:
    FZ_STATIC_REFCOUNT
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (sameIid(iid, IEventList::iid) || sameIid(iid, FUnknown::iid)) {
            *obj = static_cast<IEventList*>(this); return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    int32 PLUGIN_API getEventCount() override { return 0; }
    tresult PLUGIN_API getEvent(int32, Event&) override { return kResultFalse; }
    tresult PLUGIN_API addEvent(Event&) override { return kResultOk; }
};

// Main thread -> audio thread, one writer, one reader: the parameter edits the
// processor has not seen yet. Full means the audio is not running; the edit is
// dropped then (the controller has it, and a JUCE-style plugin shares it).
struct EditQueue {
    static constexpr uint32_t kSize = 2048;
    struct Edit { ParamID id; ParamValue value; };
    std::array<Edit, kSize> items{};
    std::atomic<uint32_t>   head{0}, tail{0};

    bool push(const Edit& e) {
        const uint32_t h = head.load(std::memory_order_relaxed);
        if (h - tail.load(std::memory_order_acquire) >= kSize) return false;
        items[h % kSize] = e;
        head.store(h + 1, std::memory_order_release);
        return true;
    }
    bool pop(Edit& e) {
        const uint32_t t = tail.load(std::memory_order_relaxed);
        if (t == head.load(std::memory_order_acquire)) return false;
        e = items[t % kSize];
        tail.store(t + 1, std::memory_order_release);
        return true;
    }
};

// --- The run loop: what keeps a Linux plugin's editor alive -----------------------
// A plugin hands over file descriptors (its X connection, its message queue) and
// timers; idle() polls the one and fires the other. Callbacks may register and
// unregister while they run, so every pass works on a copy and asks again
// before each call.
class RunLoop final : public Linux::IRunLoop {
public:
    FZ_STATIC_REFCOUNT
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (sameIid(iid, Linux::IRunLoop::iid) || sameIid(iid, FUnknown::iid)) {
            *obj = static_cast<Linux::IRunLoop*>(this); return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    tresult PLUGIN_API registerEventHandler(Linux::IEventHandler* h, Linux::FileDescriptor fd) override {
        if (!h) return kInvalidArgument;
        h->addRef();
        fds.push_back({h, fd});
        return kResultTrue;
    }
    tresult PLUGIN_API unregisterEventHandler(Linux::IEventHandler* h) override {
        for (auto it = fds.begin(); it != fds.end();)
            if (it->handler == h) { it->handler->release(); it = fds.erase(it); }
            else ++it;
        return kResultTrue;
    }
    tresult PLUGIN_API registerTimer(Linux::ITimerHandler* h, Linux::TimerInterval ms) override {
        if (!h) return kInvalidArgument;
        h->addRef();
        const auto every = std::chrono::milliseconds(std::max<Linux::TimerInterval>(ms, 1));
        timers.push_back({h, every, std::chrono::steady_clock::now() + every});
        return kResultTrue;
    }
    tresult PLUGIN_API unregisterTimer(Linux::ITimerHandler* h) override {
        for (auto it = timers.begin(); it != timers.end();)
            if (it->handler == h) { it->handler->release(); it = timers.erase(it); }
            else ++it;
        return kResultTrue;
    }

    void service() {
        using clock = std::chrono::steady_clock;
        const auto until = clock::now() + std::chrono::milliseconds(4);
        // A message queue may hand out one message per wake-up: keep going while
        // there is something to read, within a few milliseconds.
        for (int pass = 0; pass < 64 && !fds.empty(); ++pass) {
            std::vector<pollfd> p;
            for (const Fd& f : fds) p.push_back({f.fd, POLLIN, 0});
            if (::poll(p.data(), static_cast<nfds_t>(p.size()), 0) <= 0) break;
            std::vector<Fd> ready;
            for (std::size_t i = 0; i < p.size(); ++i)
                if (p[i].revents & (POLLIN | POLLERR | POLLHUP)) {
                    fds[i].handler->addRef();
                    ready.push_back(fds[i]);
                }
            for (const Fd& r : ready) {
                if (registered(r)) r.handler->onFDIsSet(r.fd);
                r.handler->release();
            }
            if (clock::now() > until) break;
        }
        const auto now = clock::now();
        std::vector<Timer> due;
        for (Timer& t : timers)
            if (now >= t.next) {
                t.next = now + t.every;
                t.handler->addRef();
                due.push_back(t);
            }
        for (const Timer& t : due) {
            if (std::any_of(timers.begin(), timers.end(), [&](const Timer& x) { return x.handler == t.handler; }))
                t.handler->onTimer();
            t.handler->release();
        }
    }

private:
    struct Fd { Linux::IEventHandler* handler; Linux::FileDescriptor fd; };
    struct Timer {
        Linux::ITimerHandler*                     handler;
        std::chrono::milliseconds                 every;
        std::chrono::steady_clock::time_point     next;
    };
    bool registered(const Fd& f) const {
        return std::any_of(fds.begin(), fds.end(),
                           [&](const Fd& x) { return x.handler == f.handler && x.fd == f.fd; });
    }
    std::vector<Fd>    fds;
    std::vector<Timer> timers;
};
RunLoop g_runLoop;

// --- Xlib, opened at run time ------------------------------------------------------
// The editor window is an X11 one (plugins embed into an X window id; under
// Wayland that is XWayland). Loaded with dlopen so a machine without libX11 still
// runs everything but the editors.
struct X11 {
    bool tried = false;
    bool ok    = false;
    Display* dpy = nullptr;
    Atom wmDelete = 0, wmProtocols = 0, netWmName = 0, utf8String = 0;

    decltype(&::XOpenDisplay)       xOpenDisplay = nullptr;
    decltype(&::XDefaultScreen)     xDefaultScreen = nullptr;
    decltype(&::XRootWindow)        xRootWindow = nullptr;
    decltype(&::XBlackPixel)        xBlackPixel = nullptr;
    decltype(&::XCreateSimpleWindow) xCreateSimpleWindow = nullptr;
    decltype(&::XDestroyWindow)     xDestroyWindow = nullptr;
    decltype(&::XMapRaised)         xMapRaised = nullptr;
    decltype(&::XResizeWindow)      xResizeWindow = nullptr;
    decltype(&::XSelectInput)       xSelectInput = nullptr;
    decltype(&::XStoreName)         xStoreName = nullptr;
    decltype(&::XChangeProperty)    xChangeProperty = nullptr;
    decltype(&::XInternAtom)        xInternAtom = nullptr;
    decltype(&::XSetWMProtocols)    xSetWMProtocols = nullptr;
    decltype(&::XAllocSizeHints)    xAllocSizeHints = nullptr;
    decltype(&::XSetWMNormalHints)  xSetWMNormalHints = nullptr;
    decltype(&::XFree)              xFree = nullptr;
    decltype(&::XPending)           xPending = nullptr;
    decltype(&::XNextEvent)         xNextEvent = nullptr;
    decltype(&::XFlush)             xFlush = nullptr;
    decltype(&::XSync)              xSync = nullptr;
    decltype(&::XSetErrorHandler)   xSetErrorHandler = nullptr;

    bool load() {
        if (tried) return ok;
        tried = true;
        void* lib = dlopen("libX11.so.6", RTLD_LAZY | RTLD_LOCAL);
        if (!lib) { std::fprintf(stderr, "[vst3] no libX11 -- plugin editors cannot open\n"); return false; }
        bool all = true;
        auto get = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, name));
            all = all && fn;
        };
        get(xOpenDisplay, "XOpenDisplay");           get(xDefaultScreen, "XDefaultScreen");
        get(xRootWindow, "XRootWindow");             get(xBlackPixel, "XBlackPixel");
        get(xCreateSimpleWindow, "XCreateSimpleWindow"); get(xDestroyWindow, "XDestroyWindow");
        get(xMapRaised, "XMapRaised");               get(xResizeWindow, "XResizeWindow");
        get(xSelectInput, "XSelectInput");           get(xStoreName, "XStoreName");
        get(xChangeProperty, "XChangeProperty");     get(xInternAtom, "XInternAtom");
        get(xSetWMProtocols, "XSetWMProtocols");     get(xAllocSizeHints, "XAllocSizeHints");
        get(xSetWMNormalHints, "XSetWMNormalHints"); get(xFree, "XFree");
        get(xPending, "XPending");                   get(xNextEvent, "XNextEvent");
        get(xFlush, "XFlush");                       get(xSync, "XSync");
        get(xSetErrorHandler, "XSetErrorHandler");
        if (!all) { std::fprintf(stderr, "[vst3] libX11 is missing functions\n"); return false; }
        dpy = xOpenDisplay(nullptr);
        if (!dpy) { std::fprintf(stderr, "[vst3] no X display (DISPLAY unset?) -- plugin editors cannot open\n"); return false; }
        // Xlib's default handler ends the process; a stale window id must not.
        xSetErrorHandler([](Display*, XErrorEvent* e) {
            std::fprintf(stderr, "[vst3] X error %d (request %d)\n", e->error_code, e->request_code);
            return 0;
        });
        wmDelete    = xInternAtom(dpy, "WM_DELETE_WINDOW", False);
        wmProtocols = xInternAtom(dpy, "WM_PROTOCOLS", False);
        netWmName   = xInternAtom(dpy, "_NET_WM_NAME", False);
        utf8String  = xInternAtom(dpy, "UTF8_STRING", False);
        ok = true;
        return true;
    }
};
X11 g_x;

// --- Modules: loaded once, never unloaded ------------------------------------------
// A plugin library's statics (JUCE's message thread, for one) do not take well
// to being unloaded and loaded again; a module stays until the process ends.
struct Module {
    void*           handle  = nullptr;
    IPluginFactory* factory = nullptr;
    std::string     error;
};
std::map<std::string, Module>& modules() { static std::map<std::string, Module> m; return m; }

const char* archDir() {
#if defined(__x86_64__)
    return "x86_64-linux";
#elif defined(__aarch64__)
    return "aarch64-linux";
#elif defined(__i386__)
    return "i386-linux";
#elif defined(__arm__)
    return "armv7l-linux";
#else
    return "unknown-linux";
#endif
}

IPluginFactory* loadModule(const std::string& bundle, std::string& error) {
    auto [it, fresh] = modules().try_emplace(bundle);
    Module& m = it->second;
    if (!fresh) { error = m.error; return m.factory; }

    namespace fs = std::filesystem;
    std::error_code ec;
    std::string lib = bundle;
    if (fs::is_directory(bundle, ec)) {
        lib = (fs::path(bundle) / "Contents" / archDir() / (fs::path(bundle).stem().string() + ".so")).string();
        if (!fs::exists(lib, ec)) { m.error = error = "no " + std::string(archDir()) + " binary in the bundle"; return nullptr; }
    }
    m.handle = dlopen(lib.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (!m.handle) {
        const char* why = dlerror();
        m.error = error = why ? why : "dlopen failed";
        return nullptr;
    }
    using EntryFn   = bool (*)(void*);
    using FactoryFn = IPluginFactory* (PLUGIN_API*)();
    if (auto entry = reinterpret_cast<EntryFn>(dlsym(m.handle, "ModuleEntry")); entry && !entry(m.handle)) {
        m.error = error = "ModuleEntry failed";
        return nullptr;
    }
    auto get = reinterpret_cast<FactoryFn>(dlsym(m.handle, "GetPluginFactory"));
    if (!get) { m.error = error = "no GetPluginFactory"; return nullptr; }
    m.factory = get();
    if (!m.factory) m.error = error = "GetPluginFactory returned nothing";
    return m.factory;
}

// --- Scanning ---------------------------------------------------------------------
bool isInstrument(const std::string& subCategories) {
    return subCategories.find("Instrument") != std::string::npos ||
           subCategories.find("Synth") != std::string::npos ||
           subCategories.find("Drum") != std::string::npos;
}

// moduleinfo.json is JSON5 in principle: comments and trailing commas. The
// comments nlohmann skips by itself; the commas go here (outside strings).
std::string dropTrailingCommas(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool str = false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (str) {
            out += c;
            if (c == '\\' && i + 1 < s.size()) out += s[++i];
            else if (c == '"') str = false;
            continue;
        }
        if (c == '"') { str = true; out += c; continue; }
        if (c == ',') {
            std::size_t j = i + 1;
            while (j < s.size() && std::isspace(static_cast<unsigned char>(s[j]))) ++j;
            if (j < s.size() && (s[j] == '}' || s[j] == ']')) continue;
        }
        out += c;
    }
    return out;
}

bool scanModuleInfo(const std::string& bundle, std::vector<PluginInfo>& out) {
    std::ifstream f(std::filesystem::path(bundle) / "Contents" / "Resources" / "moduleinfo.json");
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    try {
        const nlohmann::json j = nlohmann::json::parse(dropTrailingCommas(ss.str()), nullptr, true, true);
        std::string factoryVendor;
        if (auto fi = j.find("Factory Info"); fi != j.end() && fi->is_object())
            factoryVendor = fi->value("Vendor", std::string());
        const auto classes = j.find("Classes");
        if (classes == j.end() || !classes->is_array()) return false;
        for (const auto& c : *classes) {
            if (c.value("Category", std::string()) != kVstAudioEffectClass) continue;
            PluginInfo p;
            p.uid     = c.value("CID", std::string());
            p.name    = c.value("Name", std::string());
            p.vendor  = c.value("Vendor", factoryVendor);
            p.version = c.value("Version", std::string());
            if (auto sc = c.find("Sub Categories"); sc != c.end() && sc->is_array())
                for (const auto& s : *sc)
                    if (s.is_string()) p.category += (p.category.empty() ? "" : "|") + s.get<std::string>();
            p.path       = bundle;
            p.instrument = isInstrument(p.category);
            std::transform(p.uid.begin(), p.uid.end(), p.uid.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
            TUID check;
            if (!p.name.empty() && parseUid(p.uid, check)) out.push_back(std::move(p));
        }
        return true;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[vst3] %s: unreadable moduleinfo.json (%s)\n", bundle.c_str(), e.what());
        return false;
    }
}

void scanByLoading(const std::string& bundle, std::vector<PluginInfo>& out) {
    std::string err;
    IPluginFactory* f = loadModule(bundle, err);
    if (!f) { std::fprintf(stderr, "[vst3] %s: %s\n", bundle.c_str(), err.c_str()); return; }
    PFactoryInfo fi{};
    f->getFactoryInfo(&fi);
    IPtr<IPluginFactory2> f2;
    {
        void* p = nullptr;
        if (f->queryInterface(IPluginFactory2::iid, &p) == kResultOk) f2 = owned(static_cast<IPluginFactory2*>(p));
    }
    for (int32 i = 0; i < f->countClasses(); ++i) {
        PluginInfo p;
        PClassInfo2 ci2{};
        PClassInfo  ci{};
        if (f2 && f2->getClassInfo2(i, &ci2) == kResultOk) {
            if (std::strcmp(ci2.category, kVstAudioEffectClass) != 0) continue;
            p.uid      = uidString(ci2.cid);
            p.name     = ci2.name;
            p.vendor   = ci2.vendor[0] ? ci2.vendor : fi.vendor;
            p.version  = ci2.version;
            p.category = ci2.subCategories;
        } else if (f->getClassInfo(i, &ci) == kResultOk) {
            if (std::strcmp(ci.category, kVstAudioEffectClass) != 0) continue;
            p.uid    = uidString(ci.cid);
            p.name   = ci.name;
            p.vendor = fi.vendor;
        } else {
            continue;
        }
        p.path       = bundle;
        p.instrument = isInstrument(p.category);
        out.push_back(std::move(p));
    }
}

std::vector<PluginInfo>& known() { static std::vector<PluginInfo> v; return v; }
bool g_scanned = false;

// --- The component handler: edits coming out of the plugin's own editor -----------
class Handler final : public IComponentHandler {
public:
    FZ_STATIC_REFCOUNT   // a member of the instance, which outlives the controller
    explicit Handler(Instance::Impl& o) : owner(o) {}
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (sameIid(iid, IComponentHandler::iid) || sameIid(iid, FUnknown::iid)) {
            *obj = static_cast<IComponentHandler*>(this); return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    tresult PLUGIN_API beginEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API performEdit(ParamID id, ParamValue v) override;
    tresult PLUGIN_API endEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API restartComponent(int32 flags) override;

    Instance::Impl& owner;
};

struct Editor;
class Frame final : public IPlugFrame {
public:
    FZ_STATIC_REFCOUNT   // a member of the editor, which releases the view first
    explicit Frame(Editor& e) : editor(e) {}
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (sameIid(iid, IPlugFrame::iid) || sameIid(iid, FUnknown::iid)) {
            *obj = static_cast<IPlugFrame*>(this); return kResultOk;
        }
        // How a Linux plugin finds the host's run loop: through its frame.
        if (sameIid(iid, Linux::IRunLoop::iid)) {
            *obj = static_cast<Linux::IRunLoop*>(&g_runLoop); return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* size) override;

    Editor& editor;
};

// An open editor: our top-level X window with the plugin's view inside.
struct Editor {
    Instance::Impl* owner = nullptr;
    IPlugView*      view  = nullptr;
    Frame           frame{*this};
    Window          window = 0;
    int             w = 0, h = 0;
    bool            resizable = false;
    bool            closeAsked = false;

    void fixSize() {
        if (resizable) return;
        // Not resizable: the window manager is told so (min = max).
        if (XSizeHints* hints = g_x.xAllocSizeHints()) {
            hints->flags = PMinSize | PMaxSize;
            hints->min_width = hints->max_width = w;
            hints->min_height = hints->max_height = h;
            g_x.xSetWMNormalHints(g_x.dpy, window, hints);
            g_x.xFree(hints);
        }
    }
    void resizeTo(int nw, int nh) {
        nw = std::max(nw, 16);
        nh = std::max(nh, 16);
        if (nw == w && nh == h) return;
        w = nw;
        h = nh;
        fixSize();
        g_x.xResizeWindow(g_x.dpy, window, static_cast<unsigned>(w), static_cast<unsigned>(h));
        g_x.xFlush(g_x.dpy);
    }
};
std::vector<Editor*> g_editors;

} // namespace

// --- An instance ------------------------------------------------------------------
struct Instance::Impl {
    PluginInfo                 info;
    double                     sampleRate = 48000.0;
    IPtr<IComponent>           component;
    IPtr<IAudioProcessor>      processor;
    IPtr<IEditController>      controller;
    IPtr<IConnectionPoint>     componentCP, controllerCP;
    bool                       separateController = false;
    bool                       active = false, processing = false;
    Handler                    handler{*this};
    std::vector<ParamInfo>     params;
    bool                       paramsStale = false;
    std::unique_ptr<Editor>    editor;

    // The audio thread's: one buffer set per bus, the main buses' channel counts.
    struct Bus {
        std::vector<float>  data;    // planar, kMaxBlock per channel
        std::vector<float*> ptrs;
    };
    std::vector<Bus>             inBuses, outBuses;
    std::vector<AudioBusBuffers> inBufs, outBufs;
    int                          mainIn = 0, mainOut = 0;   // channels of bus 0, each way
    ParamChanges                 inChanges{512}, outChanges{64};
    EventList                    inEvents, outEvents;
    ProcessContext               context{};
    ProcessData                  data;
    EditQueue                    edits;

    bool init(IPluginFactory* f, const TUID cid, std::string& error);
    void readParams();
    ~Impl();
};

namespace {

// The one way an editor goes: the view first (it may still call back while it
// does), then our window.
void shutEditor(Instance::Impl& I) {
    Editor* ed = I.editor.get();
    if (!ed) return;
    g_editors.erase(std::remove(g_editors.begin(), g_editors.end(), ed), g_editors.end());
    if (ed->view) {
        ed->view->removed();
        ed->view->setFrame(nullptr);
        ed->view->release();
        ed->view = nullptr;
    }
    if (g_x.ok && ed->window) {
        g_x.xDestroyWindow(g_x.dpy, ed->window);
        g_x.xFlush(g_x.dpy);
    }
    I.editor.reset();
}

tresult PLUGIN_API Handler::performEdit(ParamID id, ParamValue v) {
    owner.edits.push({id, v});
    return kResultOk;
}

tresult PLUGIN_API Handler::restartComponent(int32 flags) {
    if (flags & (kParamTitlesChanged | kParamValuesChanged | kReloadComponent)) owner.paramsStale = true;
    return kResultOk;
}

tresult PLUGIN_API Frame::resizeView(IPlugView* view, ViewRect* size) {
    if (!view || !size || view != editor.view) return kInvalidArgument;
    editor.resizeTo(size->getWidth(), size->getHeight());
    view->onSize(size);
    return kResultTrue;
}

template <class T>
IPtr<T> queryAs(FUnknown* from) {
    void* p = nullptr;
    if (from && from->queryInterface(T::iid, &p) == kResultOk && p) return owned(static_cast<T*>(p));
    return {};
}

void prepareBus(Instance::Impl::Bus& b, AudioBusBuffers& buf, int channels) {
    b.data.assign(static_cast<std::size_t>(channels) * kMaxBlock, 0.0f);
    b.ptrs.resize(static_cast<std::size_t>(channels));
    for (int c = 0; c < channels; ++c) b.ptrs[static_cast<std::size_t>(c)] = b.data.data() + static_cast<std::size_t>(c) * kMaxBlock;
    buf.numChannels      = channels;
    buf.silenceFlags     = 0;
    buf.channelBuffers32 = b.ptrs.data();
}

} // namespace

bool Instance::Impl::init(IPluginFactory* f, const TUID cid, std::string& error) {
    {
        void* p = nullptr;
        if (f->createInstance(cid, IComponent::iid, &p) != kResultOk || !p) { error = "the factory made no component"; return false; }
        component = owned(static_cast<IComponent*>(p));
    }
    if (component->initialize(&g_host) != kResultOk) { error = "the component would not initialize"; component = nullptr; return false; }

    // The controller: a class of its own (the usual), or the component itself.
    TUID ctrlCid;
    if (component->getControllerClassId(ctrlCid) == kResultOk) {
        void* p = nullptr;
        if (f->createInstance(ctrlCid, IEditController::iid, &p) == kResultOk && p) {
            controller = owned(static_cast<IEditController*>(p));
            if (controller->initialize(&g_host) == kResultOk) separateController = true;
            else controller = nullptr;
        }
    }
    if (!controller) controller = queryAs<IEditController>(component);

    // Two halves that talk: connect them (a JUCE plugin finds its processor this way).
    if (separateController) {
        componentCP  = queryAs<IConnectionPoint>(component);
        controllerCP = queryAs<IConnectionPoint>(controller);
        if (componentCP && controllerCP) {
            componentCP->connect(controllerCP);
            controllerCP->connect(componentCP);
        }
    }
    if (controller) {
        controller->setComponentHandler(&handler);
        MemoryStream s;
        if (component->getState(&s) == kResultOk) {
            s.rewind();
            controller->setComponentState(&s);
        }
    }

    processor = queryAs<IAudioProcessor>(component);
    if (!processor) { error = "not an audio processor"; return false; }
    if (processor->canProcessSampleSize(kSample32) != kResultTrue) { error = "no 32-bit processing"; return false; }

    // Stereo on the main buses; the others as they are, and off.
    const int32 nIn  = component->getBusCount(kAudio, kInput);
    const int32 nOut = component->getBusCount(kAudio, kOutput);
    std::vector<SpeakerArrangement> ins(static_cast<std::size_t>(nIn)), outs(static_cast<std::size_t>(nOut));
    for (int32 i = 0; i < nIn; ++i) {
        SpeakerArrangement a = SpeakerArr::kStereo;
        if (i > 0) processor->getBusArrangement(kInput, i, a);
        ins[static_cast<std::size_t>(i)] = a;
    }
    for (int32 i = 0; i < nOut; ++i) {
        SpeakerArrangement a = SpeakerArr::kStereo;
        if (i > 0) processor->getBusArrangement(kOutput, i, a);
        outs[static_cast<std::size_t>(i)] = a;
    }
    processor->setBusArrangements(ins.data(), nIn, outs.data(), nOut);   // a refusal leaves its own
    inBuses.resize(static_cast<std::size_t>(nIn));
    inBufs.resize(static_cast<std::size_t>(nIn));
    outBuses.resize(static_cast<std::size_t>(nOut));
    outBufs.resize(static_cast<std::size_t>(nOut));
    for (int32 i = 0; i < nIn; ++i) {
        SpeakerArrangement a = 0;
        processor->getBusArrangement(kInput, i, a);
        component->activateBus(kAudio, kInput, i, i == 0);
        prepareBus(inBuses[static_cast<std::size_t>(i)], inBufs[static_cast<std::size_t>(i)], channelsOf(a));
        if (i == 0) mainIn = channelsOf(a);
    }
    for (int32 i = 0; i < nOut; ++i) {
        SpeakerArrangement a = 0;
        processor->getBusArrangement(kOutput, i, a);
        component->activateBus(kAudio, kOutput, i, i == 0);
        prepareBus(outBuses[static_cast<std::size_t>(i)], outBufs[static_cast<std::size_t>(i)], channelsOf(a));
        if (i == 0) mainOut = channelsOf(a);
    }
    for (int32 i = 0; i < component->getBusCount(kEvent, kInput); ++i) component->activateBus(kEvent, kInput, i, false);
    for (int32 i = 0; i < component->getBusCount(kEvent, kOutput); ++i) component->activateBus(kEvent, kOutput, i, false);

    ProcessSetup setup{kRealtime, kSample32, kMaxBlock, sampleRate};
    if (processor->setupProcessing(setup) != kResultOk) { error = "setupProcessing refused"; return false; }
    if (component->setActive(true) != kResultOk) { error = "would not activate"; return false; }
    active = true;
    processor->setProcessing(true);   // many plugins answer kNotImplemented; that is fine
    processing = true;

    context.sampleRate         = sampleRate;
    context.tempo              = 120.0;
    context.timeSigNumerator   = 4;
    context.timeSigDenominator = 4;
    context.state = ProcessContext::kTempoValid | ProcessContext::kTimeSigValid | ProcessContext::kContTimeValid;

    data.processMode            = kRealtime;
    data.symbolicSampleSize     = kSample32;
    data.numInputs              = nIn;
    data.numOutputs             = nOut;
    data.inputs                 = inBufs.empty() ? nullptr : inBufs.data();
    data.outputs                = outBufs.empty() ? nullptr : outBufs.data();
    data.inputParameterChanges  = &inChanges;
    data.outputParameterChanges = &outChanges;
    data.inputEvents            = &inEvents;
    data.outputEvents           = &outEvents;
    data.processContext         = &context;

    readParams();
    return true;
}

void Instance::Impl::readParams() {
    params.clear();
    paramsStale = false;
    if (!controller) return;
    const int32 n = controller->getParameterCount();
    for (int32 i = 0; i < n; ++i) {
        ParameterInfo pi{};
        if (controller->getParameterInfo(i, pi) != kResultOk) continue;
        ParamInfo p;
        p.id           = pi.id;
        p.title        = utf8(pi.title, 128);
        p.units        = utf8(pi.units, 128);
        p.defaultValue = pi.defaultNormalizedValue;
        p.steps        = pi.stepCount;
        p.readOnly     = (pi.flags & ParameterInfo::kIsReadOnly) != 0;
        p.hidden       = (pi.flags & ParameterInfo::kIsHidden) != 0;
        p.bypass       = (pi.flags & ParameterInfo::kIsBypass) != 0;
        params.push_back(std::move(p));
    }
}

Instance::Impl::~Impl() {
    shutEditor(*this);
    if (processor && processing) processor->setProcessing(false);
    if (component && active) component->setActive(false);
    if (componentCP && controllerCP) {
        componentCP->disconnect(controllerCP);
        controllerCP->disconnect(componentCP);
    }
    componentCP  = nullptr;
    controllerCP = nullptr;
    if (controller) {
        controller->setComponentHandler(nullptr);
        if (separateController) controller->terminate();
    }
    controller = nullptr;
    processor  = nullptr;
    if (component) component->terminate();
    component = nullptr;
}

// --- The public face ---------------------------------------------------------------
bool supported() { return true; }

std::vector<std::string> searchPaths() {
    std::vector<std::string> out;
    if (const char* home = std::getenv("HOME"); home && *home) out.push_back(std::string(home) + "/.vst3");
    out.push_back("/usr/local/lib/vst3");
    out.push_back("/usr/lib/vst3");
    if (const char* extra = std::getenv("VST3_PATH")) {
        std::string s = extra;
        std::size_t start = 0;
        while (start <= s.size()) {
            const std::size_t colon = s.find(':', start);
            const std::string p = s.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
            if (!p.empty()) out.push_back(p);
            if (colon == std::string::npos) break;
            start = colon + 1;
        }
    }
    return out;
}

const std::vector<PluginInfo>& plugins(bool rescan) {
    if (g_scanned && !rescan) return known();
    g_scanned = true;
    std::vector<PluginInfo> found;
    namespace fs = std::filesystem;
    for (const std::string& root : searchPaths()) {
        std::error_code ec;
        if (!fs::is_directory(root, ec)) continue;
        for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            const fs::path& p = it->path();
            if (p.extension() != ".vst3") continue;
            if (it->is_directory(ec)) it.disable_recursion_pending();   // a bundle, not a folder of them
            const std::string bundle = p.string();
            if (!scanModuleInfo(bundle, found)) scanByLoading(bundle, found);
        }
    }
    // The first of each class wins: ~/.vst3 before the system's folders.
    std::vector<PluginInfo> unique;
    for (PluginInfo& p : found)
        if (std::none_of(unique.begin(), unique.end(), [&](const PluginInfo& u) { return u.uid == p.uid; }))
            unique.push_back(std::move(p));
    std::sort(unique.begin(), unique.end(), [](const PluginInfo& a, const PluginInfo& b) {
        if (a.instrument != b.instrument) return !a.instrument;
        return a.name < b.name;
    });
    known() = std::move(unique);
    std::fprintf(stderr, "[vst3] %zu plugin(s) found\n", known().size());
    return known();
}

const PluginInfo* find(const std::string& uid) {
    for (const PluginInfo& p : plugins())
        if (p.uid == uid) return &p;
    return nullptr;
}

std::shared_ptr<Instance> Instance::create(const PluginInfo& info, double sampleRate, std::string* error) {
    auto fail = [&](const std::string& why) -> std::shared_ptr<Instance> {
        if (error) *error = why;
        std::fprintf(stderr, "[vst3] %s: %s\n", info.name.c_str(), why.c_str());
        return nullptr;
    };
    TUID cid;
    if (!parseUid(info.uid, cid)) return fail("bad class id");
    std::string why;
    IPluginFactory* f = loadModule(info.path, why);
    if (!f) return fail(why);
    auto impl = std::make_unique<Impl>();
    impl->info       = info;
    impl->sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    if (!impl->init(f, cid, why)) return fail(why);
    return std::shared_ptr<Instance>(new Instance(std::move(impl)));
}

Instance::Instance(std::unique_ptr<Impl> impl) : m(std::move(impl)) {}

Instance::~Instance() {
    closeEditor();
    m.reset();
}

const PluginInfo& Instance::info() const { return m->info; }

const std::vector<ParamInfo>& Instance::params() const {
    if (m->paramsStale) m->readParams();
    return m->params;
}

double Instance::param(uint32_t id) const {
    return m->controller ? m->controller->getParamNormalized(id) : 0.0;
}

void Instance::setParam(uint32_t id, double v) {
    v = std::clamp(v, 0.0, 1.0);
    if (m->controller) m->controller->setParamNormalized(id, v);
    m->edits.push({id, v});
}

std::string Instance::display(uint32_t id, double v) const {
    if (!m->controller) return {};
    String128 s{};
    if (m->controller->getParamStringByValue(id, v, s) != kResultOk) return {};
    return utf8(s, 128);
}

// Both halves, one after the other, each with its length in front:
// "FZV3" u32 component-bytes, bytes, u32 controller-bytes, bytes.
std::string Instance::state() const {
    MemoryStream comp, ctrl;
    if (m->component) m->component->getState(&comp);
    if (m->controller && m->separateController) m->controller->getState(&ctrl);
    std::string out = "FZV3";
    auto put = [&](const std::string& b) {
        const uint32_t n = static_cast<uint32_t>(b.size());
        for (int i = 0; i < 4; ++i) out += static_cast<char>((n >> (i * 8)) & 0xFF);
        out += b;
    };
    put(comp.data);
    put(ctrl.data);
    return out;
}

bool Instance::setState(const std::string& bytes) {
    if (bytes.size() < 12 || bytes.compare(0, 4, "FZV3") != 0) return false;
    std::size_t at = 4;
    auto take = [&](std::string& out) {
        if (at + 4 > bytes.size()) return false;
        uint32_t n = 0;
        for (int i = 0; i < 4; ++i) n |= static_cast<uint32_t>(static_cast<unsigned char>(bytes[at + i])) << (i * 8);
        at += 4;
        if (at + n > bytes.size()) return false;
        out = bytes.substr(at, n);
        at += n;
        return true;
    };
    std::string comp, ctrl;
    if (!take(comp) || !take(ctrl)) return false;
    if (!comp.empty() && m->component) {
        MemoryStream s(comp);
        m->component->setState(&s);
        if (m->controller) {
            MemoryStream again(comp);
            m->controller->setComponentState(&again);
        }
    }
    if (!ctrl.empty() && m->controller && m->separateController) {
        MemoryStream s(ctrl);
        m->controller->setState(&s);
    }
    m->paramsStale = true;
    return true;
}

bool Instance::hasEditor() const { return m->controller != nullptr; }
bool Instance::editorOpen() const { return m->editor != nullptr; }

bool Instance::openEditor(const std::string& title) {
    if (m->editor) return true;
    if (!m->controller || !g_x.load()) return false;
    IPlugView* view = m->controller->createView(ViewType::kEditor);
    if (!view) return false;
    if (view->isPlatformTypeSupported(kPlatformTypeX11EmbedWindowID) != kResultTrue) {
        std::fprintf(stderr, "[vst3] %s: its editor does not embed in X11\n", m->info.name.c_str());
        view->release();
        return false;
    }
    auto ed = std::make_unique<Editor>();
    ed->owner     = m.get();
    ed->view      = view;
    ed->resizable = view->canResize() == kResultTrue;
    ViewRect r{};
    view->getSize(&r);
    ed->w = std::max(r.getWidth(), 64);
    ed->h = std::max(r.getHeight(), 48);

    const int screen = g_x.xDefaultScreen(g_x.dpy);
    ed->window = g_x.xCreateSimpleWindow(g_x.dpy, g_x.xRootWindow(g_x.dpy, screen), 80, 80,
                                        static_cast<unsigned>(ed->w), static_cast<unsigned>(ed->h), 0,
                                        g_x.xBlackPixel(g_x.dpy, screen), g_x.xBlackPixel(g_x.dpy, screen));
    if (!ed->window) { view->release(); return false; }
    const std::string name = title.empty() ? m->info.name : title;
    g_x.xStoreName(g_x.dpy, ed->window, name.c_str());
    g_x.xChangeProperty(g_x.dpy, ed->window, g_x.netWmName, g_x.utf8String, 8, PropModeReplace,
                       reinterpret_cast<const unsigned char*>(name.data()), static_cast<int>(name.size()));
    Atom protocols[] = {g_x.wmDelete};
    g_x.xSetWMProtocols(g_x.dpy, ed->window, protocols, 1);
    g_x.xSelectInput(g_x.dpy, ed->window, StructureNotifyMask);
    ed->fixSize();
    g_x.xSync(g_x.dpy, False);   // the window exists before the plugin's own connection looks for it

    view->setFrame(&ed->frame);
    if (view->attached(reinterpret_cast<void*>(static_cast<uintptr_t>(ed->window)),
                       kPlatformTypeX11EmbedWindowID) != kResultOk) {
        std::fprintf(stderr, "[vst3] %s: its editor would not attach\n", m->info.name.c_str());
        view->setFrame(nullptr);
        view->release();
        g_x.xDestroyWindow(g_x.dpy, ed->window);
        g_x.xFlush(g_x.dpy);
        return false;
    }
    // Attaching may have changed its mind about its size.
    if (view->getSize(&r) == kResultOk) ed->resizeTo(r.getWidth(), r.getHeight());
    g_x.xMapRaised(g_x.dpy, ed->window);
    g_x.xFlush(g_x.dpy);
    g_editors.push_back(ed.get());
    m->editor = std::move(ed);
    return true;
}

void Instance::closeEditor() {
    if (m) shutEditor(*m);
}

void Instance::process(float* buf, unsigned frames, unsigned channels) {
    Impl& I = *m;
    if (!I.processor || channels == 0 || !buf) return;
    for (unsigned done = 0; done < frames;) {
        const unsigned n = std::min<unsigned>(frames - done, static_cast<unsigned>(kMaxBlock));
        float* b = buf + static_cast<std::size_t>(done) * channels;

        // In: the strip's channels to the main bus (a mono bus gets the left).
        for (int c = 0; c < I.mainIn; ++c) {
            float* dst = I.inBuses[0].ptrs[static_cast<std::size_t>(c)];
            const unsigned src = std::min<unsigned>(static_cast<unsigned>(c), channels - 1);
            for (unsigned f = 0; f < n; ++f) dst[f] = b[f * channels + src];
        }
        for (std::size_t i = 1; i < I.inBuses.size(); ++i)
            std::fill(I.inBuses[i].data.begin(), I.inBuses[i].data.end(), 0.0f);

        // The edits since the last block, the newest of each.
        I.inChanges.clear();
        I.outChanges.clear();
        EditQueue::Edit e;
        while (I.edits.pop(e)) {
            int32 index = 0;
            if (IParamValueQueue* q = I.inChanges.addParameterData(e.id, index)) {
                int32 pi = 0;
                q->addPoint(0, e.value, pi);
            }
        }

        I.data.numSamples = static_cast<int32>(n);
        for (auto& ob : I.outBufs) ob.silenceFlags = 0;
        for (auto& ib : I.inBufs) ib.silenceFlags = 0;
        const bool ok = I.processor->process(I.data) == kResultOk;
        I.context.continousTimeSamples += n;
        I.context.projectTimeSamples   += n;

        // Out: the main bus back over the strip (a mono bus to both sides).
        if (ok && I.mainOut > 0)
            for (unsigned c = 0; c < channels; ++c) {
                const float* src = I.outBuses[0].ptrs[std::min<std::size_t>(c, static_cast<std::size_t>(I.mainOut) - 1)];
                for (unsigned f = 0; f < n; ++f) b[f * channels + c] = src[f];
            }
        done += n;
    }
}

void idle() {
    if (g_x.ok) {
        while (g_x.xPending(g_x.dpy) > 0) {
            XEvent ev;
            g_x.xNextEvent(g_x.dpy, &ev);
            for (Editor* ed : g_editors) {
                if (ev.xany.window != ed->window) continue;
                if (ev.type == ClientMessage && static_cast<Atom>(ev.xclient.data.l[0]) == g_x.wmDelete)
                    ed->closeAsked = true;
                else if (ev.type == ConfigureNotify && ed->resizable && ed->view &&
                         (ev.xconfigure.width != ed->w || ev.xconfigure.height != ed->h)) {
                    // Dragged by the user: the plugin may round it to what it can be.
                    ViewRect r{0, 0, ev.xconfigure.width, ev.xconfigure.height};
                    ed->view->checkSizeConstraint(&r);
                    ed->w = ev.xconfigure.width;
                    ed->h = ev.xconfigure.height;
                    if (r.getWidth() != ed->w || r.getHeight() != ed->h) ed->resizeTo(r.getWidth(), r.getHeight());
                    ed->view->onSize(&r);
                }
                break;
            }
        }
    }
    g_runLoop.service();
    // Closed with the window's own button: the view goes now, outside the loop above.
    std::vector<Instance::Impl*> closing;
    for (Editor* ed : g_editors)
        if (ed->closeAsked) closing.push_back(ed->owner);
    for (Instance::Impl* owner : closing) shutEditor(*owner);
}

#endif // FITZEL_VST3

} // namespace fitzel::vst3
