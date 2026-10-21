#ifndef ProxyAudioDevice_h
#define ProxyAudioDevice_h

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreAudio/CoreAudio.h>
#include <vector>
#include <atomic>

#include "AudioDevice.h"
#include "CAMutex.h"

class AudioRingBuffer;

enum {
    kObjectID_PlugIn = kAudioObjectPlugInObject,
    kObjectID_Box = 2,
    kObjectID_Device = 3,
    kObjectID_Stream_Output = 4,
    kObjectID_Volume_Output_L = 5,
    kObjectID_Volume_Output_R = 6,
    kObjectID_Mute_Output_Master = 7,
    kObjectID_DataSource_Output_Master = 8
};

#define kPlugIn_BundleID "net.briankendall.ProxyAudioDevice"
#define kBox_UID "ProxyAudioBox_UID"
#define kDevice_UID "ProxyAudioDevice_UID"
#define kDevice_ModelUID "ProxyAudioDevice_ModelUID"
#define kOutputDeviceDefaultBufferFrameSize 512
#define kOutputDeviceMinBufferFrameSize 4
#define kOutputDeviceDefaultActiveCondition ActiveCondition::userActive
// Default to false so the proxy device stays visible even when the target is
// unavailable. This preserves the long-standing behavior for existing users.
#define kOutputDeviceDefaultHideWhenUnavailable false

// Enhanced feature: when the target output device disappears (e.g. it is
// unplugged), remove the proxy device from the audio device list after a grace
// period, so the system automatically falls back to another output. The
// removal is debounced by kTargetRemovalGracePeriodSeconds and is re-checked
// against the device list right before it executes, so brief link retrains
// (USB-C displays waking up) do not flap the device list. This is a code-level
// switch only; there is intentionally no settings UI for it yet.
#define kRemoveProxyWhenTargetUnavailableDefault true
#define kTargetRemovalGracePeriodSeconds 10.0

// Enhanced feature: perceptual volume curve. The scalar volume (0 to 1) is
// converted to dB as  dB = kVolume_MinDB + (kVolume_MaxDB - kVolume_MinDB) *
// pow(scalar, gamma).  gamma = 1 is linear-in-dB (upstream behavior). gamma
// < 1 spends more slider travel in the loud region for finer control there;
// gamma > 1 does the opposite. gamma is user-adjustable from the settings app.
#define kVolumeCurveGammaDefault 1.0
#define kVolumeCurveGammaMin 0.5
#define kVolumeCurveGammaMax 1.5

// Proxy clock servo (see GetZeroTimeStamp / updateProxyClockCorrectionNoLock). The proxy device's
// clock is synthetic, so rather than letting it free-run at a rate estimated from the target
// device's rate scalar alone, its rate is steered so that the ring buffer fill level measured in
// the target device's IO proc stays at the level it had when the read position was anchored. The
// error signal is the phase between the two clocks in frames, measured by projecting the proxy's
// time line onto the host time of each target device IO cycle (a continuous quantity), rather than
// the ring buffer's write pointer, which only advances once per input cycle and so carries a
// phase-dependent offset of up to one input buffer.
// Gains are per frame of phase error, applied once per zero time stamp period.
#define kProxyClockGainP (1.0 / (64.0 * 16384.0))
#define kProxyClockGainI (kProxyClockGainP / 128.0)
#define kProxyClockMaxCorrection 500e-6
#define kProxyClockMaxIntegral (kProxyClockMaxCorrection / kProxyClockGainI)
// A forward jump in a device's IO sample time larger than this (or any backward jump) is treated
// as the device having established a new time line, e.g. after coreaudiod restarted its IO engine.
#define kTimelineJumpToleranceFrames 16384.0

class ProxyAudioDevice {
  public:
    enum class ConfigType {
        none,
        outputDevice,
        outputDeviceBufferFrameSize,
        deviceName,
        deviceActiveCondition,
        deviceHideWhenUnavailable,
        volumeCurveGamma
    };
    enum class ActiveCondition { proxiedDeviceActive = 0, userActive = 1, always = 2 };

