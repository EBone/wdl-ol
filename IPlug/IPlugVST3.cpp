// disable unknown pragmas warning MSVC
#pragma warning (disable : 4068 )

#include "IPlugVST3.h"
#include <cstdio>
#include "pluginterfaces/base/ustring.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstevents.h"

using namespace Steinberg;
using namespace Vst;

#include "IParam.h"

class IPlugVST3Parameter : public Parameter
{
public:
  IPlugVST3Parameter (IParam* pParam, ParamID tag, UnitID unitID)
  : mIPlugParam(pParam)
  {
    UString (info.title, str16BufferSize (String128)).assign (pParam->GetNameForHost());
    UString (info.units, str16BufferSize (String128)).assign (pParam->GetLabelForHost());
    
    precision = pParam->GetPrecision();
    
    if (pParam->Type() != IParam::kTypeDouble)
      info.stepCount = pParam->GetRange();
    else
      info.stepCount = 0; // continuous
    
    int32 flags = 0;

    if (pParam->GetCanAutomate())
    {
      flags |= ParameterInfo::kCanAutomate;
    }
    
    info.defaultNormalizedValue = valueNormalized = pParam->GetDefaultNormalized();
    info.flags = flags;
    info.id = tag;
    info.unitId = unitID;
  }
  
  virtual void toString (ParamValue valueNormalized, String128 string) const override
  {
    char disp[MAX_PARAM_DISPLAY_LEN];
    mIPlugParam->GetDisplayForHost(valueNormalized, true, disp);
    Steinberg::UString(string, 128).fromAscii(disp);
  }
  
  virtual bool fromString (const TChar* string, ParamValue& valueNormalized) const override
  {
    String str ((TChar*)string);
    valueNormalized = mIPlugParam->GetNormalized(atof(str.text8()));
    
    return true;
  }
  
  virtual Steinberg::Vst::ParamValue toPlain (ParamValue valueNormalized) const override
  {
    return mIPlugParam->GetNonNormalized(valueNormalized);
  }
  
  virtual Steinberg::Vst::ParamValue toNormalized (ParamValue plainValue) const override
  {
    return mIPlugParam->GetNormalized(valueNormalized);
  }
  
  OBJ_METHODS (IPlugVST3Parameter, Parameter)
  
protected:
  IParam* mIPlugParam;
};

IPlugVST3::IPlugVST3(IPlugInstanceInfo instanceInfo, IPlugConfig c)
: IPLUG_BASE_CLASS(c, kAPIVST3)
, mScChans(c.plugScChans)
{
  SetInputChannelConnections(0, NInChannels(), true);
  SetOutputChannelConnections(0, NOutChannels(), true);
  
  if (NInChannels()) 
  {
    mLatencyDelay = new NChanDelayLine<double>(NInChannels(), NOutChannels());
    mLatencyDelay->SetDelayTime(c.latency);
  }

  // initialize the bus labels
  SetInputBusLabel(0, "Main Input");

  if (mScChans)
  {
    SetInputBusLabel(1, "Aux Input");
  }

  if (IsInst())
  {
    int busNum = 0;
    char label[32]; //TODO: 32!!!

    for (int i = 0; i < NOutChannels(); i+=2) // stereo buses only
    {
      sprintf(label, "Output %i", busNum+1);
      SetOutputBusLabel(busNum++, label);
    }
  }
  else
  {
    SetOutputBusLabel(0, "Output");
  }
    
  // Make sure the process context is predictably initialised in case it is used before process is called
 
  memset(&mProcessContext, 0, sizeof(ProcessContext));
}

IPlugVST3::~IPlugVST3() {}

#pragma mark -
#pragma mark AudioEffect overrides

