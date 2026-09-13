// Integration tests for the delivered DLL, using only public VST3 interfaces.
// The plugin implementation is never included or compiled into this harness.
#define NOMINMAX
#include <windows.h>
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/gui/iplugview.h"
#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/common/memorystream.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {
constexpr ParamID levelId = 0;
constexpr ParamID bypassId = 1;

void require(bool condition, const std::string& reason) {
    if (!condition) throw std::runtime_error(reason);
}
void ok(tresult result, const std::string& reason) {
    require(result == kResultOk, reason + " (result " + std::to_string(result) + ")");
}
void close(double actual, double expected, const std::string& reason, double tolerance = 2e-6) {
    require(std::isfinite(actual) && std::abs(actual - expected) <= tolerance,
            reason + ": expected " + std::to_string(expected) + ", got " + std::to_string(actual));
}

struct Module {
    HMODULE handle{};
    IPluginFactory* factory{};
    bool (*exitDll)(){};
    TUID processorId{};
    explicit Module(const std::filesystem::path& binary) {
        handle = LoadLibraryW(binary.c_str());
        require(handle != nullptr, "LoadLibraryW failed: " + std::to_string(GetLastError()));
        auto init = reinterpret_cast<bool (*)()>(GetProcAddress(handle, "InitDll"));
        exitDll = reinterpret_cast<bool (*)()>(GetProcAddress(handle, "ExitDll"));
        require(init && exitDll && init(), "InitDll/ExitDll exports and initialization");
        auto getFactory = reinterpret_cast<GetFactoryProc>(GetProcAddress(handle, "GetPluginFactory"));
        require(getFactory != nullptr, "GetPluginFactory export");
        factory = getFactory();
        require(factory != nullptr, "Factory returned null");
        bool found = false;
        for (int32 i = 0; i < factory->countClasses(); ++i) {
            PClassInfo info{};
            ok(factory->getClassInfo(i, &info), "Read factory class");
            if (std::strcmp(info.category, kVstAudioEffectClass) == 0) {
                std::memcpy(processorId, info.cid, sizeof(TUID));
                found = true;
                break;
            }
        }
        require(found, "Factory must expose an audio-effect class");
    }
    ~Module() {
        if (factory) factory->release();
        if (exitDll) exitDll();
        if (handle) FreeLibrary(handle);
    }
};

struct Instance {
    IComponent* component{};
    IAudioProcessor* processor{};
    bool active = false;
    int32 sampleSize = kSample32;
    int channels = 2;

    explicit Instance(Module& module) {
        ok(module.factory->createInstance(module.processorId, IComponent::iid,
                                          reinterpret_cast<void**>(&component)), "Create component");
        ok(component->queryInterface(IAudioProcessor::iid, reinterpret_cast<void**>(&processor)),
           "Query audio processor");
        ok(component->initialize(nullptr), "Initialize component");
    }
    ~Instance() {
        if (active) {
            processor->setProcessing(false);
            component->setActive(false);
        }
        if (component) component->terminate();
        if (processor) processor->release();
        if (component) component->release();
    }
    void configure(int channelCount = 2, int32 precision = kSample32, int maximumFrames = 16384) {
        if (active) {
            processor->setProcessing(false);
            component->setActive(false);
            active = false;
        }
        channels = channelCount;
        sampleSize = precision;
        ok(processor->canProcessSampleSize(precision), "Support requested sample precision");
        SpeakerArrangement layout = channels == 1 ? SpeakerArr::kMono : SpeakerArr::kStereo;
        ok(processor->setBusArrangements(&layout, 1, &layout, 1), "Set mono/stereo arrangement");
        ProcessSetup setup{};
        setup.processMode = kRealtime;
        setup.symbolicSampleSize = precision;
        setup.maxSamplesPerBlock = maximumFrames;
        setup.sampleRate = 48000;
        ok(processor->setupProcessing(setup), "Setup processing");
        ok(component->activateBus(kAudio, kInput, 0, true), "Activate audio input");
        ok(component->activateBus(kAudio, kOutput, 0, true), "Activate audio output");
        ok(component->activateBus(kEvent, kInput, 0, true), "Activate MIDI input");
        ok(component->setActive(true), "Activate component");
        ok(processor->setProcessing(true), "Start processing notification");
        active = true;
    }
    void flush(IEventList* events = nullptr, IParameterChanges* changes = nullptr) {
        ProcessData data{};
        data.processMode = kRealtime;
        data.symbolicSampleSize = sampleSize;
        data.numSamples = 0;
        data.inputEvents = events;
        data.inputParameterChanges = changes;
        ok(processor->process(data), "Zero-frame event/parameter flush");
    }

