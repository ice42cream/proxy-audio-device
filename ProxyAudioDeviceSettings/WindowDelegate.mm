#include <vector>
#include <CoreAudio/CoreAudio.h>
#import "WindowDelegate.h"
#include "AudioDevice.h"
#include "ProxyAudioDevice.h"

int onDevicesChanged(AudioObjectID inObjectID,
                     UInt32 inNumberAddresses,
                     const AudioObjectPropertyAddress *inAddresses,
                     void *inClientData);

// Preview of the perceptual volume curve: scalar volume on the x axis, dB on the
// y axis. Must be kept in sync with kVolume_MinDB / kVolume_MaxDB in
// ProxyAudioDevice.h.
static const Float64 kPreviewMinDB = -25.0;
static const Float64 kPreviewMaxDB = 0.0;

static Float64 previewCurveDB(Float64 scalar, Float64 gamma)
{
    if (scalar <= 0.0) {
        return kPreviewMinDB;
    }
    if (scalar >= 1.0) {
        return kPreviewMaxDB;
    }
    return kPreviewMinDB + pow(scalar, gamma) * (kPreviewMaxDB - kPreviewMinDB);
}

@interface VolumeCurvePreviewView : NSView
@property(nonatomic) Float64 gamma;
@end

@implementation VolumeCurvePreviewView

- (instancetype)initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    if (self) {
        _gamma = kVolumeCurveGammaDefault;
    }
    return self;
}

- (void)drawRect:(NSRect)dirtyRect
{
    [[NSColor whiteColor] setFill];
    NSRectFill(self.bounds);

    const CGFloat padLeft = 44.0;
    const CGFloat padRight = 12.0;
    const CGFloat padTop = 8.0;
    const CGFloat padBottom = 20.0;
    NSRect plot = self.bounds;
    plot.origin.x += padLeft;
    plot.size.width -= padLeft + padRight;
    plot.origin.y += padBottom;
    plot.size.height -= padTop + padBottom;

    NSMutableParagraphStyle *rightAlign = [NSMutableParagraphStyle new];
    rightAlign.alignment = NSTextAlignmentRight;
    NSDictionary *labelAttributes = @{
        NSFontAttributeName : [NSFont systemFontOfSize:9],
        NSForegroundColorAttributeName : NSColor.secondaryLabelColor,
        NSParagraphStyleAttributeName : rightAlign
    };

    // Horizontal gridlines every 5 dB, with the 0 dB line at the top.
    for (int db = (int)kPreviewMinDB; db <= (int)kPreviewMaxDB; db += 5) {
        CGFloat y = plot.origin.y + ((Float64)db - kPreviewMinDB) / (kPreviewMaxDB - kPreviewMinDB) * plot.size.height;
        NSBezierPath *line = [NSBezierPath bezierPath];
        [line moveToPoint:NSMakePoint(plot.origin.x, y)];
        [line lineToPoint:NSMakePoint(NSMaxX(plot), y)];
        if (db == 0) {
            line.lineWidth = 1.2;
            [NSColor.labelColor setStroke];
        } else {
            line.lineWidth = 0.5;
            [NSColor.tertiaryLabelColor setStroke];
        }
        [line stroke];
        [[NSString stringWithFormat:@"%d", db] drawInRect:NSMakeRect(4, y - 6, padLeft - 10, 12)
                                           withAttributes:labelAttributes];
    }

    // Reference curve (gamma = 1, the original linear-in-dB behavior), dashed.
    [[NSColor tertiaryLabelColor] setStroke];
    NSBezierPath *reference = [NSBezierPath bezierPath];
    reference.lineWidth = 1.0;
    CGFloat dashPattern[2] = {4.0, 3.0};
    [reference setLineDash:dashPattern count:2 phase:0.0];
    [reference moveToPoint:NSMakePoint(plot.origin.x, plot.origin.y)];
    [reference lineToPoint:NSMakePoint(NSMaxX(plot), NSMaxY(plot))];
    [reference stroke];

    // Current curve.
    [[NSColor systemBlueColor] setStroke];
    NSBezierPath *curve = [NSBezierPath bezierPath];
    curve.lineWidth = 2.0;
    const int sampleCount = 100;
    for (int i = 0; i <= sampleCount; ++i) {
        Float64 scalar = (Float64)i / (Float64)sampleCount;
        Float64 db = previewCurveDB(scalar, self.gamma);
        CGFloat x = plot.origin.x + scalar * plot.size.width;
        CGFloat y = plot.origin.y + (db - kPreviewMinDB) / (kPreviewMaxDB - kPreviewMinDB) * plot.size.height;
        if (i == 0) {
            [curve moveToPoint:NSMakePoint(x, y)];
        } else {
            [curve lineToPoint:NSMakePoint(x, y)];
        }
    }
    [curve stroke];

    // X axis labels.
    NSDictionary *axisAttributes = @{
        NSFontAttributeName : [NSFont systemFontOfSize:9],
        NSForegroundColorAttributeName : NSColor.secondaryLabelColor
    };
    [@"0" drawInRect:NSMakeRect(plot.origin.x - 2, plot.origin.y - 16, 40, 12) withAttributes:axisAttributes];
    NSString *fullVolume = @"100%";
    NSSize fullVolumeSize = [fullVolume sizeWithAttributes:axisAttributes];
    [fullVolume drawInRect:NSMakeRect(NSMaxX(plot) - fullVolumeSize.width,
                                      plot.origin.y - 16,
                                      fullVolumeSize.width + 2,
                                      12)
            withAttributes:axisAttributes];
}