tresult PLUGIN_API IPlugVST3::initialize (FUnknown* context)
{
  TRACE;

  tresult result = SingleComponentEffect::initialize(context);

  String128 tmpStringBuf;
  char hostNameCString[128];
  FUnknownPtr<IHostApplication>app(context);

  if (app)
  {
    app->getName(tmpStringBuf);
    Steinberg::UString(tmpStringBuf, 128).toAscii(hostNameCString, 128);
    SetHost(hostNameCString, 0); // Can't get version in VST3
  }

  if (result == kResultOk)
  {
    SpeakerArrangement maxInputs = getSpeakerArrForChans(NInChannels()-mScChans);
    if(maxInputs < 0) maxInputs = 0;

    // add io buses with the maximum i/o to start with

    if (maxInputs)
    {
      Steinberg::UString(tmpStringBuf, 128).fromAscii(GetInputBusLabel(0)->Get(), 128);
      addAudioInput(tmpStringBuf, maxInputs);
    }

    if(!mIsInst) // if effect, just add one output bus with max chan count
    {
      Steinberg::UString(tmpStringBuf, 128).fromAscii(GetOutputBusLabel(0)->Get(), 128);
      addAudioOutput(tmpStringBuf, getSpeakerArrForChans(NOutChannels()) );
    }
    else
    {
      for (int i = 0, busIdx = 0; i < NOutChannels(); i+=2, busIdx++)
      {
        Steinberg::UString(tmpStringBuf, 128).fromAscii(GetOutputBusLabel(busIdx)->Get(), 128);
        addAudioOutput(tmpStringBuf, SpeakerArr::kStereo );
      }
    }

    if (mScChans)
    {
      if (mScChans > 2) mScChans = 2;
      Steinberg::UString(tmpStringBuf, 128).fromAscii(GetInputBusLabel(1)->Get(), 128);
      addAudioInput(tmpStringBuf, getSpeakerArrForChans(mScChans), kAux, 0);
    }

    if(DoesMIDI())
    {
      addEventInput (STR16("MIDI Input"), 1);
      //addEventOutput(STR16("MIDI Output"), 1);
    }

    if (NPresets())
    {
      parameters.addParameter(new Parameter(STR16("Preset"),
                                            kPresetParam,
                                            STR16(""),
                                            0,
                                            NPresets(),
                                            ParameterInfo::kIsProgramChange));
    }

    if(!mIsInst)
    {
      StringListParameter * bypass = new StringListParameter(STR16("Bypass"),
                                                            kBypassParam,
                                                            0,
                                                            ParameterInfo::kCanAutomate | ParameterInfo::kIsBypass | ParameterInfo::kIsList);
      bypass->appendString(STR16("off"));
      bypass->appendString(STR16("on"));
      parameters.addParameter(bypass);
    }

    for (int i=0; i<NParams(); i++)
    {
      IParam *p = GetParam(i);

      UnitID unitID = kRootUnitId;
      
      const char* paramGroupName = p->GetParamGroupForHost();

      if (CSTR_NOT_EMPTY(paramGroupName))
      {        
        for(int j = 0; j < mParamGroups.GetSize(); j++)
        {
          if(strcmp(paramGroupName, mParamGroups.Get(j)) == 0)
          {
            unitID = j+1;
          }
        }
        
        if (unitID == kRootUnitId) // new unit, nothing found, so add it
        {
          mParamGroups.Add(paramGroupName);
          unitID = mParamGroups.GetSize();
        }
      }
      
      Parameter* param = new IPlugVST3Parameter(p, i, unitID);
      parameters.addParameter(param);
    }
  }

  OnHostIdentified();
  RestorePreset(0);
  
  return result;
}

tresult PLUGIN_API IPlugVST3::terminate ()
{
  TRACE;

  mViews.empty();
  return SingleComponentEffect::terminate();
}