    template <typename T>
    std::vector<std::vector<T>> run(std::vector<std::vector<T>> input,
            IEventList* events = nullptr, IParameterChanges* changes = nullptr,
            bool inPlace = false, uint64 inputSilence = 0, uint64* outputSilence = nullptr,
            bool aliasBus = false, IParameterChanges* feedback = nullptr) {
        require(static_cast<int>(input.size()) == channels, "Harness channel count");
        require(!aliasBus || inPlace, "Aliased bus requires in-place audio");
        int32 frames = static_cast<int32>(input[0].size());
        std::vector<std::vector<T>> output(channels, std::vector<T>(frames, T(12345)));
        std::vector<T*> inputPointers(channels), outputPointers(channels);
        for (int c = 0; c < channels; ++c) {
            require(input[c].size() == static_cast<size_t>(frames), "Harness frame count");
            inputPointers[c] = input[c].data();
            outputPointers[c] = inPlace ? input[c].data() : output[c].data();
        }
        AudioBusBuffers in{}, out{};
        in.numChannels = out.numChannels = channels;
        in.silenceFlags = inputSilence;
        out.silenceFlags = 0;
        if constexpr (std::is_same_v<T, float>) {
            in.channelBuffers32 = inputPointers.data();
            out.channelBuffers32 = outputPointers.data();
        } else {
            in.channelBuffers64 = inputPointers.data();
            out.channelBuffers64 = outputPointers.data();
        }
        ProcessData data{};
        data.processMode = kRealtime;
        data.symbolicSampleSize = sampleSize;
        data.numSamples = frames;
        data.numInputs = data.numOutputs = 1;
        data.inputs = &in;
        data.outputs = aliasBus ? &in : &out;
        data.inputEvents = events;
        data.inputParameterChanges = changes;
        data.outputParameterChanges = feedback;
        ok(processor->process(data), "Process audio block");
        if (outputSilence) *outputSilence = data.outputs->silenceFlags;
        return inPlace ? input : output;
    }
};

void note(EventList& events, int offset, int rawVelocity, int pitch = 60, int channel = 0,
          bool isOff = false) {
    Event event{};
    event.busIndex = 0;
    event.sampleOffset = offset;
    event.type = isOff ? Event::kNoteOffEvent : Event::kNoteOnEvent;
    if (isOff) {
        event.noteOff.channel = static_cast<int16>(channel);
        event.noteOff.pitch = static_cast<int16>(pitch);
        event.noteOff.velocity = static_cast<float>(rawVelocity) / 127.0f;
        event.noteOff.noteId = -1;
    } else {
        event.noteOn.channel = static_cast<int16>(channel);
        event.noteOn.pitch = static_cast<int16>(pitch);
        event.noteOn.velocity = static_cast<float>(rawVelocity) / 127.0f;
        event.noteOn.noteId = -1;
    }
    ok(events.addEvent(event), "Add test MIDI event");
}
void parameter(ParameterChanges& changes, ParamID id, int offset, double value) {
    int32 queueIndex = 0, pointIndex = 0;
    auto* queue = changes.addParameterData(id, queueIndex);
    require(queue != nullptr, "Create test parameter queue");
    ok(queue->addPoint(offset, value, pointIndex), "Add test parameter point");
}
template <typename T> std::vector<std::vector<T>> constant(int channels, int frames, T value = T(1)) {
    return std::vector<std::vector<T>>(channels, std::vector<T>(frames, value));
}
template <typename T> void allEqual(const std::vector<std::vector<T>>& audio, double value,
                                  const std::string& reason) {
    for (const auto& channel : audio) for (T sample : channel) close(sample, value, reason);
}

