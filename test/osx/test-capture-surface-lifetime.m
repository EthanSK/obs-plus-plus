/* Exercise the actual capture ticks with real IOSurfaces, without starting screen capture or a renderer. */
#include <obs-module.h>
#include <IOSurface/IOSurface.h>
#include <Cocoa/Cocoa.h>
#include <util/platform.h>

static uint64_t test_time_ns = 1000000000ULL;
static int texture_error_logs;
static char last_texture_log[1024];

static uint64_t test_time(void)
{
    return test_time_ns;
}
static void test_graphics(void) {}
static gs_texture_t *test_create_texture(void *surface)
{
    return NULL;
}
static bool test_rebind_texture(gs_texture_t *texture, void *surface)
{
    return false;
}
static const char *test_source_name(const obs_source_t *source)
{
    return "diagnostic-test";
}

static void capture_log(int level, const char *format, va_list args, void *parameter)
{
    vsnprintf(last_texture_log, sizeof(last_texture_log), format, args);
    if (strstr(last_texture_log, "texture_failures="))
        texture_error_logs++;
}

static bool test_source_showing(const obs_source_t *source)
{
    (void) source;
    return true;
}

#define obs_source_showing test_source_showing
#define obs_source_get_name test_source_name
#define obs_enter_graphics test_graphics
#define obs_leave_graphics test_graphics
#define gs_texture_create_from_iosurface test_create_texture
#define gs_texture_rebind_iosurface test_rebind_texture
#define os_gettime_ns test_time
#ifdef TEST_LEGACY_CAPTURE
#include "../../plugins/mac-capture/mac-display-capture.m"
#else
#include "../../plugins/mac-capture/mac-sck-video-capture.m"
#endif
#undef obs_source_showing
#undef obs_source_get_name
#undef obs_enter_graphics
#undef obs_leave_graphics
#undef gs_texture_create_from_iosurface
#undef gs_texture_rebind_iosurface
#undef os_gettime_ns

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
#ifndef TEST_LEGACY_CAPTURE
        base_set_log_handler(capture_log, NULL);
        struct screen_capture failed_capture = {.display = 99};
        pthread_mutex_init(&failed_capture.mutex, NULL);
        for (int i = 0; i < 100; i++) {
            failed_capture.current = make_surface();
            IOSurfaceIncrementUseCount(failed_capture.current);
            sck_video_capture_tick(&failed_capture, 0);
        }
        passed = failed_capture.texture_failures == 100 && texture_error_logs == 1;
        printf("%s: repeated texture failures retain counts without flooding the log\n", passed ? "PASS" : "FAIL");
        failures += !passed;
        test_time_ns += 30000000000ULL;
        failed_capture.current = make_surface();
        IOSurfaceIncrementUseCount(failed_capture.current);
        sck_video_capture_tick(&failed_capture, 0);
        passed = texture_error_logs == 2 && strstr(last_texture_log, "source='diagnostic-test'") &&
                 strstr(last_texture_log, "display=99") && strstr(last_texture_log, "size=16x16") &&
                 strstr(last_texture_log, "texture_failures=101");
        printf("%s: texture log identifies source and surface after the 30-second limit\n", passed ? "PASS" : "FAIL");
        failures += !passed;
        IOSurfaceDecrementUseCount(failed_capture.prev);
        CFRelease(failed_capture.prev);
        pthread_mutex_destroy(&failed_capture.mutex);
#endif
        return failures != 0;
    }
}
