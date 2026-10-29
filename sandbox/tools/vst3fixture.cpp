// vst3fixture -- a VST3 plugin that exists to be hosted by vst3check.
//
// A gain and a polarity switch, built the way most real plugins are built
// rather than the simplest way: processor and controller are separate classes
// that find each other through connection points and a message the HOST has to
// make, the state comes in two halves, and the editor is an X11 window inside
// the host's, kept alive by the host's run loop (a socket and a timer), that
// asks to be resized the moment it is attached and turns a knob by itself
// through the component handler. Each of those is a place a host goes wrong.
//
// What happened is counted, and fitzelFixtureStat() hands the counts to the
// check, which looks the symbol up in the module the host already loaded.
//
// Built only with FITZEL_VST3, as a bundle in build/<tree>/bin/vst3fixture.

#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#define INIT_CLASS_IID
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/vstspeaker.h"

#include <X11/Xlib.h>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

struct Stats {
    std::atomic<int> messages{0}, processed{0}, terminated{0}, attached{0}, removed{0};
    std::atomic<int> fd{0}, timer{0}, onSize{0}, edits{0};
} g_stats;

const FUID kProcessorUid(0x5A1E7A11, 0x00F17E10, 0x7E57A11D, 0x00000001);
const FUID kControllerUid(0x5A1E7A11, 0x00F17E10, 0x7E57A11D, 0x00000002);
enum : ParamID { kGain = 0, kInvert = 1 };

bool same(const TUID a, const FUID& b) { return FUnknownPrivate::iidEqual(a, b.toTUID()); }

#define FIX_REFCOUNT                                                       \
    std::atomic<uint32> refs{1};                                           \
    uint32 PLUGIN_API addRef() override { return ++refs; }                 \
    uint32 PLUGIN_API release() override {                                 \
        const uint32 r = --refs;                                           \
        if (r == 0) delete this;                                           \
        return r;                                                          \
    }

void copyString(String128 to, const char* from) {
    int i = 0;
    for (; from[i] && i < 127; ++i) to[i] = static_cast<char16>(from[i]);
    to[i] = 0;
}

// gain is normalized: 0.5 is unity, 1.0 is +6 dB.
float gainOf(double norm) { return static_cast<float>(norm * 2.0); }