void testBuses(Module& module) {
    Instance plugin(module);
    const FUID expectedProcessor(0x9379BEB5, 0x0DEB46EB, 0xBDCBA6E2, 0xD023E53C);
    const FUID expectedController(0x47ABBF5B, 0xDCB241D9, 0x8CBFC576, 0x43F021D9);
    require(FUID::fromTUID(module.processorId) == expectedProcessor,
            "Inverse processor has its own stable plugin identity");
    require(plugin.component->getBusCount(kAudio, kInput) == 1, "One main audio input");
    require(plugin.component->getBusCount(kAudio, kOutput) == 1, "One main audio output");
    require(plugin.component->getBusCount(kEvent, kInput) == 1, "One event input");
    require(plugin.component->getBusCount(kEvent, kOutput) == 0, "No event output");
    BusInfo info{};
    ok(plugin.component->getBusInfo(kEvent, kInput, 0, info), "Read event bus");
    require(info.channelCount == 16, "Event bus accepts all 16 MIDI channels");
    require(plugin.processor->getLatencySamples() == 0, "Zero latency");
    require(plugin.processor->getTailSamples() == 0, "Zero tail");
    TUID controllerId{};
    ok(plugin.component->getControllerClassId(controllerId), "Controller class exists");
    require(FUID::fromTUID(controllerId) == expectedController,
            "Inverse controller has its own stable plugin identity");
    IEditController* controller = nullptr;
    ok(module.factory->createInstance(controllerId, IEditController::iid,
                                      reinterpret_cast<void**>(&controller)), "Instantiate controller");
    ok(controller->initialize(nullptr), "Initialize controller");
    require(controller->getParameterCount() == 2, "Only level and bypass parameters");
    bool level = false, bypass = false;
    for (int32 i = 0; i < controller->getParameterCount(); ++i) {
        ParameterInfo parameterInfo{};
        ok(controller->getParameterInfo(i, parameterInfo), "Read controller parameter");
        level |= parameterInfo.id == levelId;
        bypass |= parameterInfo.id == bypassId && (parameterInfo.flags & ParameterInfo::kIsBypass);
    }
    require(level && bypass, "Expected parameter IDs and bypass flag");
    close(controller->getParamNormalized(levelId), 1.0, "Unity controller default");
    controller->terminate();
    controller->release();
}

template <typename T> void testAudioFormat(Module& module, int channels, int32 precision) {
    Instance plugin(module);
    plugin.configure(channels, precision);
    std::vector<std::vector<T>> input(channels, std::vector<T>(257));
    for (int c = 0; c < channels; ++c)
        for (int i = 0; i < 257; ++i)
            input[c][i] = T(std::sin(i * .117 + c * .8) * .8);
    auto unity = plugin.run(input);
    require(unity == input, "Unity startup is exact passthrough");
    EventList events;
    note(events, 0, 64, 17, 15);
    auto half = plugin.run(input, &events);
    for (int c = 0; c < channels; ++c)
        for (int i = 0; i < 257; ++i)
            close(half[c][i], input[c][i] * .5, "Sine level at velocity 64");
    auto held = plugin.run(input, nullptr, nullptr, true);
    for (int c = 0; c < channels; ++c)
        for (int i = 0; i < 257; ++i)
            close(held[c][i], input[c][i] * .5, "No-MIDI hold and in-place processing");
}

void testSampleAccurateMidi(Module& module) {
    Instance plugin(module); plugin.configure();
    EventList events;
    note(events, 0, 1, 0, 15);
    note(events, 8, 64, 127, 3);
    note(events, 16, 127, 35, 9);
    note(events, 24, 100, 35, 9, true);
    note(events, 28, 0, 81, 4);
    auto output = plugin.run(constant<float>(2, 32), &events);
    for (const auto& channel : output) for (int i = 0; i < 32; ++i)
        close(channel[i], i < 8 ? 1 : i < 16 ? .5 : 0,
              "Sample-accurate inverse MIDI mapping; off/zero messages ignored");
    EventList boundary;
    note(boundary, 8, 0, 60, 0, true);
    note(boundary, 8, 64, 57, 0);
    auto next = plugin.run(constant<float>(2, 16), &boundary);
    for (const auto& channel : next) for (int i = 0; i < 16; ++i)
        close(channel[i], i < 8 ? 0 : .5, "Back-to-back off/on section boundary");
}