tresult PLUGIN_API IPlugVST3::setBusArrangements(SpeakerArrangement* inputs, int32 numIns, SpeakerArrangement* outputs, int32 numOuts)
{
  TRACE;

  // disconnect all io pins, they will be reconnected in process
  SetInputChannelConnections(0, NInChannels(), false);
  SetOutputChannelConnections(0, NOutChannels(), false);

  int32 reqNumInputChannels = SpeakerArr::getChannelCount(inputs[0]);  //requested # input channels
  int32 reqNumOutputChannels = SpeakerArr::getChannelCount(outputs[0]);//requested # output channels

  // legal io doesn't consider sidechain inputs
  if (!LegalIO(reqNumInputChannels, reqNumOutputChannels))
  {
    return kResultFalse;
  }

  // handle input
  AudioBus* bus = FCast<AudioBus>(audioInputs.at(0));

  // if existing input bus has a different number of channels to the input bus being connected
  if (bus && SpeakerArr::getChannelCount(bus->getArrangement()) != reqNumInputChannels)
  {
    audioInputs.erase(std::remove(audioInputs.begin(), audioInputs.end(), bus));
    addAudioInput(USTRING("Input"), getSpeakerArrForChans(reqNumInputChannels));
  }

  // handle output
  bus = FCast<AudioBus>(audioOutputs.at(0));
  // if existing output bus has a different number of channels to the output bus being connected
  if (bus && SpeakerArr::getChannelCount(bus->getArrangement()) != reqNumOutputChannels)
  {
    audioOutputs.erase(std::remove(audioOutputs.begin(), audioOutputs.end(), bus));
    addAudioOutput(USTRING("Output"), getSpeakerArrForChans(reqNumOutputChannels));
  }

  if (!mScChans && numIns == 1) // No sidechain, every thing OK
  {
    return kResultTrue;
  }

  if (mScChans && numIns == 2) // numIns = num Input BUSes
  {
    int32 reqNumSideChainChannels = SpeakerArr::getChannelCount(inputs[1]);  //requested # sidechain input channels

    bus = FCast<AudioBus>(audioInputs.at(1));

    if (bus && SpeakerArr::getChannelCount(bus->getArrangement()) != reqNumSideChainChannels)
    {
      audioInputs.erase(std::remove(audioInputs.begin(), audioInputs.end(), bus));
      addAudioInput(USTRING("Sidechain Input"), getSpeakerArrForChans(reqNumSideChainChannels), kAux, 0); // either mono or stereo
    }

    return kResultTrue;
  }

  return kResultFalse;
}

tresult PLUGIN_API IPlugVST3::setActive(TBool state)
{
  TRACE;

  OnActivate((bool) state);

  return SingleComponentEffect::setActive(state);
}

tresult PLUGIN_API IPlugVST3::setupProcessing (ProcessSetup& newSetup)
{
  TRACE;

  if ((newSetup.symbolicSampleSize != kSample32) && (newSetup.symbolicSampleSize != kSample64)) return kResultFalse;

  mSampleRate = newSetup.sampleRate;
  mIsBypassed = false;
  IPlugBase::SetBlockSize(newSetup.maxSamplesPerBlock);
  Reset();

  processSetup = newSetup;

  return kResultOk;
}