// --- The processor ---------------------------------------------------------------
class Processor final : public IComponent, public IAudioProcessor, public IConnectionPoint {
public:
    FIX_REFCOUNT
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (same(iid, FUnknown::iid) || same(iid, IPluginBase::iid) || same(iid, IComponent::iid))
            *obj = static_cast<IComponent*>(this);
        else if (same(iid, IAudioProcessor::iid)) *obj = static_cast<IAudioProcessor*>(this);
        else if (same(iid, IConnectionPoint::iid)) *obj = static_cast<IConnectionPoint*>(this);
        else { *obj = nullptr; return kNoInterface; }
        addRef();
        return kResultOk;
    }

    tresult PLUGIN_API initialize(FUnknown*) override { return kResultOk; }
    tresult PLUGIN_API terminate() override { ++g_stats.terminated; return kResultOk; }

    tresult PLUGIN_API getControllerClassId(TUID cid) override {
        std::memcpy(cid, kControllerUid.toTUID(), sizeof(TUID));
        return kResultOk;
    }
    tresult PLUGIN_API setIoMode(IoMode) override { return kResultOk; }
    int32 PLUGIN_API getBusCount(MediaType type, BusDirection) override { return type == kAudio ? 1 : 0; }
    tresult PLUGIN_API getBusInfo(MediaType type, BusDirection dir, int32 index, BusInfo& bus) override {
        if (type != kAudio || index != 0) return kInvalidArgument;
        bus.mediaType    = kAudio;
        bus.direction    = dir;
        bus.channelCount = 2;
        copyString(bus.name, dir == kInput ? "In" : "Out");
        bus.busType = kMain;
        bus.flags   = BusInfo::kDefaultActive;
        return kResultOk;
    }
    tresult PLUGIN_API getRoutingInfo(RoutingInfo&, RoutingInfo&) override { return kNotImplemented; }
    tresult PLUGIN_API activateBus(MediaType, BusDirection, int32, TBool) override { return kResultOk; }
    tresult PLUGIN_API setActive(TBool) override { return kResultOk; }
    tresult PLUGIN_API setState(IBStream* s) override {
        double v[2] = {0.5, 0.0};
        int32 got = 0;
        if (!s || s->read(v, sizeof(v), &got) != kResultOk || got != static_cast<int32>(sizeof(v))) return kResultFalse;
        gain = v[0];
        invert = v[1];
        return kResultOk;
    }
    tresult PLUGIN_API getState(IBStream* s) override {
        double v[2] = {gain, invert};
        return s ? s->write(v, sizeof(v), nullptr) : kInvalidArgument;
    }

    tresult PLUGIN_API setBusArrangements(SpeakerArrangement* in, int32 nIn, SpeakerArrangement* out, int32 nOut) override {
        return nIn == 1 && nOut == 1 && in[0] == SpeakerArr::kStereo && out[0] == SpeakerArr::kStereo
            ? kResultTrue : kResultFalse;
    }
    tresult PLUGIN_API getBusArrangement(BusDirection, int32 index, SpeakerArrangement& a) override {
        if (index != 0) return kInvalidArgument;
        a = SpeakerArr::kStereo;
        return kResultOk;
    }
    tresult PLUGIN_API canProcessSampleSize(int32 size) override { return size == kSample32 ? kResultTrue : kResultFalse; }
    uint32 PLUGIN_API getLatencySamples() override { return 0; }
    tresult PLUGIN_API setupProcessing(ProcessSetup&) override { return kResultOk; }
    tresult PLUGIN_API setProcessing(TBool) override { return kResultOk; }
    uint32 PLUGIN_API getTailSamples() override { return 0; }
    tresult PLUGIN_API process(ProcessData& d) override {
        if (IParameterChanges* pc = d.inputParameterChanges)
            for (int32 i = 0; i < pc->getParameterCount(); ++i) {
                IParamValueQueue* q = pc->getParameterData(i);
                int32 off = 0;
                ParamValue v = 0;
                if (!q || q->getPointCount() <= 0 || q->getPoint(q->getPointCount() - 1, off, v) != kResultOk) continue;
                if (q->getParameterId() == kGain) gain = v;
                if (q->getParameterId() == kInvert) invert = v;
            }
        if (d.numInputs < 1 || d.numOutputs < 1) return kResultOk;
        const float g = gainOf(gain) * (invert > 0.5 ? -1.0f : 1.0f);
        const int32 ch = std::min(d.inputs[0].numChannels, d.outputs[0].numChannels);
        for (int32 c = 0; c < ch; ++c)
            for (int32 f = 0; f < d.numSamples; ++f)
                d.outputs[0].channelBuffers32[c][f] = d.inputs[0].channelBuffers32[c][f] * g;
        ++g_stats.processed;
        return kResultOk;
    }

    tresult PLUGIN_API connect(IConnectionPoint* other) override { peer = other; return kResultOk; }
    tresult PLUGIN_API disconnect(IConnectionPoint*) override { peer = nullptr; return kResultOk; }
    tresult PLUGIN_API notify(IMessage* m) override {
        int64 n = 0;
        if (m && std::strcmp(m->getMessageID(), "hello") == 0 && m->getAttributes() &&
            m->getAttributes()->getInt("n", n) == kResultOk && n == 42)
            ++g_stats.messages;
        return kResultOk;
    }

private:
    double            gain = 0.5, invert = 0.0;
    IConnectionPoint* peer = nullptr;
};

class Controller;

