// Haptic taps (UIKit feedback generator) for touch feedback: a selection box starting, units selected.
// Called from the touch code (ios/touch_ios.inc), which runs on whichever thread pumps SDL events; UIKit
// feedback generators belong to the main thread, so the tap is sent there. iPads and the simulator have no
// Taptic Engine: the calls do nothing there.
#import <UIKit/UIKit.h>
#include <SDL3/SDL.h>

static UIImpactFeedbackGenerator *gen;

static UIImpactFeedbackGenerator *generator(void) {
    if (gen) return gen;
    UIWindow *w = nil;
    int n = 0;
    SDL_Window **ws = SDL_GetWindows(&n);
    if (ws && n > 0)
        w = (__bridge UIWindow *)SDL_GetPointerProperty(SDL_GetWindowProperties(ws[0]), SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, NULL);
    SDL_free(ws);
    if (@available(iOS 17.5, *)) {
        if (w) gen = [UIImpactFeedbackGenerator feedbackGeneratorWithStyle:UIImpactFeedbackStyleHeavy forView:w];
    }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    if (!gen) gen = [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleHeavy];
#pragma clang diagnostic pop
    return gen;
}

static void on_main(void (^b)(void)) {
    if ([NSThread isMainThread]) b();
    else dispatch_async(dispatch_get_main_queue(), b);
}

// a light tap; intensity 0..1
void ios_haptic(float intensity) {
    on_main(^{
        UIImpactFeedbackGenerator *g = generator();
        [g impactOccurredWithIntensity:intensity];
        [g prepare];
    });
}

// wake the Taptic Engine ahead of a likely tap (a finger landed), so it isn't late
void ios_haptic_prepare(void) {
    on_main(^{ [generator() prepare]; });
}