tresult PLUGIN_API IPlugVST3::process(ProcessData& data)
{
  TRACE_PROCESS;

  if(data.processContext)
    memcpy(&mProcessContext, data.processContext, sizeof(ProcessContext));

  //process parameters
  IParameterChanges* paramChanges = data.inputParameterChanges;
  if (paramChanges)
  {
    int32 numParamsChanged = paramChanges->getParameterCount();

    //it is possible to get a finer resolution of control here by retrieving more values (points) from the queue
    //for now we just grab the last one

    for (int32 i = 0; i < numParamsChanged; i++)
    {
      IParamValueQueue* paramQueue = paramChanges->getParameterData(i);
      if (paramQueue)
      {
        int32 numPoints = paramQueue->getPointCount();
        int32 offsetSamples;
        double value;

        if (paramQueue->getPoint(numPoints - 1,  offsetSamples, value) == kResultTrue)
        {
          int idx = paramQueue->getParameterId();

          switch (idx)
          {
            case kBypassParam:
            {
              bool bypassed = (value > 0.5);
              
              if (bypassed != mIsBypassed)
              {
                mIsBypassed = bypassed;
              }

              break;
            }
            case kPresetParam:
              RestorePreset(FromNormalizedParam(value, 0, NPresets(), 1.));
              break;
              //TODO: pitch bend, modwheel etc
            default:
              {
                WDL_MutexLock lock(&mParams_mutex);
                if (idx >= 0 && idx < NParams())
                {
                  GetParam(idx)->SetNormalized((double)value);
                  SetParameterInUIFromAPI(idx, (double) value, true);
                  OnParamChange(idx);
                }
              }
              break;
          }

        }
      }
    }
  }

  if(DoesMIDI())
  {
    //process events.. only midi note on and note off?
    IEventList* eventList = data.inputEvents;
    if (eventList)
    {
      int32 numEvent = eventList->getEventCount();
      for (int32 i=0; i<numEvent; i++)
      {
        Event event;
        if (eventList->getEvent(i, event) == kResultOk)
        {
          IMidiMsg msg;
          switch (event.type)
          {
            case Event::kNoteOnEvent:
            {
              msg.MakeNoteOnMsg(event.noteOn.pitch, event.noteOn.velocity * 127, event.sampleOffset, event.noteOn.channel);
              ProcessMidiMsg(msg);
              break;
            }

            case Event::kNoteOffEvent:
            {
              msg.MakeNoteOffMsg(event.noteOff.pitch, event.sampleOffset, event.noteOff.channel);
              ProcessMidiMsg(msg);
              break;
            }
          }
        }
      }
    }
  }

#pragma mark process single precision

  if (processSetup.symbolicSampleSize == kSample32)
  {
    if (data.numInputs)
    {
      if (mScChans)
      {
        if (getAudioInput(1)->isActive()) // Sidechain is active
        {
          mSidechainActive = true;
          SetInputChannelConnections(0, NInChannels(), true);
        }
        else
        {
          if (mSidechainActive)
          {
            ZeroScratchBuffers();
            mSidechainActive = false;
          }

          SetInputChannelConnections(0, NInChannels(), true);
          SetInputChannelConnections(data.inputs[0].numChannels, NInChannels() - mScChans, false);
        }

        AttachInputBuffers(0, NInChannels() - mScChans, data.inputs[0].channelBuffers32, data.numSamples);
        AttachInputBuffers(mScChans, NInChannels() - mScChans, data.inputs[1].channelBuffers32, data.numSamples);
      }
      else
      {
        SetInputChannelConnections(0, data.inputs[0].numChannels, true);
        SetInputChannelConnections(data.inputs[0].numChannels, NInChannels() - data.inputs[0].numChannels, false);
        AttachInputBuffers(0, NInChannels(), data.inputs[0].channelBuffers32, data.numSamples);
      }
    }

    for (int outBus = 0, chanOffset = 0; outBus < data.numOutputs; outBus++)
    {
      int busChannels = data.outputs[outBus].numChannels;
      SetOutputChannelConnections(chanOffset, busChannels, (bool) getAudioOutput(outBus)->isActive());
      SetOutputChannelConnections(chanOffset + busChannels, NOutChannels() - (chanOffset + busChannels), false);
      AttachOutputBuffers(chanOffset, busChannels, data.outputs[outBus].channelBuffers32);
      chanOffset += busChannels;
    }

    if (mIsBypassed)
      PassThroughBuffers(0.0f, data.numSamples);
    else
      ProcessBuffers(0.0f, data.numSamples); // process buffers single precision
  }

#pragma mark process double precision

  else if (processSetup.symbolicSampleSize == kSample64)
  {
    if (data.numInputs)
    {
      if (mScChans)
      {
        if (getAudioInput(1)->isActive()) // Sidechain is active
        {
          mSidechainActive = true;
          SetInputChannelConnections(0, NInChannels(), true);
        }
        else
        {
          if (mSidechainActive)
          {
            ZeroScratchBuffers();
            mSidechainActive = false;
          }

          SetInputChannelConnections(0, NInChannels(), true);
          SetInputChannelConnections(data.inputs[0].numChannels, NInChannels() - mScChans, false);
        }

        AttachInputBuffers(0, NInChannels() - mScChans, data.inputs[0].channelBuffers64, data.numSamples);
        AttachInputBuffers(mScChans, NInChannels() - mScChans, data.inputs[1].channelBuffers64, data.numSamples);
      }
      else
      {
        SetInputChannelConnections(0, data.inputs[0].numChannels, true);
        SetInputChannelConnections(data.inputs[0].numChannels, NInChannels() - data.inputs[0].numChannels, false);
        AttachInputBuffers(0, NInChannels(), data.inputs[0].channelBuffers64, data.numSamples);
      }
    }

    for (int outBus = 0, chanOffset = 0; outBus < data.numOutputs; outBus++)
    {
      int busChannels = data.outputs[outBus].numChannels;
      SetOutputChannelConnections(chanOffset, busChannels, (bool) getAudioOutput(outBus)->isActive());
      SetOutputChannelConnections(chanOffset + busChannels, NOutChannels() - (chanOffset + busChannels), false);
      AttachOutputBuffers(chanOffset, busChannels, data.outputs[outBus].channelBuffers64);
      chanOffset += busChannels;
    }

    if (mIsBypassed)
      PassThroughBuffers(0.0, data.numSamples);
    else
      ProcessBuffers(0.0, data.numSamples); // process buffers double precision
  }

// Midi Out
//  if (mDoesMidi) {
//    IEventList eventList = data.outputEvents;
//
//    if (eventList)
//    {
//      Event event;
//
//      while (!mMidiOutputQueue.Empty()) {
//        //TODO: parse events and add
//        eventList.addEvent(event);
//      }
//    }
//  }

  return kResultOk;
}