// --- The editor ------------------------------------------------------------------
class View final : public IPlugView, public Linux::IEventHandler, public Linux::ITimerHandler {
public:
    FIX_REFCOUNT
    explicit View(Controller& c) : ctrl(c) {}
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (same(iid, FUnknown::iid) || same(iid, IPlugView::iid)) *obj = static_cast<IPlugView*>(this);
        else if (same(iid, Linux::IEventHandler::iid)) *obj = static_cast<Linux::IEventHandler*>(this);
        else if (same(iid, Linux::ITimerHandler::iid)) *obj = static_cast<Linux::ITimerHandler*>(this);
        else { *obj = nullptr; return kNoInterface; }
        addRef();
        return kResultOk;
    }
    tresult PLUGIN_API isPlatformTypeSupported(FIDString t) override {
        return std::strcmp(t, kPlatformTypeX11EmbedWindowID) == 0 ? kResultTrue : kResultFalse;
    }
    tresult PLUGIN_API attached(void* parent, FIDString) override {
        dpy = XOpenDisplay(nullptr);
        if (!dpy) return kResultFalse;
        win = XCreateSimpleWindow(dpy, static_cast<Window>(reinterpret_cast<uintptr_t>(parent)), 0, 0,
                                  static_cast<unsigned>(rect.getWidth()), static_cast<unsigned>(rect.getHeight()),
                                  0, 0, 0x3a6ea5);
        XSelectInput(dpy, win, ExposureMask | StructureNotifyMask);
        XMapWindow(dpy, win);
        XFlush(dpy);
        ++g_stats.attached;
        // The run loop comes through the frame, as on every Linux host.
        void* p = nullptr;
        if (frame && frame->queryInterface(Linux::IRunLoop::iid, &p) == kResultOk && p) {
            loop = static_cast<Linux::IRunLoop*>(p);
            loop->registerEventHandler(this, ConnectionNumber(dpy));
            loop->registerTimer(this, 15);
        }
        // Bigger than it said at first: the host has to follow.
        ViewRect want(0, 0, 360, 240);
        if (frame) frame->resizeView(this, &want);
        return kResultOk;
    }
    tresult PLUGIN_API removed() override {
        if (loop) {
            loop->unregisterEventHandler(this);
            loop->unregisterTimer(this);
            loop->release();
            loop = nullptr;
        }
        if (dpy) {
            XDestroyWindow(dpy, win);
            XCloseDisplay(dpy);
            dpy = nullptr;
        }
        ++g_stats.removed;
        return kResultOk;
    }
    tresult PLUGIN_API onWheel(float) override { return kResultFalse; }
    tresult PLUGIN_API onKeyDown(char16, int16, int16) override { return kResultFalse; }
    tresult PLUGIN_API onKeyUp(char16, int16, int16) override { return kResultFalse; }
    tresult PLUGIN_API getSize(ViewRect* r) override { if (r) *r = rect; return kResultOk; }
    tresult PLUGIN_API onSize(ViewRect* r) override {
        if (!r) return kInvalidArgument;
        rect = *r;
        ++g_stats.onSize;
        if (dpy) {
            XResizeWindow(dpy, win, static_cast<unsigned>(rect.getWidth()), static_cast<unsigned>(rect.getHeight()));
            XFlush(dpy);
        }
        return kResultOk;
    }
    tresult PLUGIN_API onFocus(TBool) override { return kResultOk; }
    tresult PLUGIN_API setFrame(IPlugFrame* f) override { frame = f; return kResultOk; }
    tresult PLUGIN_API canResize() override { return kResultTrue; }
    tresult PLUGIN_API checkSizeConstraint(ViewRect* r) override {
        if (r->getWidth() < 120) r->right = r->left + 120;
        if (r->getHeight() < 80) r->bottom = r->top + 80;
        return kResultOk;
    }

    void PLUGIN_API onFDIsSet(Linux::FileDescriptor) override {
        ++g_stats.fd;
        while (dpy && XPending(dpy)) {
            XEvent e;
            XNextEvent(dpy, &e);
        }
    }
    void PLUGIN_API onTimer() override;

private:
    Controller&      ctrl;
    IPlugFrame*      frame = nullptr;
    Linux::IRunLoop* loop = nullptr;
    Display*         dpy = nullptr;
    Window           win = 0;
    ViewRect         rect{0, 0, 300, 200};
};