    ProxyAudioDevice() : inputIOIsActive(false) {};
    AudioDevice findTargetOutputAudioDevice();
    static int outputDeviceAliveListenerStatic(AudioObjectID inObjectID,
                                               UInt32 inNumberAddresses,
                                               const AudioObjectPropertyAddress *inAddresses,
                                               void *inClientData);
    int outputDeviceAliveListener(AudioObjectID inObjectID,
                                  UInt32 inNumberAddresses,
                                  const AudioObjectPropertyAddress *inAddresses);
    static int outputDeviceSampleRateListenerStatic(AudioObjectID inObjectID,
                                                    UInt32 inNumberAddresses,
                                                    const AudioObjectPropertyAddress *inAddresses,
                                                    void *inClientData);
    int outputDeviceSampleRateListener(AudioObjectID inObjectID,
                                       UInt32 inNumberAddresses,
                                       const AudioObjectPropertyAddress *inAddresses);
    void updateOutputDeviceStartedState();
    void matchOutputDeviceSampleRateNoLock();
    void matchOutputDeviceSampleRate();
    static int devicesListenerProcStatic(AudioObjectID inObjectID,
                                         UInt32 inNumberAddresses,
                                         const AudioObjectPropertyAddress *inAddresses,
                                         void *inClientData);
    int devicesListenerProc(AudioObjectID inObjectID,
                            UInt32 inNumberAddresses,
                            const AudioObjectPropertyAddress *inAddresses);
    void setupAudioDevicesListener();
    void setupTargetOutputDevice();
    void initializeOutputDevice();
    void deinitializeOutputDeviceNoLock();
    void deinitializeOutputDevice();
    void resetInputData();
    static OSStatus outputDeviceIOProcStatic(AudioDeviceID inDevice,
                                             const AudioTimeStamp *inNow,
                                             const AudioBufferList *inInputData,
                                             const AudioTimeStamp *inInputTime,
                                             AudioBufferList *outOutputData,
                                             const AudioTimeStamp *inOutputTime,
                                             void *inClientData);
    OSStatus outputDeviceIOProc(AudioDeviceID inDevice,
                                const AudioTimeStamp *inNow,
                                const AudioBufferList *inInputData,
                                const AudioTimeStamp *inInputTime,
                                AudioBufferList *outOutputData,
                                const AudioTimeStamp *inOutputTime);
    Float64 anchorInputOutputSampleDelta(const AudioTimeStamp *outputTime,
                                         UInt32 outputBufferFrameSize,
                                         UInt32 safetyOffset);
    void updateProxyClockCorrectionNoLock();
    Float64 proxySampleTimeForHostTimeNoLock(UInt64 hostTime);
    void calculateVolumeFactors(Float32 volumeL,
                                Float32 volumeR,
                                bool mute,
                                Float32 &volumeFactorL,
                                Float32 &volumeFactorR);
    bool isConfigurationString(CFStringRef val);
    void parseConfigurationString(CFStringRef configString, ConfigType &action, CFStringRef &value);
    void setConfigurationValue(ConfigType action, CFStringRef value);
    CFStringRef copyConfigurationValue(ConfigType action);
    CFStringRef copyDeviceNameFromStorage();
    void setDeviceName(CFStringRef newName);
    CFStringRef copyDefaultProxyOutputDeviceUID();
    CFStringRef copyOutputDeviceUIDFromStorage();
    void setOutputDevice(CFStringRef deviceUID);
    UInt32 retrieveOutputDeviceBufferFrameSizeFromStorage();
    void setOutputDeviceBufferFrameSize(UInt32 size);
    ActiveCondition retrieveOutputDeviceActiveConditionFromStorage();
    void setOutputDeviceActiveCondition(ActiveCondition newActiveCondition);
    bool retrieveOutputDeviceHideWhenUnavailableFromStorage();
    void setOutputDeviceHideWhenUnavailable(bool newHideWhenUnavailable);
    void notifyHiddenPropertyChanged();