//TODO: VST3 State needs work

tresult PLUGIN_API IPlugVST3::canProcessSampleSize(int32 symbolicSampleSize)
{
  tresult retval = kResultFalse;

  switch (symbolicSampleSize)
  {
    case kSample32:
    case kSample64:
      retval = kResultTrue;
      break;
    default:
      retval = kResultFalse;
      break;
  }

  return retval;
}

Steinberg::uint32 PLUGIN_API IPlugVST3::getLatencySamples () 
{ 
  return mLatency;
} 

#pragma mark -
#pragma mark IEditController overrides

IPlugView* PLUGIN_API IPlugVST3::createView (const char* name)
{
  if (name && strcmp (name, "editor") == 0)
  {
    IPlugVST3View* view = new IPlugVST3View(this);
    addDependentView(view);
    return view;
  }

  return 0;
}

tresult PLUGIN_API IPlugVST3::setEditorState(IBStream* state)
{
  TRACE;

  ByteChunk chunk;
  //InitChunkWithIPlugVer(&chunk); // TODO: IPlugVer should be in chunk!

  SerializeState(chunk); // to get the size

  if (chunk.Size() > 0)
  {
    state->read(chunk.GetBytes(), chunk.Size());
    UnserializeState(chunk, 0);
    
    int32 savedBypass = 0;
    
    if (state->read (&savedBypass, sizeof (int32)) != kResultOk)
    {
      return kResultFalse;
    }
    
    mIsBypassed = (bool) savedBypass;
    
    RedrawParamControls();
    return kResultOk;
  }

  return kResultFalse;
}

tresult PLUGIN_API IPlugVST3::getEditorState(IBStream* state)
{
  TRACE;

  ByteChunk chunk;

  if (SerializeState(chunk))
  {
    state->write(chunk.GetBytes(), chunk.Size());
  }
  else
  {
    return kResultFalse;
  }  
  
  int32 toSaveBypass = mIsBypassed ? 1 : 0;
  state->write(&toSaveBypass, sizeof (int32));  

  return kResultOk;
}

ParamValue PLUGIN_API IPlugVST3::plainParamToNormalized(ParamID tag, ParamValue plainValue)
{
  WDL_MutexLock lock(&mParams_mutex);
  IParam* param = GetParam(tag);

  if (param)
  {
    return param->GetNormalized(plainValue);
  }

  return plainValue;
}

ParamValue PLUGIN_API IPlugVST3::getParamNormalized(ParamID tag)
{
  if (tag == kBypassParam) 
  {
    return (ParamValue) mIsBypassed;
  }
//   else if (tag == kPresetParam) 
//   {
//     return (ParamValue) ToNormalizedParam(mCurrentPresetIdx, 0, NPresets(), 1.);
//   }

  WDL_MutexLock lock(&mParams_mutex);
  IParam* param = GetParam(tag);

  if (param)
  {
    return param->GetNormalized();
  }

  return 0.0;
}

tresult PLUGIN_API IPlugVST3::setParamNormalized(ParamID tag, ParamValue value)
{
  WDL_MutexLock lock(&mParams_mutex);
  IParam* param = GetParam(tag);

  if (param)
  {
    param->SetNormalized(value);
    return kResultOk;
  }

  return kResultFalse;
}

tresult PLUGIN_API IPlugVST3::getParamStringByValue(ParamID tag, ParamValue valueNormalized, String128 string)
{
  WDL_MutexLock lock(&mParams_mutex);
  IParam* param = GetParam(tag);

  if (param)
  {
    char disp[MAX_PARAM_NAME_LEN];
    param->GetDisplayForHost(valueNormalized, true, disp);
    Steinberg::UString(string, 128).fromAscii(disp);
    return kResultTrue;
  }

  return kResultFalse;
}

tresult PLUGIN_API IPlugVST3::getParamValueByString (ParamID tag, TChar* string, ParamValue& valueNormalized)
{
  return SingleComponentEffect::getParamValueByString (tag, string, valueNormalized);
}

void IPlugVST3::addDependentView(IPlugVST3View* view)
{
  mViews.push_back(view);
}