@end

@implementation WindowDelegate {
    std::vector<AudioDeviceID> currentDeviceList;
    int initializationAttemptInterval;
}

- (void)awakeFromNib {
    self.deviceNameTextField.stringValue = NSLocalizedString(@"< Loading... >", nil);
    self.deviceNameTextField.enabled = NO;
    self.outputDeviceComboBox.enabled = NO;
    self.bufferSizeComboBox.enabled = NO;
    self.proxiedDeviceIsActiveRadioButton.enabled = NO;
    self.userIsActiveRadioButton.enabled = NO;
    self.alwaysRadioButton.enabled = NO;
    self.hideWhenUnavailableCheckbox.enabled = NO;
    [self setupVolumeCurveControls];
    initializationAttemptInterval = 3;
    [self keepTryingToInitializeUntilSuccess];
}

// The volume curve controls are built in code rather than in the xib to keep
// the change self-contained. The window is grown downward to make room.
- (void)setupVolumeCurveControls {
    NSWindow *window = self.hideWhenUnavailableCheckbox.window;
    const CGFloat addedHeight = 140.0;

    NSRect windowFrame = window.frame;
    windowFrame.size.height += addedHeight;
    windowFrame.origin.y -= addedHeight;
    [window setFrame:windowFrame display:NO];

    NSView *contentView = window.contentView;
    const CGFloat contentWidth = contentView.bounds.size.width;
    const CGFloat left = 20.0;
    const CGFloat width = contentWidth - 2 * left;

    // Move the "hide when unavailable" checkbox up into the space it had before.
    NSRect checkboxFrame = self.hideWhenUnavailableCheckbox.frame;
    checkboxFrame.origin.y += addedHeight;
    self.hideWhenUnavailableCheckbox.frame = checkboxFrame;
    self.hideWhenUnavailableCheckbox.autoresizingMask = NSViewMinYMargin;

    NSTextField *sectionLabel = [NSTextField labelWithString:NSLocalizedString(@"Volume curve:", nil)];
    sectionLabel.font = [NSFont boldSystemFontOfSize:[NSFont smallSystemFontSize]];
    sectionLabel.frame = NSMakeRect(left, 130.0, 220.0, 17.0);
    sectionLabel.autoresizingMask = NSViewMinYMargin;
    [contentView addSubview:sectionLabel];

    NSTextField *valueLabel = [NSTextField labelWithString:@"γ = 1.00"];
    valueLabel.alignment = NSTextAlignmentRight;
    valueLabel.frame = NSMakeRect(contentWidth - left - 90.0, 130.0, 90.0, 17.0);
    valueLabel.autoresizingMask = NSViewMinYMargin | NSViewMinXMargin;
    [contentView addSubview:valueLabel];
    self.volumeCurveValueLabel = valueLabel;

    NSSlider *slider = [NSSlider sliderWithValue:kVolumeCurveGammaDefault
                                       minValue:kVolumeCurveGammaMin
                                       maxValue:kVolumeCurveGammaMax
                                          target:self
                                          action:@selector(volumeCurveChanged:)];
    slider.frame = NSMakeRect(left, 98.0, width, 24.0);
    slider.autoresizingMask = NSViewMinYMargin | NSViewWidthSizable;
    slider.numberOfTickMarks = 5;
    slider.enabled = NO;
    slider.toolTip = NSLocalizedString(
        @"Shape of the volume response. Below 1 gives finer control at high volumes, "
        @"1 is the original linear-in-dB curve, above 1 gives finer control at low volumes.",
        nil);
    [contentView addSubview:slider];
    self.volumeCurveSlider = slider;

    VolumeCurvePreviewView *preview = [[VolumeCurvePreviewView alloc] initWithFrame:NSMakeRect(left, 12.0, width, 76.0)];
    preview.autoresizingMask = NSViewMinYMargin | NSViewWidthSizable;
    [contentView addSubview:preview];
    self.volumeCurvePreview = preview;
}

