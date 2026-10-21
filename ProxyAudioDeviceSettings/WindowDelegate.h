#import <Cocoa/Cocoa.h>

NS_ASSUME_NONNULL_BEGIN

@class VolumeCurvePreviewView;

@interface WindowDelegate : NSObject
@property(nonatomic, strong) IBOutlet NSTextField *deviceNameTextField;
@property(nonatomic, strong) IBOutlet NSComboBox *outputDeviceComboBox;
@property(nonatomic, strong) IBOutlet NSComboBox *bufferSizeComboBox;
@property(nonatomic, strong) IBOutlet NSButton *proxiedDeviceIsActiveRadioButton;
@property(nonatomic, strong) IBOutlet NSButton *userIsActiveRadioButton;
@property(nonatomic, strong) IBOutlet NSButton *alwaysRadioButton;
@property(nonatomic, strong) IBOutlet NSButton *hideWhenUnavailableCheckbox;
// Volume curve controls are created programmatically (see setupVolumeCurveControls)
@property(nonatomic, strong) NSSlider *volumeCurveSlider;
@property(nonatomic, strong) NSTextField *volumeCurveValueLabel;
@property(nonatomic, strong) VolumeCurvePreviewView *volumeCurvePreview;

- (void)awakeFromNib;
- (IBAction)deviceNameEntered:(id)sender;
- (IBAction)outputDeviceSelected:(id)sender;
- (IBAction)outputDeviceBufferFrameSizeSelected:(id)sender;
- (IBAction)proxiedDeviceIsActiveConditionSelected:(id)sender;
- (IBAction)userIsActiveConditionSelected:(id)sender;
- (IBAction)alwaysConditionSelected:(id)sender;
- (IBAction)hideWhenUnavailableToggled:(id)sender;
- (IBAction)volumeCurveChanged:(id)sender;

@end

NS_ASSUME_NONNULL_END