void IPlugVST3::removeDependentView(IPlugVST3View* view)
{
  mViews.erase(std::remove(mViews.begin(), mViews.end(), view));
}

tresult IPlugVST3::beginEdit(ParamID tag)
{
  if (componentHandler)
    return componentHandler->beginEdit(tag);
  return kResultFalse;
}

tresult IPlugVST3::performEdit(ParamID tag, ParamValue valueNormalized)
{
  if (componentHandler)
    return componentHandler->performEdit(tag, valueNormalized);
  return kResultFalse;
}

tresult IPlugVST3::endEdit(ParamID tag)
{
  if (componentHandler)
    return componentHandler->endEdit(tag);
  return kResultFalse;
}

AudioBus* IPlugVST3::getAudioInput (int32 index)
{
  AudioBus* bus = FCast<AudioBus> (audioInputs.at(index));
  return bus;
}

AudioBus* IPlugVST3::getAudioOutput (int32 index)
{
  AudioBus* bus = FCast<AudioBus> (audioOutputs.at(index));
  return bus;
}

// TODO: more speaker arrs
SpeakerArrangement IPlugVST3::getSpeakerArrForChans(int32 chans)
{
  switch (chans)
  {
    case 1:
      return SpeakerArr::kMono;
    case 2:
      return SpeakerArr::kStereo;
    case 3:
      return SpeakerArr::k30Music;
    case 4:
      return SpeakerArr::kAmbi1stOrderACN;
    case 5:
      return SpeakerArr::k50;
    case 6:
      return SpeakerArr::k51;
    default:
      return SpeakerArr::kEmpty;
      break;
  }
}

#pragma mark -
#pragma mark IUnitInfo overrides

int32 PLUGIN_API IPlugVST3::getUnitCount()
{
  TRACE;
  
  return mParamGroups.GetSize() + 1;
}

tresult PLUGIN_API IPlugVST3::getUnitInfo(int32 unitIndex, UnitInfo& info)
{
  TRACE;
  
  if (unitIndex == 0) 
  {
    info.id = kRootUnitId;
    info.parentUnitId = kNoParentUnitId;
    UString name(info.name, 128);
    name.fromAscii("Root Unit");
#ifdef VST3_PRESET_LIST
    info.programListId = kPresetParam;
#else
    info.programListId = kNoProgramListId;
#endif
    return kResultTrue;
  }
  else if (unitIndex > 0 && mParamGroups.GetSize()) 
  {
    info.id = unitIndex;
    info.parentUnitId = kRootUnitId;
    info.programListId = kNoProgramListId;
    
    UString name(info.name, 128);
    name.fromAscii(mParamGroups.Get(unitIndex-1));
    
    return kResultTrue;
  }

  return kResultFalse;
}

int32 PLUGIN_API IPlugVST3::getProgramListCount()
{
#ifdef VST3_PRESET_LIST
  return (NPresets() > 0);
#else
  return 0;
#endif
}

tresult PLUGIN_API IPlugVST3::getProgramListInfo(int32 listIndex, ProgramListInfo& info /*out*/)
{
  if (listIndex == 0)
  {
    info.id = kPresetParam;
    info.programCount = (int32) NPresets();
    UString name(info.name, 128);
    name.fromAscii("Factory Presets");
    return kResultTrue;
  }
  return kResultFalse;
}

tresult PLUGIN_API IPlugVST3::getProgramName(ProgramListID listId, int32 programIndex, String128 name /*out*/)
{
  if (listId == kPresetParam)
  {
    Steinberg::UString(name, 128).fromAscii(GetPresetName(programIndex));
    return kResultTrue;
  }
  return kResultFalse;
}

#pragma mark -
#pragma mark IPlugBase overrides

void IPlugVST3::BeginInformHostOfParamChange(int idx)
{
  Trace(TRACELOC, "%d", idx);
  beginEdit(idx);
}

void IPlugVST3::InformHostOfParamChange(int idx, double normalizedValue)
{
  Trace(TRACELOC, "%d:%f", idx, normalizedValue);
  performEdit(idx, normalizedValue);
}

void IPlugVST3::EndInformHostOfParamChange(int idx)
{
  Trace(TRACELOC, "%d", idx);
  endEdit(idx);
}