- (void)updateVolumeCurveValueLabel {
    self.volumeCurveValueLabel.stringValue =
        [NSString stringWithFormat:@"γ = %.2f", self.volumeCurveSlider.doubleValue];
}

- (void)keepTryingToInitializeUntilSuccess {
    // For some reason, sometimes when the app launches right after the system boots we'll get bunk data
    // for all of the connected audio devices. If that happens then we'll try to initialize again after
    // a few seconds. We're initially waiting three seconds because that seems to work. Using less time
    // can actually cause the audio server to crash, so we want to be careful not to query it too often!
    bool success = [self initialize];
    
    if (!success) {
        NSLog(@"NB: failed to initialize, will try again in a sec...");
        [NSTimer scheduledTimerWithTimeInterval:initializationAttemptInterval target:self selector:@selector(keepTryingToInitializeUntilSuccess) userInfo:nil repeats:NO];
        // Increase the length of time between attempting to initialize by two seconds each time, just to be safe:
        initializationAttemptInterval += 2;
    }
}

- (bool)initialize {
    if (![self setCurrentProcessAsConfigurator]) {
        return false;
    }
    
    self.deviceNameTextField.stringValue = [self currentDeviceName];
    
    if (![self proxyAudioDeviceAvailable]) {
        // It's expected that we won't find the Proxy Audio Device if it's not installed, so this
        // technically isn't a failure case where we'd want to try initializing the app again.
        return true;
    }
    
    if (![self refreshOutputDevices]) {
        return false;
    }
    
    if (![self setupListenerForCurrentAudioDevices]) {
        return false;
    }
    
    [self.bufferSizeComboBox selectItemWithObjectValue:[self currentOutputDeviceBufferFrameSize]];
    self.deviceNameTextField.enabled = YES;
    self.outputDeviceComboBox.enabled = YES;
    self.bufferSizeComboBox.enabled = YES;
    self.proxiedDeviceIsActiveRadioButton.enabled = YES;
    self.userIsActiveRadioButton.enabled = YES;
    self.alwaysRadioButton.enabled = YES;
    self.hideWhenUnavailableCheckbox.enabled = YES;
    self.hideWhenUnavailableCheckbox.state =
        [self currentHideWhenUnavailable] ? NSControlStateValueOn : NSControlStateValueOff;

    self.volumeCurveSlider.enabled = YES;
    Float64 gamma = [self currentVolumeCurveGamma];
    self.volumeCurveSlider.doubleValue = gamma;
    self.volumeCurvePreview.gamma = gamma;
    [self updateVolumeCurveValueLabel];
    self.volumeCurvePreview.needsDisplay = YES;

    ProxyAudioDevice::ActiveCondition condition = [self currentOutputDeviceActiveCondition];

    if (condition == ProxyAudioDevice::ActiveCondition::proxiedDeviceActive) {
        self.proxiedDeviceIsActiveRadioButton.state = NSControlStateValueOn;
        self.userIsActiveRadioButton.state = NSControlStateValueOff;
        self.alwaysRadioButton.state = NSControlStateValueOff;
    } else if (condition == ProxyAudioDevice::ActiveCondition::userActive) {
        self.proxiedDeviceIsActiveRadioButton.state = NSControlStateValueOff;
        self.userIsActiveRadioButton.state = NSControlStateValueOn;
        self.alwaysRadioButton.state = NSControlStateValueOff;
    } else {
        self.proxiedDeviceIsActiveRadioButton.state = NSControlStateValueOff;
        self.userIsActiveRadioButton.state = NSControlStateValueOff;
        self.alwaysRadioButton.state = NSControlStateValueOn;
    }

    return true;
}