void testParametersAndMidiPriority(Module& module) {
    Instance plugin(module); plugin.configure();
    ParameterChanges changes;
    parameter(changes, levelId, 0, .25);
    parameter(changes, levelId, 8, .9);
    parameter(changes, levelId, 12, .75);
    EventList events;
    note(events, 8, 64);
    note(events, 16, 1);
    auto output = plugin.run(constant<float>(2, 24), &events, &changes);
    for (const auto& channel : output) for (int i = 0; i < 24; ++i)
        close(channel[i], i < 8 ? .25 : i < 12 ? .5 : i < 16 ? .75 : 1,
              "Parameters are sample-accurate and same-offset MIDI wins");
}

void testFlushAndBypass(Module& module) {
    Instance plugin(module); plugin.configure();
    EventList events;
    note(events, 0, 64);
    plugin.flush(&events);
    allEqual(plugin.run(constant<float>(2, 16)), .5, "Zero-frame MIDI flush updates held gain");
    ParameterChanges changes;
    parameter(changes, bypassId, 4, 1);
    parameter(changes, bypassId, 12, 0);
    EventList duringBypass;
    note(duringBypass, 8, 127);
    auto output = plugin.run(constant<float>(2, 16), &duringBypass, &changes);
    for (const auto& channel : output) for (int i = 0; i < 16; ++i)
        close(channel[i], i < 4 ? .5 : i < 12 ? 1 : 0,
              "Bypass passes input and still consumes MIDI");
}

void testBlockEndAndInvalidEvents(Module& module) {
    Instance plugin(module); plugin.configure();
    EventList endpoint;
    note(endpoint, 16, 64);
    allEqual(plugin.run(constant<float>(2, 16), &endpoint), 1,
             "An event at block end leaves current audio unchanged");
    allEqual(plugin.run(constant<float>(2, 16)), .5,
             "An event at block end sets the next block's gain");
    EventList invalid;
    note(invalid, -1, 127);
    note(invalid, 17, 1);
    allEqual(plugin.run(constant<float>(2, 16), &invalid), .5,
             "Out-of-range event offsets are ignored");
    ParameterChanges endpointParameter;
    parameter(endpointParameter, levelId, 16, .25);
    allEqual(plugin.run(constant<float>(2, 16), nullptr, &endpointParameter), .5,
             "A parameter at block end leaves current audio unchanged");
    allEqual(plugin.run(constant<float>(2, 16)), .25,
             "A parameter at block end sets the next block's gain");
}

void testSharedBusObject(Module& module) {
    Instance plugin(module); plugin.configure();
    allEqual(plugin.run(constant<float>(2, 32), nullptr, nullptr, true, 0, nullptr, true), 1,
             "Identical input/output AudioBusBuffers preserves exact unity");
    EventList events;
    note(events, 16, 64);
    auto output = plugin.run(constant<float>(2, 32), &events, nullptr, true, 0, nullptr, true);
    for (const auto& channel : output) for (int i = 0; i < 32; ++i)
        close(channel[i], i < 16 ? 1 : .5, "Aliased bus with sample-accurate gain change");
}