void IPlugVST3::GetTime(ITimeInfo& timeInfo)
{
  timeInfo.mSamplePos = (double) mProcessContext.projectTimeSamples;
  timeInfo.mPPQPos = mProcessContext.projectTimeMusic;
  timeInfo.mTempo = mProcessContext.tempo;
  timeInfo.mLastBar = mProcessContext.barPositionMusic;
  timeInfo.mCycleStart = mProcessContext.cycleStartMusic;
  timeInfo.mCycleEnd = mProcessContext.cycleEndMusic;
  timeInfo.mNumerator = mProcessContext.timeSigNumerator;
  timeInfo.mDenominator = mProcessContext.timeSigDenominator;
  timeInfo.mTransportIsRunning = mProcessContext.state & ProcessContext::kPlaying;
  timeInfo.mTransportLoopEnabled = mProcessContext.state & ProcessContext::kCycleActive;
}

double IPlugVST3::GetTempo()
{
  return mProcessContext.tempo;
}

void IPlugVST3::GetTimeSig(int& numerator, int& denominator)
{
  numerator = mProcessContext.timeSigNumerator;
  denominator = mProcessContext.timeSigDenominator;
}

int IPlugVST3::GetSamplePos()
{
  return (int) mProcessContext.projectTimeSamples;
}

void IPlugVST3::ResizeGraphics(int w, int h, double scale)
{
  if (GetHasUI())
  {
    mViews.at(0)->resize(w, h); // only resize view 0?
  }
}

void IPlugVST3::SetLatency(int latency)
{
  IPlugBase::SetLatency(latency);

  FUnknownPtr<IComponentHandler>handler(componentHandler);
  handler->restartComponent(kLatencyChanged);  
}

#pragma mark -
#pragma mark IPlugVST3View
IPlugVST3View::IPlugVST3View(IPlugVST3* pPlug)
  : mPlug(pPlug)
  , mExpectingNewSize(false)
{
  if (mPlug)
    mPlug->addRef();
}

IPlugVST3View::~IPlugVST3View()
{
  if (mPlug)
  {
    mPlug->removeDependentView (this);
    mPlug->release();
  }
}

tresult PLUGIN_API IPlugVST3View::isPlatformTypeSupported(FIDString type)
{
  if(mPlug->GetHasUI()) // for no editor plugins
  {
#ifdef OS_WIN
    if (strcmp (type, kPlatformTypeHWND) == 0)
      return kResultTrue;

#elif defined OS_OSX
    if (strcmp (type, kPlatformTypeNSView) == 0)
      return kResultTrue;
    else if (strcmp (type, kPlatformTypeHIView) == 0)
      return kResultTrue;
#endif
  }

  return kResultFalse;
}

tresult PLUGIN_API IPlugVST3View::onSize(ViewRect* newSize)
{
  TRACE;

  if (newSize)
  {
    rect = *newSize;

    if (mExpectingNewSize)
    {
      mPlug->OnWindowResize();
      mExpectingNewSize = false;
    }
  }

  return kResultTrue;
}

tresult PLUGIN_API IPlugVST3View::getSize(ViewRect* size)
{
  TRACE;

  if (mPlug->GetHasUI())
  {
    *size = ViewRect(0, 0, mPlug->GetUIWidth(), mPlug->GetUIHeight());

    return kResultTrue;
  }
  else
  {
    return kResultFalse;
  }
}

tresult PLUGIN_API IPlugVST3View::attached (void* parent, FIDString type)
{
  if (mPlug->GetHasUI())
  {
    #ifdef OS_WIN
    if (strcmp (type, kPlatformTypeHWND) == 0)
      mPlug->OpenWindow(parent);
    #elif defined OS_OSX
    if (strcmp (type, kPlatformTypeNSView) == 0)
      mPlug->OpenWindow(parent);
    else // Carbon
      return kResultFalse;
    #endif
    mPlug->OnGUIOpen();

    return kResultTrue;
  }

  return kResultFalse;
}

tresult PLUGIN_API IPlugVST3View::removed()
{
  if (mPlug->GetHasUI())
  {
    mPlug->OnGUIClose();
    mPlug->CloseWindow();
  }

  return CPluginView::removed();
}

void IPlugVST3View::resize(int w, int h)
{
  TRACE;

  ViewRect newSize = ViewRect(0, 0, w, h);
  mExpectingNewSize = true;
  plugFrame->resizeView(this, &newSize);
}