- (bool)setCurrentProcessAsConfigurator {
    AudioDeviceID proxyAudioBox = AudioDevice::audioDeviceIDForBoxUID(CFSTR(kBox_UID));
    
    if (proxyAudioBox == kAudioObjectUnknown) {
        NSLog(@"Error: unable to find proxy audio device");
        // It's expected that we won't find the Proxy Audio Device if it's not installed, so this
        // technically isn't a failure case where we'd want to try initializing the app again.
        return true;
    }
    
    if (!AudioDevice::setIdentifyValue(proxyAudioBox, getpid())) {
        NSLog(@"Error: unable to set current process as configurator");
        return false;
    }
    
    return true;
}

int onDevicesChanged(AudioObjectID inObjectID,
                     UInt32 inNumberAddresses,
                     const AudioObjectPropertyAddress *inAddresses,
                     void *inClientData) {
#pragma unused(inObjectID, inNumberAddresses, inAddresses)
    dispatch_async(dispatch_get_main_queue(), ^{
        WindowDelegate *delegate = (__bridge WindowDelegate *)inClientData;
        [delegate refreshOutputDevices];
    });

    return noErr;
}

- (bool)setupListenerForCurrentAudioDevices {
    AudioObjectPropertyAddress listenerPropertyAddress = {
        kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMaster};
    OSStatus err =
        AudioObjectAddPropertyListener(kAudioObjectSystemObject, &listenerPropertyAddress, &onDevicesChanged, (__bridge_retained void *)self);

    if (err != noErr) {
        NSLog(@"Error: could not set up listener for audio devices changing");
        return false;
    }
    
    return true;
}

- (bool)proxyAudioDeviceAvailable {
    return AudioDevice::audioDeviceIDForBoxUID(CFSTR(kBox_UID)) != kAudioObjectUnknown;
}

- (NSString *)currentDeviceName {
    AudioDeviceID proxyAudioBox = AudioDevice::audioDeviceIDForBoxUID(CFSTR(kBox_UID));
    AudioDevice::setIdentifyValue(proxyAudioBox, -((SInt32)ProxyAudioDevice::ConfigType::deviceName));
    NSString *result = (__bridge_transfer NSString *)AudioDevice::copyObjectName(proxyAudioBox);
    
    return result ? result : NSLocalizedString(@"< Proxy Audio Device not found >", nil);
}

