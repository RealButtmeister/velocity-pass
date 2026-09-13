#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include "public.sdk/source/vst/vstaudioeffect.h"
#include "public.sdk/source/vst/vsteditcontroller.h"
#include "public.sdk/source/main/pluginfactory.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "base/source/fstreamer.h"

using namespace Steinberg;
using namespace Steinberg::Vst;
namespace {
const FUID processorId(0x33BDA9E7,0x8435421B,0x8E7AC569,0x287D0913);
const FUID controllerId(0x742ECA1D,0x4BF643A8,0xB728E156,0x0CF8DA29);
constexpr ParamID kLevel=0, kBypass=1;
constexpr int32 never=std::numeric_limits<int32>::max();
const wchar_t* editorClass=L"EightBar_VelocityPass_33BDA9E7";
char moduleAddress;

HINSTANCE moduleHandle() {
    HMODULE module=nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                      reinterpret_cast<LPCWSTR>(&moduleAddress),&module);
    return module;
}

bool readState(IBStream* stream,double& level,bool& bypass) {
    if(!stream)return false;
    IBStreamer reader(stream,kLittleEndian);int32 version=0,flag=0;double value=0;
    if(!reader.readInt32(version)||version!=1||!reader.readDouble(value)||!reader.readInt32(flag)
       ||!std::isfinite(value)||value<0||value>1||(flag!=0&&flag!=1))return false;
    level=value;bypass=flag!=0;return true;
}

double decodeVelocity(double velocity) {
    double midi=velocity*127.;
    // Restore exact 7-bit endpoints lost in VST3 float conversion. Values
    // genuinely between MIDI steps retain their higher-resolution value.
    const double nearest=std::round(midi);
    if(std::abs(midi-nearest)<.00001)midi=nearest;
    return std::clamp((midi-1.)/126.,0.,1.);
}

struct ParameterCursor {
    IParamValueQueue* queue=nullptr;
    int32 index=0,count=0,offset=never,frames=0;
    double value=0;
    void start(IParamValueQueue* q,int32 n) {queue=q;frames=n;index=0;count=q?q->getPointCount():0;next();}
    void next() {
        offset=never;
        while(queue&&index<count) {
            int32 time=0;double v=0;
            if(queue->getPoint(index++,time,v)!=kResultTrue||!std::isfinite(v)||time<0||time>frames)continue;
            offset=time;value=std::clamp(v,0.,1.);return;
        }
    }
};

struct MidiCursor {
    IEventList* events=nullptr;
    int32 index=0,count=0,offset=never,frames=0;
    double value=1;
    void start(IEventList* list,int32 n) {events=list;frames=n;index=0;count=list?list->getEventCount():0;next();}
    void next() {
        offset=never;
        while(events&&index<count) {
            Event event{};
            if(events->getEvent(index++,event)!=kResultTrue||event.busIndex!=0||event.type!=Event::kNoteOnEvent
               ||!std::isfinite(event.noteOn.velocity)||event.noteOn.velocity<=0
               ||event.sampleOffset<0||event.sampleOffset>frames)continue;
            offset=event.sampleOffset;value=decodeVelocity(event.noteOn.velocity);return;
        }
    }
};