void testOutputFeedback(Module& module) {
    Instance plugin(module); plugin.configure();
    ParameterChanges feedback;
    auto readLevel = [&]() {
        require(feedback.getParameterCount() == 1, "One level feedback queue");
        auto* queue = feedback.getParameterData(0);
        require(queue && queue->getParameterId() == levelId, "Feedback is the level parameter");
        require(queue->getPointCount() >= 1, "Feedback contains a level value");
        int32 offset = -1;
        double value = -1;
        ok(queue->getPoint(queue->getPointCount() - 1, offset, value), "Read level feedback");
        require(offset >= 0 && offset < 32, "Feedback timestamp falls within the block");
        return value;
    };
    EventList half;
    note(half, 16, 64);
    plugin.run(constant<float>(2, 32), &half, nullptr, false, 0, nullptr, false, &feedback);
    close(readLevel(), .5, "MIDI updates the host's displayed level");
    feedback.clearQueue();
    EventList zero;
    note(zero, 0, 127);
    plugin.run(constant<float>(2, 32), &zero, nullptr, false, 0, nullptr, false, &feedback);
    close(readLevel(), 0, "Zero control level is reported");
    feedback.clearQueue();
    ParameterChanges reset;
    parameter(reset, levelId, 0, 1);
    allEqual(plugin.run(constant<float>(2, 32), nullptr, &reset, false, 0, nullptr, false, &feedback),
             1, "Host reset to level one restores pure passthrough");
    close(readLevel(), 1, "Host reset is reflected in output feedback");
}