- (IBAction)deviceNameEntered:(id)sender {
#pragma unused(sender)
    NSString *newName = [self.deviceNameTextField.stringValue
        stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
    
    if (newName.length == 0) {
        self.deviceNameTextField.stringValue = [self currentDeviceName];
        return;
    }

    AudioDeviceID proxyAudioBox = AudioDevice::audioDeviceIDForBoxUID(CFSTR(kBox_UID));
    AudioDevice::setObjectName(proxyAudioBox,
                               (__bridge_retained CFStringRef)[NSString stringWithFormat:@"deviceName=%@", newName]);
}

- (AudioDeviceID)currentOutputDevice {
    AudioDeviceID proxyAudioBox = AudioDevice::audioDeviceIDForBoxUID(CFSTR(kBox_UID));
    AudioDevice::setIdentifyValue(proxyAudioBox, -((SInt32)ProxyAudioDevice::ConfigType::outputDevice));
    NSString *outputDeviceUID = (__bridge_transfer NSString *)AudioDevice::copyObjectName(proxyAudioBox);
    
    return AudioDevice::audioDeviceIDForDeviceUID((__bridge_retained CFStringRef)outputDeviceUID);
}

- (bool)refreshOutputDevices {
    bool success = false;
    [self.outputDeviceComboBox removeAllItems];
    currentDeviceList = AudioDevice::devicesWithOutputCapabilitiesThatAreNotProxyAudioDevice();
    AudioDeviceID outputDevice = [self currentOutputDevice];
    
    for (unsigned int i = 0; i < currentDeviceList.size(); ++i) {
        NSString *deviceName = (__bridge_transfer NSString *)AudioDevice::copyObjectName(currentDeviceList[i]);
        
        if (!deviceName) {
            NSLog(@"Note: got null device name for audio device with device with ID: %d", currentDeviceList[i]);
            continue;
        }
        
        [self.outputDeviceComboBox addItemWithObjectValue:deviceName];
        
        if (outputDevice == currentDeviceList[i]) {
            [self.outputDeviceComboBox selectItemAtIndex:i];
        }
        
        success = true;
    }
    
    if (!success) {
        NSLog(@"Error: failed to get any information about current output devices!");
    }
    
    return success;
}

- (IBAction)outputDeviceSelected:(id)sender {
#pragma unused(sender)
    unsigned long index = (unsigned long)self.outputDeviceComboBox.indexOfSelectedItem;
    
    if (index < 0 || index >= currentDeviceList.size()) {
        NSLog(@"Error: got invalid selection index when trying to set output device");
        return;
    }

    NSString *uid = (__bridge_transfer NSString *)AudioDevice::copyDeviceUID(currentDeviceList[index]);
    
    if (!uid) {
        NSLog(@"Error: got invalid UID when trying to set output device");
        return;
    }

    AudioDeviceID proxyAudioBox = AudioDevice::audioDeviceIDForBoxUID(CFSTR(kBox_UID));
    AudioDevice::setObjectName(proxyAudioBox,
                               (__bridge_retained CFStringRef)[NSString stringWithFormat:@"outputDevice=%@", uid]);
}

- (NSString *)currentOutputDeviceBufferFrameSize {
    AudioDeviceID proxyAudioBox = AudioDevice::audioDeviceIDForBoxUID(CFSTR(kBox_UID));
    AudioDevice::setIdentifyValue(proxyAudioBox, -((SInt32)ProxyAudioDevice::ConfigType::outputDeviceBufferFrameSize));
    NSString *result = (__bridge_transfer NSString *)AudioDevice::copyObjectName(proxyAudioBox);
    
    return result ? result : @"";
}

- (IBAction)outputDeviceBufferFrameSizeSelected:(id)sender {
#pragma unused(sender)
    NSString *newBufferFrameSizeString = self.bufferSizeComboBox.objectValueOfSelectedItem;

    if (!newBufferFrameSizeString) {
        NSLog(@"Error: got invalid buffer frame size value");
        return;
    }

    AudioDeviceID proxyAudioBox = AudioDevice::audioDeviceIDForBoxUID(CFSTR(kBox_UID));
    AudioDevice::setObjectName(
        proxyAudioBox,
        (__bridge_retained CFStringRef)
            [NSString stringWithFormat:@"outputDeviceBufferFrameSize=%@", newBufferFrameSizeString]);
}

- (ProxyAudioDevice::ActiveCondition)currentOutputDeviceActiveCondition {
    AudioDeviceID proxyAudioBox = AudioDevice::audioDeviceIDForBoxUID(CFSTR(kBox_UID));
    AudioDevice::setIdentifyValue(proxyAudioBox, -((SInt32)ProxyAudioDevice::ConfigType::deviceActiveCondition));
    NSString *result = (__bridge_transfer NSString *)AudioDevice::copyObjectName(proxyAudioBox);

    return (ProxyAudioDevice::ActiveCondition)[result intValue];
}

- (void)setCurrentOutputDeviceActiveCondition:(ProxyAudioDevice::ActiveCondition)condition {
    AudioDeviceID proxyAudioBox = AudioDevice::audioDeviceIDForBoxUID(CFSTR(kBox_UID));
    AudioDevice::setObjectName(
        proxyAudioBox,
        (__bridge_retained CFStringRef)
            [NSString stringWithFormat:@"outputDeviceActiveCondition=%d", condition]);
}

- (IBAction)proxiedDeviceIsActiveConditionSelected:(id)sender {
    #pragma unused(sender)
    self.alwaysRadioButton.state = NSControlStateValueOff;
    self.userIsActiveRadioButton.state = NSControlStateValueOff;
    [self setCurrentOutputDeviceActiveCondition:ProxyAudioDevice::ActiveCondition::proxiedDeviceActive];
}

- (IBAction)userIsActiveConditionSelected:(id)sender {
    #pragma unused(sender)
    self.alwaysRadioButton.state = NSControlStateValueOff;
    self.proxiedDeviceIsActiveRadioButton.state = NSControlStateValueOff;
    [self setCurrentOutputDeviceActiveCondition:ProxyAudioDevice::ActiveCondition::userActive];
}

- (IBAction)alwaysConditionSelected:(id)sender {
    #pragma unused(sender)
    self.proxiedDeviceIsActiveRadioButton.state = NSControlStateValueOff;
    self.userIsActiveRadioButton.state = NSControlStateValueOff;
    [self setCurrentOutputDeviceActiveCondition:ProxyAudioDevice::ActiveCondition::always];
}

// The driver reports this preference as "1" or "0" (see copyConfigurationValue
// in ProxyAudioDevice.cpp). Anything non-"1" is treated as false.
- (bool)currentHideWhenUnavailable {
    AudioDeviceID proxyAudioBox = AudioDevice::audioDeviceIDForBoxUID(CFSTR(kBox_UID));
    AudioDevice::setIdentifyValue(proxyAudioBox, -((SInt32)ProxyAudioDevice::ConfigType::deviceHideWhenUnavailable));
    NSString *result = (__bridge_transfer NSString *)AudioDevice::copyObjectName(proxyAudioBox);

    return [result intValue] != 0;
}

- (void)setCurrentHideWhenUnavailable:(bool)hide {
    AudioDeviceID proxyAudioBox = AudioDevice::audioDeviceIDForBoxUID(CFSTR(kBox_UID));
    AudioDevice::setObjectName(
        proxyAudioBox,
        (__bridge_retained CFStringRef)
            [NSString stringWithFormat:@"outputDeviceHideWhenUnavailable=%d", hide ? 1 : 0]);
}

- (IBAction)hideWhenUnavailableToggled:(id)sender {
    #pragma unused(sender)
    [self setCurrentHideWhenUnavailable:(self.hideWhenUnavailableCheckbox.state == NSControlStateValueOn)];
}

// The driver reports this preference as a float (see copyConfigurationValue
// in ProxyAudioDevice.cpp). Anything outside the supported range falls back to
// the default.
- (Float64)currentVolumeCurveGamma {
    AudioDeviceID proxyAudioBox = AudioDevice::audioDeviceIDForBoxUID(CFSTR(kBox_UID));
    AudioDevice::setIdentifyValue(proxyAudioBox, -((SInt32)ProxyAudioDevice::ConfigType::volumeCurveGamma));
    NSString *result = (__bridge_transfer NSString *)AudioDevice::copyObjectName(proxyAudioBox);

    double value = [result doubleValue];
    if (value < kVolumeCurveGammaMin || value > kVolumeCurveGammaMax) {
        value = kVolumeCurveGammaDefault;
    }
    return value;
}

- (IBAction)volumeCurveChanged:(id)sender {
    #pragma unused(sender)
    Float64 gamma = self.volumeCurveSlider.doubleValue;

    AudioDeviceID proxyAudioBox = AudioDevice::audioDeviceIDForBoxUID(CFSTR(kBox_UID));
    AudioDevice::setObjectName(proxyAudioBox,
                               (__bridge_retained CFStringRef)[NSString stringWithFormat:@"volumeCurveGamma=%.2f", gamma]);

    self.volumeCurvePreview.gamma = gamma;
    self.volumeCurvePreview.needsDisplay = YES;
    [self updateVolumeCurveValueLabel];
}

@end