class Processor final:public AudioEffect {
public:
    Processor() {setControllerClass(controllerId);}
    static FUnknown* create(void*) {return static_cast<IAudioProcessor*>(new Processor);}
    tresult PLUGIN_API initialize(FUnknown* context) override {
        const auto result=AudioEffect::initialize(context);if(result!=kResultOk)return result;
        addAudioInput(STR16("Input"),SpeakerArr::kStereo);
        addAudioOutput(STR16("Output"),SpeakerArr::kStereo);
        addEventInput(STR16("Velocity control"),16);
        return kResultOk;
    }
    tresult PLUGIN_API setBusArrangements(SpeakerArrangement* in,int32 ni,SpeakerArrangement* out,int32 no) override {
        if(!in||!out||ni!=1||no!=1||in[0]!=out[0]
           ||(in[0]!=SpeakerArr::kMono&&in[0]!=SpeakerArr::kStereo))return kResultFalse;
        return AudioEffect::setBusArrangements(in,ni,out,no);
    }
    tresult PLUGIN_API canProcessSampleSize(int32 size) override {
        return size==kSample32||size==kSample64?kResultTrue:kResultFalse;
    }
    tresult PLUGIN_API setupProcessing(ProcessSetup& setup) override {
        if(!std::isfinite(setup.sampleRate)||setup.sampleRate<=0||setup.maxSamplesPerBlock<0
           ||canProcessSampleSize(setup.symbolicSampleSize)!=kResultTrue)return kResultFalse;
        return AudioEffect::setupProcessing(setup);
    }
    uint32 PLUGIN_API getLatencySamples() override {return 0;}
    uint32 PLUGIN_API getTailSamples() override {return 0;}
    tresult PLUGIN_API setProcessing(TBool) override {return kResultOk;}
    tresult PLUGIN_API process(ProcessData& data) override {
        if(data.numSamples<0||canProcessSampleSize(data.symbolicSampleSize)!=kResultTrue)return kResultFalse;
        IParamValueQueue *levelQueue=nullptr,*bypassQueue=nullptr;
        if(data.inputParameterChanges)for(int32 i=0;i<data.inputParameterChanges->getParameterCount();++i) {
            auto* queue=data.inputParameterChanges->getParameterData(i);if(!queue)continue;
            if(queue->getParameterId()==kLevel)levelQueue=queue;
            if(queue->getParameterId()==kBypass)bypassQueue=queue;
        }
        ParameterCursor level,bypass;MidiCursor midi;
        level.start(levelQueue,data.numSamples);bypass.start(bypassQueue,data.numSamples);midi.start(data.inputEvents,data.numSamples);
        double gain=heldLevel.load(std::memory_order_relaxed);
        bool bypassed=heldBypass.load(std::memory_order_relaxed);
        int32 cursor=0;
        const uint64 inputSilence=data.numInputs>0&&data.inputs?data.inputs[0].silenceFlags:0;
        AudioBusBuffers* output=data.numOutputs>0&&data.outputs?&data.outputs[0]:nullptr;
        if(output)output->silenceFlags=output->numChannels>=64?~uint64(0):((uint64(1)<<std::max(0,output->numChannels))-1);
        while(true) {
            const int32 boundary=std::max(cursor,std::min({level.offset,bypass.offset,midi.offset,data.numSamples}));
            if(boundary>cursor) {
                if(data.symbolicSampleSize==kSample32)render<float>(data,cursor,boundary,gain,bypassed,inputSilence);
                else render<double>(data,cursor,boundary,gain,bypassed,inputSilence);
                cursor=boundary;
            }
            // Host level edits apply first; a simultaneous MIDI note wins.
            while(level.offset<=cursor) {gain=level.value;level.next();}
            while(bypass.offset<=cursor) {bypassed=bypass.value>=.5;bypass.next();}
            while(midi.offset<=cursor) {gain=midi.value;midi.next();}
            if(cursor>=data.numSamples)break;
        }
        heldLevel.store(gain,std::memory_order_relaxed);heldBypass.store(bypassed,std::memory_order_relaxed);
        // Standard VST3 output parameter feedback keeps the host/controller
        // display informed. There are no allocations or UI calls in process.
        if(data.outputParameterChanges&&gain!=lastReported) {
            int32 index=0;
            if(auto* queue=data.outputParameterChanges->addParameterData(kLevel,index)) {
                int32 point=0;queue->addPoint(std::max(0,data.numSamples-1),gain,point);lastReported=gain;
            }
        }
        return kResultOk;
    }
    tresult PLUGIN_API getState(IBStream* stream) override {
        if(!stream)return kInvalidArgument;
        IBStreamer writer(stream,kLittleEndian);
        return writer.writeInt32(1)&&writer.writeDouble(heldLevel.load(std::memory_order_relaxed))
            &&writer.writeInt32(heldBypass.load(std::memory_order_relaxed)?1:0)?kResultOk:kResultFalse;
    }
    tresult PLUGIN_API setState(IBStream* stream) override {
        double gain;bool bypass;
        if(!readState(stream,gain,bypass))return kResultFalse;
        heldLevel.store(gain,std::memory_order_relaxed);heldBypass.store(bypass,std::memory_order_relaxed);
        return kResultOk;
    }
private:
    std::atomic<double> heldLevel{1.};std::atomic<bool> heldBypass{false};double lastReported=-1;
    template<class Sample>void render(ProcessData& data,int32 start,int32 end,double gain,bool bypass,uint64 inputSilence) {
        if(data.numOutputs<1||!data.outputs)return;
        auto& output=data.outputs[0];
        auto* input=data.numInputs>0&&data.inputs?&data.inputs[0]:nullptr;
        Sample** out=nullptr;Sample** in=nullptr;
        if constexpr(sizeof(Sample)==sizeof(float)) {out=output.channelBuffers32;if(input)in=input->channelBuffers32;}
        else {out=output.channelBuffers64;if(input)in=input->channelBuffers64;}
        if(!out)return;
        const Sample multiplier=static_cast<Sample>(bypass?1.:gain);
        for(int32 channel=0;channel<output.numChannels;++channel) {
            if(!out[channel])continue;
            const bool sourceSilent=!input||!in||channel>=input->numChannels||!in[channel]
                ||(channel<64&&(inputSilence&(uint64(1)<<channel)));
            bool silent=true;
            for(int32 i=start;i<end;++i) {
                // Direct copy at unity is bit-transparent, including in-place.
                const Sample value=sourceSilent||multiplier==0?Sample(0):multiplier==1?in[channel][i]:in[channel][i]*multiplier;
                out[channel][i]=value;silent=silent&&value==0;
            }
            if(!silent&&channel<64)output.silenceFlags&=~(uint64(1)<<channel);
        }
    }
};