    // Enhanced: dynamic removal / restoration of the proxy device when the
    // target output device disappears / reappears.
    void armProxyRemovalIfNeeded();
    void cancelProxyRemoval();
    void removeProxyDeviceFromSystem();
    void restoreProxyDeviceFromSystem();
    void notifyPluginDeviceListChanged();
    bool proxyDeviceIsVisible() const { return gBox_Acquired && !proxyDeviceRemoved; }

    // Enhanced: perceptual volume curve helpers. These read volumeCurveGamma
    // without taking stateMutex; callers that already hold stateMutex must not
    // call them (a single Float32 read is benignly racy).
    Float32 volumeScalarToDecibels(Float32 scalar);
    Float32 volumeDecibelsToScalar(Float32 decibels);
    Float32 retrieveVolumeCurveGammaFromStorage();
    void setVolumeCurveGamma(Float32 newGamma);

    static ProxyAudioDevice *deviceForDriver(void *inDriver);

    //    Entry points for the COM methods
    static HRESULT ProxyAudio_QueryInterface(void *inDriver, REFIID inUUID, LPVOID *outInterface);
    static ULONG ProxyAudio_AddRef(void *inDriver);
    static ULONG ProxyAudio_Release(void *inDriver);
    static OSStatus ProxyAudio_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost);
    static OSStatus ProxyAudio_CreateDevice(AudioServerPlugInDriverRef inDriver,
                                            CFDictionaryRef inDescription,
                                            const AudioServerPlugInClientInfo *inClientInfo,
                                            AudioObjectID *outDeviceObjectID);
    static OSStatus ProxyAudio_DestroyDevice(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID);
    static OSStatus ProxyAudio_AddDeviceClient(AudioServerPlugInDriverRef inDriver,
                                               AudioObjectID inDeviceObjectID,
                                               const AudioServerPlugInClientInfo *inClientInfo);
    static OSStatus ProxyAudio_RemoveDeviceClient(AudioServerPlugInDriverRef inDriver,
                                                  AudioObjectID inDeviceObjectID,
                                                  const AudioServerPlugInClientInfo *inClientInfo);
    static OSStatus ProxyAudio_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver,
                                                                AudioObjectID inDeviceObjectID,
                                                                UInt64 inChangeAction,
                                                                void *inChangeInfo);
    static OSStatus ProxyAudio_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver,
                                                              AudioObjectID inDeviceObjectID,
                                                              UInt64 inChangeAction,
                                                              void *inChangeInfo);
    static Boolean ProxyAudio_HasProperty(AudioServerPlugInDriverRef inDriver,
                                          AudioObjectID inObjectID,
                                          pid_t inClientProcessID,
                                          const AudioObjectPropertyAddress *inAddress);
    static OSStatus ProxyAudio_IsPropertySettable(AudioServerPlugInDriverRef inDriver,
                                                  AudioObjectID inObjectID,
                                                  pid_t inClientProcessID,
                                                  const AudioObjectPropertyAddress *inAddress,
                                                  Boolean *outIsSettable);
    static OSStatus ProxyAudio_GetPropertyDataSize(AudioServerPlugInDriverRef inDriver,
                                                   AudioObjectID inObjectID,
                                                   pid_t inClientProcessID,
                                                   const AudioObjectPropertyAddress *inAddress,
                                                   UInt32 inQualifierDataSize,
                                                   const void *inQualifierData,
                                                   UInt32 *outDataSize);
    static OSStatus ProxyAudio_GetPropertyData(AudioServerPlugInDriverRef inDriver,
                                               AudioObjectID inObjectID,
                                               pid_t inClientProcessID,
                                               const AudioObjectPropertyAddress *inAddress,
                                               UInt32 inQualifierDataSize,
                                               const void *inQualifierData,
                                               UInt32 inDataSize,
                                               UInt32 *outDataSize,
                                               void *outData);
    static OSStatus ProxyAudio_SetPropertyData(AudioServerPlugInDriverRef inDriver,
                                               AudioObjectID inObjectID,
                                               pid_t inClientProcessID,
                                               const AudioObjectPropertyAddress *inAddress,
                                               UInt32 inQualifierDataSize,
                                               const void *inQualifierData,
                                               UInt32 inDataSize,
                                               const void *inData);
    static OSStatus ProxyAudio_StartIO(AudioServerPlugInDriverRef inDriver,
                                       AudioObjectID inDeviceObjectID,
                                       UInt32 inClientID);
    static OSStatus ProxyAudio_StopIO(AudioServerPlugInDriverRef inDriver,
                                      AudioObjectID inDeviceObjectID,
                                      UInt32 inClientID);
    static OSStatus ProxyAudio_GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver,
                                                AudioObjectID inDeviceObjectID,
                                                UInt32 inClientID,
                                                Float64 *outSampleTime,
                                                UInt64 *outHostTime,
                                                UInt64 *outSeed);
    static OSStatus ProxyAudio_WillDoIOOperation(AudioServerPlugInDriverRef inDriver,
                                                 AudioObjectID inDeviceObjectID,
                                                 UInt32 inClientID,
                                                 UInt32 inOperationID,
                                                 Boolean *outWillDo,
                                                 Boolean *outWillDoInPlace);
    static OSStatus ProxyAudio_BeginIOOperation(AudioServerPlugInDriverRef inDriver,
                                                AudioObjectID inDeviceObjectID,
                                                UInt32 inClientID,
                                                UInt32 inOperationID,
                                                UInt32 inIOBufferFrameSize,
                                                const AudioServerPlugInIOCycleInfo *inIOCycleInfo);
    static OSStatus ProxyAudio_DoIOOperation(AudioServerPlugInDriverRef inDriver,
                                             AudioObjectID inDeviceObjectID,
                                             AudioObjectID inStreamObjectID,
                                             UInt32 inClientID,
                                             UInt32 inOperationID,
                                             UInt32 inIOBufferFrameSize,
                                             const AudioServerPlugInIOCycleInfo *inIOCycleInfo,
                                             void *ioMainBuffer,
                                             void *ioSecondaryBuffer);
    static OSStatus ProxyAudio_EndIOOperation(AudioServerPlugInDriverRef inDriver,
                                              AudioObjectID inDeviceObjectID,
                                              UInt32 inClientID,
                                              UInt32 inOperationID,
                                              UInt32 inIOBufferFrameSize,
                                              const AudioServerPlugInIOCycleInfo *inIOCycleInfo);

    //    Implementation
    HRESULT QueryInterface(void *inDriver, REFIID inUUID, LPVOID *outInterface);
    ULONG AddRef(void *inDriver);
    ULONG Release(void *inDriver);
    OSStatus Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost);
    OSStatus CreateDevice(AudioServerPlugInDriverRef inDriver,
                          CFDictionaryRef inDescription,
                          const AudioServerPlugInClientInfo *inClientInfo,
                          AudioObjectID *outDeviceObjectID);
    OSStatus DestroyDevice(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID);
    OSStatus AddDeviceClient(AudioServerPlugInDriverRef inDriver,
                             AudioObjectID inDeviceObjectID,
                             const AudioServerPlugInClientInfo *inClientInfo);
    OSStatus RemoveDeviceClient(AudioServerPlugInDriverRef inDriver,
                                AudioObjectID inDeviceObjectID,
                                const AudioServerPlugInClientInfo *inClientInfo);
    OSStatus PerformDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver,
                                              AudioObjectID inDeviceObjectID,
                                              UInt64 inChangeAction,
                                              void *inChangeInfo);
    OSStatus AbortDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver,
                                            AudioObjectID inDeviceObjectID,
                                            UInt64 inChangeAction,
                                            void *inChangeInfo);
    Boolean HasProperty(AudioServerPlugInDriverRef inDriver,
                        AudioObjectID inObjectID,
                        pid_t inClientProcessID,
                        const AudioObjectPropertyAddress *inAddress);
    OSStatus IsPropertySettable(AudioServerPlugInDriverRef inDriver,
                                AudioObjectID inObjectID,
                                pid_t inClientProcessID,
                                const AudioObjectPropertyAddress *inAddress,
                                Boolean *outIsSettable);
    OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef inDriver,
                                 AudioObjectID inObjectID,
                                 pid_t inClientProcessID,
                                 const AudioObjectPropertyAddress *inAddress,
                                 UInt32 inQualifierDataSize,
                                 const void *inQualifierData,
                                 UInt32 *outDataSize);
    OSStatus GetPropertyData(AudioServerPlugInDriverRef inDriver,
                             AudioObjectID inObjectID,
                             pid_t inClientProcessID,
                             const AudioObjectPropertyAddress *inAddress,
                             UInt32 inQualifierDataSize,
                             const void *inQualifierData,
                             UInt32 inDataSize,
                             UInt32 *outDataSize,
                             void *outData);
    OSStatus SetPropertyData(AudioServerPlugInDriverRef inDriver,
                             AudioObjectID inObjectID,
                             pid_t inClientProcessID,
                             const AudioObjectPropertyAddress *inAddress,
                             UInt32 inQualifierDataSize,
                             const void *inQualifierData,
                             UInt32 inDataSize,
                             const void *inData);
    OSStatus StartIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID);
    OSStatus StopIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID);
    OSStatus GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver,
                              AudioObjectID inDeviceObjectID,
                              UInt32 inClientID,
                              Float64 *outSampleTime,
                              UInt64 *outHostTime,
                              UInt64 *outSeed);
    OSStatus WillDoIOOperation(AudioServerPlugInDriverRef inDriver,
                               AudioObjectID inDeviceObjectID,
                               UInt32 inClientID,
                               UInt32 inOperationID,
                               Boolean *outWillDo,
                               Boolean *outWillDoInPlace);
    OSStatus BeginIOOperation(AudioServerPlugInDriverRef inDriver,
                              AudioObjectID inDeviceObjectID,
                              UInt32 inClientID,
                              UInt32 inOperationID,
                              UInt32 inIOBufferFrameSize,
                              const AudioServerPlugInIOCycleInfo *inIOCycleInfo);
    OSStatus DoIOOperation(AudioServerPlugInDriverRef inDriver,
                           AudioObjectID inDeviceObjectID,
                           AudioObjectID inStreamObjectID,
                           UInt32 inClientID,
                           UInt32 inOperationID,
                           UInt32 inIOBufferFrameSize,
                           const AudioServerPlugInIOCycleInfo *inIOCycleInfo,
                           void *ioMainBuffer,
                           void *ioSecondaryBuffer);
    OSStatus EndIOOperation(AudioServerPlugInDriverRef inDriver,
                            AudioObjectID inDeviceObjectID,
                            UInt32 inClientID,
                            UInt32 inOperationID,
                            UInt32 inIOBufferFrameSize,
                            const AudioServerPlugInIOCycleInfo *inIOCycleInfo);

    Boolean HasPlugInProperty(AudioServerPlugInDriverRef inDriver,
                              AudioObjectID inObjectID,
                              pid_t inClientProcessID,
                              const AudioObjectPropertyAddress *inAddress);
    OSStatus IsPlugInPropertySettable(AudioServerPlugInDriverRef inDriver,
                                      AudioObjectID inObjectID,
                                      pid_t inClientProcessID,
                                      const AudioObjectPropertyAddress *inAddress,
                                      Boolean *outIsSettable);
    OSStatus GetPlugInPropertyDataSize(AudioServerPlugInDriverRef inDriver,
                                       AudioObjectID inObjectID,
                                       pid_t inClientProcessID,
                                       const AudioObjectPropertyAddress *inAddress,
                                       UInt32 inQualifierDataSize,
                                       const void *inQualifierData,
                                       UInt32 *outDataSize);
    OSStatus GetPlugInPropertyData(AudioServerPlugInDriverRef inDriver,
                                   AudioObjectID inObjectID,
                                   pid_t inClientProcessID,
                                   const AudioObjectPropertyAddress *inAddress,
                                   UInt32 inQualifierDataSize,
                                   const void *inQualifierData,
                                   UInt32 inDataSize,
                                   UInt32 *outDataSize,
                                   void *outData);
    OSStatus SetPlugInPropertyData(AudioServerPlugInDriverRef inDriver,
                                   AudioObjectID inObjectID,
                                   pid_t inClientProcessID,
                                   const AudioObjectPropertyAddress *inAddress,
                                   UInt32 inQualifierDataSize,
                                   const void *inQualifierData,
                                   UInt32 inDataSize,
                                   const void *inData,
                                   UInt32 *outNumberPropertiesChanged,
                                   AudioObjectPropertyAddress outChangedAddresses[2]);
    Boolean HasBoxProperty(AudioServerPlugInDriverRef inDriver,
                           AudioObjectID inObjectID,
                           pid_t inClientProcessID,
                           const AudioObjectPropertyAddress *inAddress);
    OSStatus IsBoxPropertySettable(AudioServerPlugInDriverRef inDriver,
                                   AudioObjectID inObjectID,
                                   pid_t inClientProcessID,
                                   const AudioObjectPropertyAddress *inAddress,
                                   Boolean *outIsSettable);
    OSStatus GetBoxPropertyDataSize(AudioServerPlugInDriverRef inDriver,
                                    AudioObjectID inObjectID,
                                    pid_t inClientProcessID,
                                    const AudioObjectPropertyAddress *inAddress,
                                    UInt32 inQualifierDataSize,
                                    const void *inQualifierData,
                                    UInt32 *outDataSize);
    OSStatus GetBoxPropertyData(AudioServerPlugInDriverRef inDriver,
                                AudioObjectID inObjectID,
                                pid_t inClientProcessID,
                                const AudioObjectPropertyAddress *inAddress,
                                UInt32 inQualifierDataSize,
                                const void *inQualifierData,
                                UInt32 inDataSize,
                                UInt32 *outDataSize,
                                void *outData);
    OSStatus SetBoxPropertyData(AudioServerPlugInDriverRef inDriver,
                                AudioObjectID inObjectID,
                                pid_t inClientProcessID,
                                const AudioObjectPropertyAddress *inAddress,
                                UInt32 inQualifierDataSize,
                                const void *inQualifierData,
                                UInt32 inDataSize,
                                const void *inData,
                                UInt32 *outNumberPropertiesChanged,
                                AudioObjectPropertyAddress outChangedAddresses[2]);

    Boolean HasDeviceProperty(AudioServerPlugInDriverRef inDriver,
                              AudioObjectID inObjectID,
                              pid_t inClientProcessID,
                              const AudioObjectPropertyAddress *inAddress);
    OSStatus IsDevicePropertySettable(AudioServerPlugInDriverRef inDriver,
                                      AudioObjectID inObjectID,
                                      pid_t inClientProcessID,
                                      const AudioObjectPropertyAddress *inAddress,
                                      Boolean *outIsSettable);
    OSStatus GetDevicePropertyDataSize(AudioServerPlugInDriverRef inDriver,
                                       AudioObjectID inObjectID,
                                       pid_t inClientProcessID,
                                       const AudioObjectPropertyAddress *inAddress,
                                       UInt32 inQualifierDataSize,
                                       const void *inQualifierData,
                                       UInt32 *outDataSize);
    OSStatus GetDevicePropertyData(AudioServerPlugInDriverRef inDriver,
                                   AudioObjectID inObjectID,
                                   pid_t inClientProcessID,
                                   const AudioObjectPropertyAddress *inAddress,
                                   UInt32 inQualifierDataSize,
                                   const void *inQualifierData,
                                   UInt32 inDataSize,
                                   UInt32 *outDataSize,
                                   void *outData);
    OSStatus SetDevicePropertyData(AudioServerPlugInDriverRef inDriver,
                                   AudioObjectID inObjectID,
                                   pid_t inClientProcessID,
                                   const AudioObjectPropertyAddress *inAddress,
                                   UInt32 inQualifierDataSize,
                                   const void *inQualifierData,
                                   UInt32 inDataSize,
                                   const void *inData,
                                   UInt32 *outNumberPropertiesChanged,
                                   AudioObjectPropertyAddress outChangedAddresses[2]);
    Boolean HasStreamProperty(AudioServerPlugInDriverRef inDriver,
                              AudioObjectID inObjectID,
                              pid_t inClientProcessID,
                              const AudioObjectPropertyAddress *inAddress);
    OSStatus IsStreamPropertySettable(AudioServerPlugInDriverRef inDriver,
                                      AudioObjectID inObjectID,
                                      pid_t inClientProcessID,
                                      const AudioObjectPropertyAddress *inAddress,
                                      Boolean *outIsSettable);
    OSStatus GetStreamPropertyDataSize(AudioServerPlugInDriverRef inDriver,
                                       AudioObjectID inObjectID,
                                       pid_t inClientProcessID,
                                       const AudioObjectPropertyAddress *inAddress,
                                       UInt32 inQualifierDataSize,
                                       const void *inQualifierData,
                                       UInt32 *outDataSize);
    OSStatus GetStreamPropertyData(AudioServerPlugInDriverRef inDriver,
                                   AudioObjectID inObjectID,
                                   pid_t inClientProcessID,
                                   const AudioObjectPropertyAddress *inAddress,
                                   UInt32 inQualifierDataSize,
                                   const void *inQualifierData,
                                   UInt32 inDataSize,
                                   UInt32 *outDataSize,
                                   void *outData);
    OSStatus SetStreamPropertyData(AudioServerPlugInDriverRef inDriver,
                                   AudioObjectID inObjectID,
                                   pid_t inClientProcessID,
                                   const AudioObjectPropertyAddress *inAddress,
                                   UInt32 inQualifierDataSize,
                                   const void *inQualifierData,
                                   UInt32 inDataSize,
                                   const void *inData,
                                   UInt32 *outNumberPropertiesChanged,
                                   AudioObjectPropertyAddress outChangedAddresses[2]);
    Boolean HasControlProperty(AudioServerPlugInDriverRef inDriver,
                               AudioObjectID inObjectID,
                               pid_t inClientProcessID,
                               const AudioObjectPropertyAddress *inAddress);
    OSStatus IsControlPropertySettable(AudioServerPlugInDriverRef inDriver,
                                       AudioObjectID inObjectID,
                                       pid_t inClientProcessID,
                                       const AudioObjectPropertyAddress *inAddress,
                                       Boolean *outIsSettable);
    OSStatus GetControlPropertyDataSize(AudioServerPlugInDriverRef inDriver,
                                        AudioObjectID inObjectID,
                                        pid_t inClientProcessID,
                                        const AudioObjectPropertyAddress *inAddress,
                                        UInt32 inQualifierDataSize,
                                        const void *inQualifierData,
                                        UInt32 *outDataSize);
    OSStatus GetControlPropertyData(AudioServerPlugInDriverRef inDriver,
                                    AudioObjectID inObjectID,
                                    pid_t inClientProcessID,
                                    const AudioObjectPropertyAddress *inAddress,
                                    UInt32 inQualifierDataSize,
                                    const void *inQualifierData,
                                    UInt32 inDataSize,
                                    UInt32 *outDataSize,
                                    void *outData);
    OSStatus SetControlPropertyData(AudioServerPlugInDriverRef inDriver,
                                    AudioObjectID inObjectID,
                                    pid_t inClientProcessID,
                                    const AudioObjectPropertyAddress *inAddress,
                                    UInt32 inQualifierDataSize,
                                    const void *inQualifierData,
                                    UInt32 inDataSize,
                                    const void *inData,
                                    UInt32 *outNumberPropertiesChanged,
                                    AudioObjectPropertyAddress outChangedAddresses[2]);
    void monitorUserActivity();
    dispatch_queue_t AudioOutputDispatchQueue();
    void ExecuteInAudioOutputThread(void (^block)());
    
    CAMutex stateMutex = CAMutex("ProxyAudioStateMutex");
    CAMutex IOMutex = CAMutex("ProxyAudioIOMutex");
    CAMutex outputDeviceMutex = CAMutex("ProxyAudioOutputDeviceMutex");
    CAMutex getZeroTimestampMutex = CAMutex("ProxyAudioGetZeroTimestampMutex");
    dispatch_queue_t audioOutputQueue = NULL;
    dispatch_source_t inputMonitoringTimer = NULL;
    AudioRingBuffer *inputBuffer = NULL;
    Byte *workBuffer = NULL;
    AudioDevice outputDevice;
    bool outputDeviceReady = false;
    std::atomic_bool inputIOIsActive;
    Float64 lastInputFrameTime = -1;
    Float64 lastInputBufferFrameSize = -1;
    Float64 inputOutputSampleDelta = 0;
    bool inputOutputSampleDeltaValid = false;
    Float64 inputFinalFrameTime = -1;
    // Sample time we expect the target device's next IO cycle to start at, or -1 if unknown.
    Float64 expectedOutputSampleTime = -1;
    int inputCycleCount = 0;
    ConfigType nextConfigurationToRead = ConfigType::none;
    pid_t configuratorPid = 0;
    CFStringRef deviceName = NULL;
    CFStringRef boxName = NULL;
    CFStringRef outputDeviceUID = NULL;
    UInt32 outputDeviceBufferFrameSize = kOutputDeviceDefaultBufferFrameSize;
    // Proxy clock servo state. Everything below up to outputDeviceActiveCondition is protected by
    // getZeroTimestampMutex.
    Float64 outputAccumulatedRateRatio = 0.0;
    UInt64 outputAccumulatedRateRatioSamples = 0;
    Float64 outputRateRatio = 1.0;
    Float64 proxyClockCorrection = 1.0;
    Float64 proxyHostTicksPerFrame = 0.0;
    Float64 phaseFramesAtAnchor = 0.0;
    Float64 phaseErrorAccumulated = 0.0;
    UInt64 phaseErrorSamples = 0;
    Float64 phaseErrorIntegral = 0.0;
    UInt64 proxyClockPeriodsSinceLog = 0;
    ActiveCondition outputDeviceActiveCondition = ActiveCondition::userActive;
    bool outputDeviceHideWhenUnavailable = kOutputDeviceDefaultHideWhenUnavailable;

    // Enhanced: proxy device auto-removal state (protected by stateMutex).
    const bool removeProxyWhenTargetUnavailable = kRemoveProxyWhenTargetUnavailableDefault;
    bool proxyDeviceRemoved = false;
    bool proxyRemovalTimerArmed = false;
    UInt64 proxyRemovalGeneration = 0;

    // Enhanced: perceptual volume curve exponent (protected by stateMutex).
    Float32 volumeCurveGamma = kVolumeCurveGammaDefault;
    
    UInt32 gPlugIn_RefCount = 0;
    AudioServerPlugInHostRef gPlugIn_Host = NULL;
    Boolean gBox_Acquired = true;
    Float64 gDevice_SampleRate = 44100.0;
    std::vector<Float64> gDevice_SampleRates = {22050, 44100, 48000, 88200, 96000, 176400, 192000};
    UInt64 gDevice_IOIsRunning = 0;
    const UInt32 kDevice_RingBufferSize = 16384;
    Float64 gDevice_HostTicksPerFrame = 0.0;
    UInt64 gDevice_NumberTimeStamps = 0;
    Float64 gDevice_AnchorSampleTime = 0.0;
    Float64 gDevice_ElapsedTicks = 0.0;
    UInt64 gDevice_AnchorHostTime = 0;
    bool gStream_Output_IsActive = true;
    const Float32 kVolume_MinDB = -25.0;
    const Float32 kVolume_MaxDB = 0.0;
    Float32 gVolume_Output_L_Value = 0.0;
    Float32 gVolume_Output_R_Value = 0.0;
    bool gMute_Output_Mute = false;
    const UInt32 gDevice_BytesPerFrameInChannel = 4;
    const UInt32 gDevice_ChannelsPerFrame = 2;
    const UInt32 gDevice_SafetyOffset = 0;
};

extern "C" void *ProxyAudio_Create(CFAllocatorRef inAllocator, CFUUIDRef inRequestedTypeUUID);

#endif /* ProxyAudioDevice_h */
