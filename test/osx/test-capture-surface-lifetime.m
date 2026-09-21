/* Exercise the actual capture ticks with real IOSurfaces, without starting screen capture or a renderer. */
#include <obs-module.h>
#include <IOSurface/IOSurface.h>
#include <Cocoa/Cocoa.h>

static bool test_source_showing(const obs_source_t *source)
{
    (void) source;
    return true;
}

#define obs_source_showing test_source_showing
#ifdef TEST_LEGACY_CAPTURE
#include "../../plugins/mac-capture/mac-display-capture.m"
#else
#include "../../plugins/mac-capture/mac-sck-video-capture.m"
#endif
#undef obs_source_showing

OBS_DECLARE_MODULE()

static IOSurfaceRef make_surface(void)
{
    NSDictionary *properties = @{
        (NSString *) kIOSurfaceWidth: @16,
        (NSString *) kIOSurfaceHeight: @16,
        (NSString *) kIOSurfaceBytesPerElement: @4,
        (NSString *) kIOSurfacePixelFormat: @(kCVPixelFormatType_32BGRA),
    };
    IOSurfaceRef surface = IOSurfaceCreate((CFDictionaryRef) properties);
    assert(surface);
    return surface;
}

int main(void)
{
    @autoreleasepool {
        int failures = 0;
        IOSurfaceRef surface = make_surface();
        CFIndex baseline = CFGetRetainCount(surface);
        CFRetain(surface);
        CFRetain(surface);
        IOSurfaceIncrementUseCount(surface);
        IOSurfaceIncrementUseCount(surface);
#ifdef TEST_LEGACY_CAPTURE
        const char *name = "legacy capture";
        struct display_capture capture = {.current = surface, .prev = surface};
        pthread_mutex_init(&capture.mutex, NULL);
        display_capture_video_tick(&capture, 0);
        assert(capture.current == NULL && capture.prev == surface);
        pthread_mutex_destroy(&capture.mutex);
#else
        const char *name = "ScreenCaptureKit";
        struct screen_capture capture = {.current = surface, .prev = surface};
        pthread_mutex_init(&capture.mutex, NULL);
        sck_video_capture_tick(&capture, 0);
        assert(capture.current == NULL && capture.prev == surface);
        pthread_mutex_destroy(&capture.mutex);
#endif
        bool passed = CFGetRetainCount(surface) == baseline + 1;
        printf("%s: %s duplicate surface releases the replaced ownership\n", passed ? "PASS" : "FAIL", name);
        failures += !passed;
        IOSurfaceDecrementUseCount(surface);
        CFRelease(surface);
        passed = !IOSurfaceIsInUse(surface);
        printf("%s: %s surface is reusable after cleanup\n", passed ? "PASS" : "FAIL", name);
        failures += !passed;
        while (CFGetRetainCount(surface) > baseline) {
            IOSurfaceDecrementUseCount(surface);
            CFRelease(surface);
        }
        CFRelease(surface);
        return failures != 0;
    }
}