class LevelView final:public EditorView {
public:
    explicit LevelView(EditController* controller):EditorView(controller) {rect={0,0,420,250};}
    ~LevelView() override {destroy();}
    tresult PLUGIN_API isPlatformTypeSupported(FIDString type) override {
        return type&&std::strcmp(type,kPlatformTypeHWND)==0?kResultTrue:kResultFalse;
    }
    tresult PLUGIN_API attached(void* parent,FIDString type) override {
        if(!parent||isPlatformTypeSupported(type)!=kResultTrue||window)return kResultFalse;
        WNDCLASSEXW wc{};wc.cbSize=sizeof(wc);wc.hInstance=moduleHandle();wc.lpfnWndProc=procedure;
        wc.lpszClassName=editorClass;wc.hCursor=LoadCursor(nullptr,IDC_ARROW);
        if(!RegisterClassExW(&wc)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return kResultFalse;
        window=CreateWindowExW(0,editorClass,L"Velocity Pass",WS_CHILD|WS_VISIBLE|WS_CLIPSIBLINGS,
            0,0,420,250,static_cast<HWND>(parent),nullptr,moduleHandle(),this);
        if(!window)return kResultFalse;
        SetTimer(window,1,50,nullptr);
        return EditorView::attached(parent,type);
    }
    tresult PLUGIN_API removed() override {destroy();return EditorView::removed();}
    tresult PLUGIN_API onSize(ViewRect* size) override {
        if(!size)return kInvalidArgument;
        auto result=EditorView::onSize(size);
        if(window)MoveWindow(window,0,0,size->getWidth(),size->getHeight(),TRUE);
        return result;
    }
private:
    HWND window=nullptr;double shown=-1;bool shownBypass=false;
    void destroy() {if(window) {KillTimer(window,1);DestroyWindow(window);window=nullptr;}}
    void reset() {
        auto* controller=getController();controller->beginEdit(kLevel);
        controller->setParamNormalized(kLevel,1.);controller->performEdit(kLevel,1.);controller->endEdit(kLevel);
        InvalidateRect(window,nullptr,FALSE);
    }
    void paint(HDC dc) {
        RECT all;GetClientRect(window,&all);
        HBRUSH bg=CreateSolidBrush(RGB(17,24,32));FillRect(dc,&all,bg);DeleteObject(bg);
        SetBkMode(dc,TRANSPARENT);
        auto text=[&](const wchar_t* string,int x,int y,int size,COLORREF color,int weight=FW_NORMAL) {
            HFONT font=CreateFontW(-size,0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,
                CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
            auto old=SelectObject(dc,font);SetTextColor(dc,color);TextOutW(dc,x,y,string,lstrlenW(string));SelectObject(dc,old);DeleteObject(font);
        };
        const double level=std::clamp(getController()->getParamNormalized(kLevel),0.,1.);
        const bool bypass=getController()->getParamNormalized(kBypass)>=.5;
        text(L"VELOCITY PASS",28,22,17,RGB(232,240,245),FW_SEMIBOLD);
        text(L"Pass-through level",29,57,13,RGB(151,171,188));
        wchar_t amount[32];swprintf(amount,32,L"%.1f%%",level*100);
        text(amount,24,78,60,RGB(139,224,166),FW_SEMIBOLD);
        RECT track{30,151,390,163};HBRUSH base=CreateSolidBrush(RGB(42,59,73));FillRect(dc,&track,base);DeleteObject(base);
        RECT fill=track;fill.right=fill.left+static_cast<LONG>(360*level);
        HBRUSH accent=CreateSolidBrush(RGB(139,224,166));FillRect(dc,&fill,accent);DeleteObject(accent);
        text(bypass?L"Bypassed in host":L"Velocity sets level. Note-offs hold.",29,180,12,RGB(151,171,188));
        RECT button{266,208,391,237};HBRUSH buttonBg=CreateSolidBrush(RGB(42,59,73));FillRect(dc,&button,buttonBg);DeleteObject(buttonBg);
        text(L"Reset to 100%",282,213,12,RGB(232,240,245));
    }
    static LRESULT CALLBACK procedure(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam) {
        auto* self=reinterpret_cast<LevelView*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if(message==WM_NCCREATE) {
            self=static_cast<LevelView*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            self->window=hwnd;
            SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
        }
        if(!self)return DefWindowProcW(hwnd,message,wparam,lparam);
        if(message==WM_ERASEBKGND)return 1;
        if(message==WM_PAINT) {PAINTSTRUCT ps;HDC dc=BeginPaint(hwnd,&ps);self->paint(dc);EndPaint(hwnd,&ps);return 0;}
        if(message==WM_TIMER) {
            const double level=self->getController()->getParamNormalized(kLevel);
            const bool bypass=self->getController()->getParamNormalized(kBypass)>=.5;
            if(level!=self->shown||bypass!=self->shownBypass) {
                self->shown=level;self->shownBypass=bypass;InvalidateRect(hwnd,nullptr,FALSE);
            }
            return 0;
        }
        if(message==WM_LBUTTONUP) {
            const int x=GET_X_LPARAM(lparam),y=GET_Y_LPARAM(lparam);
            if(x>=266&&x<=391&&y>=208&&y<=237)self->reset();return 0;
        }
        if(message==WM_NCDESTROY) {SetWindowLongPtrW(hwnd,GWLP_USERDATA,0);self->window=nullptr;}
        return DefWindowProcW(hwnd,message,wparam,lparam);
    }
};

class Controller final:public EditController {
public:
    static FUnknown* create(void*) {return static_cast<IEditController*>(new Controller);}
    tresult PLUGIN_API initialize(FUnknown* context) override {
        const auto result=EditController::initialize(context);if(result!=kResultOk)return result;
        auto* level=new RangeParameter(STR16("Level"),kLevel,STR16("%"),0,100,100,0,ParameterInfo::kCanAutomate);
        level->setPrecision(1);parameters.addParameter(level);
        parameters.addParameter(new RangeParameter(STR16("Bypass"),kBypass,STR16(""),0,1,0,1,
            ParameterInfo::kCanAutomate|ParameterInfo::kIsBypass));
        return kResultOk;
    }
    tresult PLUGIN_API setComponentState(IBStream* stream) override {
        double level;bool bypass;if(!readState(stream,level,bypass))return kResultFalse;
        setParamNormalized(kLevel,level);setParamNormalized(kBypass,bypass?1.:0.);return kResultOk;
    }
    IPlugView* PLUGIN_API createView(FIDString name) override {
        return name&&std::strcmp(name,ViewType::kEditor)==0?new LevelView(this):nullptr;
    }
};
}

bool InitModule() {return true;}
bool DeinitModule() {UnregisterClassW(editorClass,moduleHandle());return true;}
BEGIN_FACTORY_DEF("Eight Bar Audio","","")
DEF_CLASS2(INLINE_UID_FROM_FUID(processorId),PClassInfo::kManyInstances,kVstAudioEffectClass,
    "Velocity Pass",Vst::kDistributable,"Fx|Tools","1.0.0",kVstVersionString,Processor::create)
DEF_CLASS2(INLINE_UID_FROM_FUID(controllerId),PClassInfo::kManyInstances,kVstComponentControllerClass,
    "Velocity Pass Controller",0,"","1.0.0",kVstVersionString,Controller::create)
END_FACTORY