void testEditorLifecycle(Module& module) {
    Instance plugin(module);
    TUID controllerId{};
    ok(plugin.component->getControllerClassId(controllerId), "Read editor controller ID");
    IEditController* controller = nullptr;
    ok(module.factory->createInstance(controllerId, IEditController::iid,
                                      reinterpret_cast<void**>(&controller)), "Create editor controller");
    ok(controller->initialize(nullptr), "Initialize editor controller");
    HWND parent = CreateWindowExW(0, L"STATIC", L"Velocity Pass Inverse integration test host",
                                  WS_OVERLAPPEDWINDOW, 0, 0, 600, 400, nullptr, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
    require(parent != nullptr, "Create hidden app-owned test window");
    IPlugView* view = nullptr;
    try {
        for (int create = 0; create < 3; ++create) {
            view = controller->createView(ViewType::kEditor);
            require(view != nullptr, "Create native editor");
            ok(view->isPlatformTypeSupported(kPlatformTypeHWND), "Editor supports native HWND");
            require(view->isPlatformTypeSupported("unsupported-platform") != kResultTrue,
                    "Editor rejects unsupported platform");
            for (int attach = 0; attach < 3; ++attach) {
                ViewRect size{};
                ok(view->getSize(&size), "Get editor size");
                require(size.getWidth() > 0 && size.getHeight() > 0, "Editor has a nonzero size");
                ok(view->attached(parent, kPlatformTypeHWND), "Attach editor to hidden test parent");
                HWND child = GetWindow(parent, GW_CHILD);
                require(child != nullptr && IsWindow(child), "Editor owns a native child window");
                require(!IsWindowVisible(parent), "Test host never appears on the desktop");
                ok(view->onSize(&size), "Apply editor size");
                ok(view->removed(), "Remove native editor");
                require(GetWindow(parent, GW_CHILD) == nullptr, "Removal destroys editor child window");
            }
            view->release();
            view = nullptr;
        }
        DestroyWindow(parent);
        controller->terminate();
        controller->release();
    } catch (...) {
        if (view) { view->removed(); view->release(); }
        DestroyWindow(parent);
        controller->terminate();
        controller->release();
        throw;
    }
}

void testLargeEventLists(Module& module) {
    Instance plugin(module); plugin.configure(2, kSample32, 16384);
    EventList events(20000);
    constexpr int count = 8192;
    for (int i = 0; i < count; ++i) {
        note(events, i, 0, i % 128, i % 16, true);
        note(events, i, i % 127 + 1, i % 128, i % 16);
    }
    auto output = plugin.run(constant<float>(2, count), &events);
    for (const auto& channel : output) for (int i = 0; i < count; ++i)
        close(channel[i], 1.0 - (i % 127) / 126.0, "Large event batch preserves every inverse step");
    EventList sameOffset(11000);
    for (int i = 0; i < 10000; ++i) note(sameOffset, 0, i % 127 + 1, i % 128, i % 16);
    note(sameOffset, 0, 64, 12, 8);
    allEqual(plugin.run(constant<float>(2, 4), &sameOffset), .5,
             "Last of 10,001 simultaneous note-ons wins without truncation");
}

void testSilenceFlags(Module& module) {
    Instance plugin(module); plugin.configure();
    uint64 silence = 0;
    allEqual(plugin.run(constant<float>(2, 32, 0), nullptr, nullptr, false, 3, &silence), 0,
             "Silent input remains silent");
    require((silence & 3) == 3, "Silent-input flags propagate");
    EventList mute;
    note(mute, 0, 127);
    allEqual(plugin.run(constant<float>(2, 32), &mute, nullptr, false, 0, &silence), 0,
             "Velocity 127 produces silence");
    require((silence & 3) == 3, "Known all-zero output is flagged silent");
    EventList recover;
    note(recover, 16, 1);
    auto output = plugin.run(constant<float>(2, 32), &recover, nullptr, false, 0, &silence);
    require((silence & 3) == 0, "Partially audible output is not flagged silent");
    for (const auto& channel : output) for (int i = 0; i < 32; ++i)
        close(channel[i], i < 16 ? 0 : 1, "Silence-to-audio transition");
}

void testState(Module& module) {
    Instance plugin(module); plugin.configure();
    EventList heldLevel;
    note(heldLevel, 0, 32);
    ParameterChanges enableBypass;
    parameter(enableBypass, bypassId, 0, 1);
    plugin.flush(&heldLevel, &enableBypass);
    MemoryStream state;
    ok(plugin.component->getState(&state), "Save versioned state");
    require(state.getSize() >= 4, "State includes version header");
    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(state.getData());
    require(bytes[0] == 1 && bytes[1] == 0 && bytes[2] == 0 && bytes[3] == 0, "State version is one");
    ParameterChanges change;
    parameter(change, levelId, 0, 1);
    parameter(change, bypassId, 0, 0);
    plugin.flush(nullptr, &change);
    state.seek(0, IBStream::kIBSeekSet, nullptr);
    ok(plugin.component->setState(&state), "Restore state");
    allEqual(plugin.run(constant<float>(2, 16)), 1, "Bypass is restored");
    ParameterChanges disableBypass;
    parameter(disableBypass, bypassId, 0, 0);
    plugin.flush(nullptr, &disableBypass);
    allEqual(plugin.run(constant<float>(2, 16)), 95.0 / 126.0,
             "Inverse MIDI-held gain is restored without a second inversion");

    MemoryStream empty;
    require(plugin.component->setState(&empty) != kResultOk, "Empty state is rejected");
    std::vector<char> invalid(state.getData(), state.getData() + state.getSize());
    invalid[0] = 99;
    MemoryStream invalidVersion(invalid.data(), static_cast<TSize>(invalid.size()));
    require(plugin.component->setState(&invalidVersion) != kResultOk, "Unknown state version is rejected");
    MemoryStream truncated(state.getData(), 5);
    require(plugin.component->setState(&truncated) != kResultOk, "Truncated state is rejected");
    allEqual(plugin.run(constant<float>(2, 16)), 95.0 / 126.0,
             "Invalid state leaves the current gain intact");
}

template <typename T>
void testComplementaryAudio(Module& inverseModule, Module& originalModule,
                            int channels, int32 precision) {
    require(std::memcmp(inverseModule.processorId, originalModule.processorId,
                        sizeof(TUID)) != 0,
            "Original and inverse have different processor IDs");
    Instance inverse(inverseModule), original(originalModule);
    inverse.configure(channels, precision);
    original.configure(channels, precision);
    TUID inverseController{}, originalController{};
    ok(inverse.component->getControllerClassId(inverseController), "Inverse controller ID");
    ok(original.component->getControllerClassId(originalController), "Original controller ID");
    require(std::memcmp(inverseController, originalController, sizeof(TUID)) != 0,
            "Original and inverse have different controller IDs");

    constexpr int stepFrames = 8, frames = 127 * stepFrames;
    std::vector<std::vector<T>> input(channels, std::vector<T>(frames));
    for (int c = 0; c < channels; ++c)
        for (int i = 0; i < frames; ++i)
            input[c][i] = T(std::sin(i * .117 + c * .8) * .8);
    EventList events(127);
    for (int velocity = 1; velocity <= 127; ++velocity)
        note(events, (velocity - 1) * stepFrames, velocity,
             velocity % 128, velocity % 16);
    auto inverseAudio = inverse.run(input, &events);
    auto originalAudio = original.run(input, &events);
    const double tolerance = std::is_same_v<T, float> ? 2e-7 : 1e-12;
    for (int c = 0; c < channels; ++c) {
        for (int i = 0; i < frames; ++i) {
            close(double(inverseAudio[c][i]) + double(originalAudio[c][i]),
                  input[c][i], "Original plus inverse reconstructs the input at every velocity",
                  tolerance);
            if (i < stepFrames) {
                require(inverseAudio[c][i] == input[c][i],
                        "Velocity one is exact inverse passthrough");
                require(originalAudio[c][i] == T(0), "Velocity one silences original");
            }
            if (i >= frames - stepFrames) {
                require(inverseAudio[c][i] == T(0), "Velocity 127 silences inverse");
                require(originalAudio[c][i] == input[c][i],
                        "Velocity 127 is exact original passthrough");
            }
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2 && argc != 3) {
        std::cerr << "Usage: plugin_tests.exe <VelocityPassInverse.vst3 binary> [original VelocityPass.vst3 binary]\n";
        return 2;
    }
    int passed = 0, failed = 0;
    try {
        Module module{std::filesystem::path(argv[1])};
        auto test = [&](const char* name, const std::function<void()>& body) {
            try { body(); ++passed; std::cout << "PASS " << name << '\n'; }
            catch (const std::exception& error) {
                ++failed; std::cout << "FAIL " << name << ": " << error.what() << '\n';
            }
        };
        test("factory, buses, controls, zero latency/tail", [&] { testBuses(module); });
        test("mono 32-bit exact startup, sine, hold, in-place", [&] { testAudioFormat<float>(module, 1, kSample32); });
        test("stereo 32-bit exact startup, sine, hold, in-place", [&] { testAudioFormat<float>(module, 2, kSample32); });
        test("mono 64-bit exact startup, sine, hold, in-place", [&] { testAudioFormat<double>(module, 1, kSample64); });
        test("stereo 64-bit exact startup, sine, hold, in-place", [&] { testAudioFormat<double>(module, 2, kSample64); });
        test("MIDI mapping, note-off, zero velocity, section boundaries", [&] { testSampleAccurateMidi(module); });
        test("parameter timing and same-offset MIDI priority", [&] { testParametersAndMidiPriority(module); });
        test("zero-frame flush and sample-accurate bypass", [&] { testFlushAndBypass(module); });
        test("block-end events and invalid offsets", [&] { testBlockEndAndInvalidEvents(module); });
        test("identical input/output bus object", [&] { testSharedBusObject(module); });
        test("level feedback and host reset", [&] { testOutputFeedback(module); });
        test("hidden native editor repeated attach/remove lifecycle", [&] { testEditorLifecycle(module); });
        test("large event batches and all pitches/channels/velocities", [&] { testLargeEventLists(module); });
        test("silence flags", [&] { testSilenceFlags(module); });
        test("versioned state roundtrip and malformed state", [&] { testState(module); });
        if (argc == 3) {
            Module original{std::filesystem::path(argv[2])};
            test("original + inverse, all velocities, mono 32-bit", [&] {
                testComplementaryAudio<float>(module, original, 1, kSample32); });
            test("original + inverse, all velocities, stereo 32-bit", [&] {
                testComplementaryAudio<float>(module, original, 2, kSample32); });
            test("original + inverse, all velocities, mono 64-bit", [&] {
                testComplementaryAudio<double>(module, original, 1, kSample64); });
            test("original + inverse, all velocities, stereo 64-bit", [&] {
                testComplementaryAudio<double>(module, original, 2, kSample64); });
        }
    } catch (const std::exception& error) {
        ++failed; std::cout << "FAIL load/setup: " << error.what() << '\n';
    }
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