// --- The controller --------------------------------------------------------------
class Controller final : public IEditController, public IConnectionPoint {
public:
    FIX_REFCOUNT
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (same(iid, FUnknown::iid) || same(iid, IPluginBase::iid) || same(iid, IEditController::iid))
            *obj = static_cast<IEditController*>(this);
        else if (same(iid, IConnectionPoint::iid)) *obj = static_cast<IConnectionPoint*>(this);
        else { *obj = nullptr; return kNoInterface; }
        addRef();
        return kResultOk;
    }
    tresult PLUGIN_API initialize(FUnknown* context) override {
        void* p = nullptr;
        if (context && context->queryInterface(IHostApplication::iid, &p) == kResultOk) host = static_cast<IHostApplication*>(p);
        return kResultOk;
    }
    tresult PLUGIN_API terminate() override {
        if (host) { host->release(); host = nullptr; }
        if (handler) { handler->release(); handler = nullptr; }
        ++g_stats.terminated;
        return kResultOk;
    }
    tresult PLUGIN_API setComponentState(IBStream* s) override {
        double v[2];
        int32 got = 0;
        if (!s || s->read(v, sizeof(v), &got) != kResultOk || got != static_cast<int32>(sizeof(v))) return kResultFalse;
        values[kGain] = v[0];
        values[kInvert] = v[1];
        return kResultOk;
    }
    // The controller's own half: something only the editor cares about.
    tresult PLUGIN_API setState(IBStream* s) override {
        int32 got = 0;
        return s && s->read(&uiWidth, sizeof(uiWidth), &got) == kResultOk && got == sizeof(uiWidth) ? kResultOk : kResultFalse;
    }
    tresult PLUGIN_API getState(IBStream* s) override { return s ? s->write(&uiWidth, sizeof(uiWidth), nullptr) : kInvalidArgument; }
    int32 PLUGIN_API getParameterCount() override { return 2; }
    tresult PLUGIN_API getParameterInfo(int32 index, ParameterInfo& p) override {
        if (index < 0 || index > 1) return kInvalidArgument;
        std::memset(&p, 0, sizeof(p));
        p.id = static_cast<ParamID>(index);
        copyString(p.title, index == 0 ? "Gain" : "Invert");
        copyString(p.shortTitle, index == 0 ? "Gain" : "Inv");
        copyString(p.units, index == 0 ? "dB" : "");
        p.stepCount = index == 0 ? 0 : 1;
        p.defaultNormalizedValue = index == 0 ? 0.5 : 0.0;
        p.flags = ParameterInfo::kCanAutomate;
        return kResultOk;
    }
    tresult PLUGIN_API getParamStringByValue(ParamID id, ParamValue v, String128 s) override {
        char buf[64];
        if (id == kGain) {
            const float g = gainOf(v);
            if (g <= 0.0f) std::snprintf(buf, sizeof(buf), "-inf");
            else std::snprintf(buf, sizeof(buf), "%.2f", 20.0 * std::log10(g));
        } else if (id == kInvert) {
            std::snprintf(buf, sizeof(buf), "%s", v > 0.5 ? "On" : "Off");
        } else {
            return kInvalidArgument;
        }
        copyString(s, buf);
        return kResultOk;
    }
    tresult PLUGIN_API getParamValueByString(ParamID, TChar*, ParamValue&) override { return kNotImplemented; }
    ParamValue PLUGIN_API normalizedParamToPlain(ParamID, ParamValue v) override { return v; }
    ParamValue PLUGIN_API plainParamToNormalized(ParamID, ParamValue v) override { return v; }
    ParamValue PLUGIN_API getParamNormalized(ParamID id) override { return id < 2 ? values[id] : 0.0; }
    tresult PLUGIN_API setParamNormalized(ParamID id, ParamValue v) override {
        if (id >= 2) return kInvalidArgument;
        values[id] = v;
        return kResultOk;
    }
    tresult PLUGIN_API setComponentHandler(IComponentHandler* h) override {
        if (h) h->addRef();
        if (handler) handler->release();
        handler = h;
        return kResultOk;
    }
    IPlugView* PLUGIN_API createView(FIDString name) override {
        return name && std::strcmp(name, ViewType::kEditor) == 0 ? new View(*this) : nullptr;
    }

    // The two halves say hello through a message the host makes.
    tresult PLUGIN_API connect(IConnectionPoint* other) override {
        peer = other;
        if (!host || !peer) return kResultOk;
        void* p = nullptr;
        TUID iid;
        std::memcpy(iid, IMessage::iid.toTUID(), sizeof(TUID));
        if (host->createInstance(iid, iid, &p) == kResultOk && p) {
            auto* m = static_cast<IMessage*>(p);
            m->setMessageID("hello");
            m->getAttributes()->setInt("n", 42);
            peer->notify(m);
            m->release();
        }
        return kResultOk;
    }
    tresult PLUGIN_API disconnect(IConnectionPoint*) override { peer = nullptr; return kResultOk; }
    tresult PLUGIN_API notify(IMessage*) override { return kResultOk; }

    // As a knob turned in the editor: the controller changes, the host is told.
    void turn(ParamID id, ParamValue v) {
        values[id] = v;
        if (handler) {
            handler->beginEdit(id);
            handler->performEdit(id, v);
            handler->endEdit(id);
            ++g_stats.edits;
        }
    }

    int32 uiWidth = 300;

private:
    IHostApplication*  host = nullptr;
    IComponentHandler* handler = nullptr;
    IConnectionPoint*  peer = nullptr;
    double             values[2] = {0.5, 0.0};
};

void PLUGIN_API View::onTimer() {
    // The first tick turns the gain to 0.75 (+3.5 dB), as a hand on the knob would.
    if (g_stats.timer++ == 0) ctrl.turn(kGain, 0.75);
}

// --- The factory -----------------------------------------------------------------
class Factory final : public IPluginFactory2 {
public:
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (same(iid, FUnknown::iid) || same(iid, IPluginFactory::iid) || same(iid, IPluginFactory2::iid)) {
            *obj = static_cast<IPluginFactory2*>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    tresult PLUGIN_API getFactoryInfo(PFactoryInfo* info) override {
        *info = PFactoryInfo("Fitzel", "", "", PFactoryInfo::kUnicode);
        return kResultOk;
    }
    int32 PLUGIN_API countClasses() override { return 2; }
    tresult PLUGIN_API getClassInfo(int32 i, PClassInfo* info) override {
        PClassInfo2 c2;
        if (getClassInfo2(i, &c2) != kResultOk) return kInvalidArgument;
        *info = PClassInfo(c2.cid, c2.cardinality, c2.category, c2.name);
        return kResultOk;
    }
    tresult PLUGIN_API getClassInfo2(int32 i, PClassInfo2* info) override {
        if (i == 0)
            *info = PClassInfo2(kProcessorUid.toTUID(), PClassInfo::kManyInstances, kVstAudioEffectClass,
                                "Fitzel Test Gain", 0, "Fx|Tools", "Fitzel", "1.0.0", kVstVersionString);
        else if (i == 1)
            *info = PClassInfo2(kControllerUid.toTUID(), PClassInfo::kManyInstances, kVstComponentControllerClass,
                                "Fitzel Test Gain Controller", 0, "", "Fitzel", "1.0.0", kVstVersionString);
        else
            return kInvalidArgument;
        return kResultOk;
    }
    tresult PLUGIN_API createInstance(FIDString cid, FIDString iid, void** obj) override {
        FUnknown* made = nullptr;
        if (FUnknownPrivate::iidEqual(cid, kProcessorUid.toTUID())) made = static_cast<IComponent*>(new Processor);
        else if (FUnknownPrivate::iidEqual(cid, kControllerUid.toTUID())) made = static_cast<IEditController*>(new Controller);
        if (!made) { *obj = nullptr; return kNoInterface; }
        const tresult r = made->queryInterface(iid, obj);
        made->release();
        return r;
    }
};
Factory g_factory;

} // namespace

extern "C" {
SMTG_EXPORT_SYMBOL IPluginFactory* PLUGIN_API GetPluginFactory() { return &g_factory; }
SMTG_EXPORT_SYMBOL bool ModuleEntry(void*) { return true; }
SMTG_EXPORT_SYMBOL bool ModuleExit() { return true; }

SMTG_EXPORT_SYMBOL int fitzelFixtureStat(const char* what) {
    const struct { const char* name; std::atomic<int>* v; } all[] = {
        {"messages", &g_stats.messages}, {"processed", &g_stats.processed},
        {"terminated", &g_stats.terminated}, {"attached", &g_stats.attached},
        {"removed", &g_stats.removed}, {"fd", &g_stats.fd}, {"timer", &g_stats.timer},
        {"onSize", &g_stats.onSize}, {"edits", &g_stats.edits},
    };
    for (const auto& s : all)
        if (std::strcmp(s.name, what) == 0) return s.v->load();
    return -1;
}
}
