/* Exercise the real content-refresh and destructor code without starting screen capture. */
#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <objc/runtime.h>
#include <stdatomic.h>
#include <unistd.h>
#import "../../plugins/mac-capture/mac-sck-common.h"
#import "../../plugins/mac-capture/window-utils.h"

static int content_destructions;
static bool fail_refresh, defer_refresh;
static atomic_bool completion_called;
static void (^pending_completion)(SCShareableContent *, NSError *);
static int fail_output_number, output_count;
static bool fail_start, stop_after_start;

@interface TestDisplay : NSObject
- (CGDirectDisplayID)displayID;
@end
@implementation TestDisplay
- (CGDirectDisplayID)displayID { return CGMainDisplayID(); }
@end

@interface TestContent : NSObject
- (NSArray *)displays;
@end
@implementation TestContent
- (NSArray *)displays { return @[[[[TestDisplay alloc] init] autorelease]]; }
- (void)dealloc
{
    content_destructions++;
    [super dealloc];
}
@end

@interface TestStream : NSObject
@property(assign) id delegate;
- (BOOL)addStreamOutput:(id)output type:(SCStreamOutputType)type sampleHandlerQueue:(dispatch_queue_t)queue error:(NSError **)error;
- (void)startCaptureWithCompletionHandler:(void (^)(NSError *))completion;
- (void)stopCaptureWithCompletionHandler:(void (^)(NSError *))completion;
@end
@implementation TestStream
@synthesize delegate;
- (BOOL)addStreamOutput:(id)output type:(SCStreamOutputType)type sampleHandlerQueue:(dispatch_queue_t)queue error:(NSError **)error
{
    (void) output; (void) type; (void) queue;
    if (++output_count == fail_output_number) {
        *error = [NSError errorWithDomain:@"controlled-output-failure" code:17 userInfo:nil];
        return NO;
    }
    return YES;
}
- (void)startCaptureWithCompletionHandler:(void (^)(NSError *))completion
{
    completion(fail_start ? [NSError errorWithDomain:@"controlled-start-failure" code:18 userInfo:nil] : nil);
    if (stop_after_start) {
        [self.delegate stream:(SCStream *)self didStopWithError:
            [NSError errorWithDomain:@"controlled-immediate-stop" code:19 userInfo:nil]];
    }
}
- (void)stopCaptureWithCompletionHandler:(void (^)(NSError *))completion { completion(nil); }
@end

static id test_filter_init(id object, SEL selector, SCDisplay *display, NSArray *windows)
{
    (void) selector; (void) display; (void) windows;
    [object release];
    return [[NSObject alloc] init];
}

static id test_stream_init(id object, SEL selector, SCContentFilter *filter, SCStreamConfiguration *config, id delegate)
{
    (void) selector; (void) filter; (void) config;
    [object release];
    output_count = 0;
    TestStream *stream = [[TestStream alloc] init];
    stream.delegate = delegate;
    return stream;
}

static void reply_to_content_request(void (^completion)(SCShareableContent *, NSError *))
{
    if (fail_refresh) {
        completion(nil, [NSError errorWithDomain:@"regression-test" code:1 userInfo:nil]);
    } else {
        completion((SCShareableContent *) [[[TestContent alloc] init] autorelease], nil);
    }
}

static void test_content_request(id object, SEL selector, BOOL exclude_desktop, BOOL on_screen_only,
                                 void (^completion)(SCShareableContent *, NSError *))
{
    (void) object;
    (void) selector;
    (void) exclude_desktop;
    (void) on_screen_only;
    if (defer_refresh)
        pending_completion = [completion copy];
    else
        reply_to_content_request(completion);
}

static void test_graphics(void) {}
static void test_update_properties(obs_source_t *source) { (void) source; }
static int test_device_type(void) { return GS_DEVICE_OPENGL; }
static gs_effect_t *test_effect(enum obs_base_effect effect) { (void) effect; return (gs_effect_t *) 1; }
static CGDirectDisplayID test_display_settings(obs_data_t *settings) { (void) settings; return CGMainDisplayID(); }
static bool test_audio_info(struct obs_audio_info *info) {
    *info = (struct obs_audio_info){.samples_per_sec = 48000, .speakers = SPEAKERS_STEREO};
    return true;
}
static bool test_video_info(struct obs_video_info *info) {
    *info = (struct obs_video_info){.fps_num = 30, .fps_den = 1};
    return true;
}
#define obs_enter_graphics test_graphics
#define obs_leave_graphics test_graphics
#define obs_source_update_properties test_update_properties
#define gs_get_device_type test_device_type
#define obs_get_base_effect test_effect
#define get_display_migrate_settings test_display_settings
#define obs_get_audio_info test_audio_info
#define obs_get_video_info test_video_info
#include "../../plugins/mac-capture/mac-sck-common.m"
#ifdef TEST_AUDIO_CAPTURE
#include "../../plugins/mac-capture/mac-sck-audio-capture.m"
#else
#include "../../plugins/mac-capture/mac-sck-video-capture.m"
#endif
#undef obs_enter_graphics
#undef obs_leave_graphics
#undef obs_source_update_properties
#undef gs_get_device_type
#undef obs_get_base_effect
#undef get_display_migrate_settings
#undef obs_get_audio_info
#undef obs_get_video_info

OBS_DECLARE_MODULE()

static void destroy_capture(struct screen_capture *capture)
{
#ifdef TEST_AUDIO_CAPTURE
    sck_audio_capture_destroy(capture);
#else
    sck_video_capture_destroy(capture);
#endif
}

static struct screen_capture *make_capture(void)
{
    struct screen_capture *capture = bzalloc(sizeof(*capture));
    os_sem_init(&capture->shareable_content_available, 1);
    pthread_mutex_init(&capture->mutex, NULL);
    return capture;
}

static void *finish_deferred_request(void *unused)
{
    (void) unused;
    @autoreleasepool {
        usleep(5000);
        atomic_store(&completion_called, true);
        reply_to_content_request(pending_completion);
        [pending_completion release];
        pending_completion = nil;
    }
    return NULL;
}

int main(void)
{
    @autoreleasepool {
        Method method = class_getClassMethod([SCShareableContent class],
            @selector(getShareableContentExcludingDesktopWindows:onScreenWindowsOnly:completionHandler:));
        IMP original = method_setImplementation(method, (IMP) test_content_request);
        long baseline = bnum_allocs();
        struct screen_capture *capture = make_capture();
        capture->shareable_content = (SCShareableContent *) [[TestContent alloc] init];
        fail_refresh = true;
        screen_capture_build_content_list(capture, true);
        assert(capture->shareable_content == nil && content_destructions == 1);
        os_sem_wait(capture->shareable_content_available);
        os_sem_post(capture->shareable_content_available);
        destroy_capture(capture);
        assert(bnum_allocs() == baseline);
        puts("PASS: failed inventory refresh clears released content and destroys its semaphore");

        for (int failed = 0; failed < 2; failed++) {
            capture = make_capture();
            fail_refresh = failed;
            defer_refresh = true;
            atomic_store(&completion_called, false);
            screen_capture_build_content_list(capture, true);
            pthread_t worker;
            assert(pthread_create(&worker, NULL, finish_deferred_request, NULL) == 0);
            destroy_capture(capture);
            assert(atomic_load(&completion_called));
            pthread_join(worker, NULL);
            assert(bnum_allocs() == baseline);
        }
        puts("PASS: teardown waits for pending successful and failed inventory requests");
        defer_refresh = false;
        capture = make_capture();
#ifdef TEST_AUDIO_CAPTURE
        assert(init_audio_screen_stream(capture));
#else
        assert(init_screen_stream(capture));
#endif
        assert(capture->capture_failed && capture->disp == nil);
#ifdef TEST_AUDIO_CAPTURE
        obs_properties_t *properties = obs_properties_create();
        obs_property_t *restart = obs_properties_add_button2(properties, "restart", "restart", reactivate_capture, capture);
        for (int i = 0; i < 3; i++) {
            assert(reactivate_capture(properties, restart, capture));
            assert(capture->capture_failed && obs_property_enabled(restart));
        }
        obs_properties_destroy(properties);
#endif
        destroy_capture(capture);
        assert(bnum_allocs() == baseline);
        puts("PASS: unavailable display retains a retryable source and balanced cleanup");

        Method filter_method = class_getInstanceMethod([SCContentFilter class], @selector(initWithDisplay:excludingWindows:));
        IMP original_filter = method_setImplementation(filter_method, (IMP) test_filter_init);
        Method stream_method = class_getInstanceMethod([SCStream class], @selector(initWithFilter:configuration:delegate:));
        IMP original_stream = method_setImplementation(stream_method, (IMP) test_stream_init);
        fail_refresh = false;
        obs_data_t *settings = obs_data_create();
        obs_data_set_string(settings, "application", "");
        for (int failure = 1; failure <= 4; failure++) {
            @autoreleasepool {
                fail_output_number = failure <= 2 ? failure : 0;
                fail_start = failure == 3;
                stop_after_start = failure == 4;
#ifdef TEST_AUDIO_CAPTURE
                capture = sck_audio_capture_create(settings, NULL);
#else
                capture = sck_video_capture_create(settings, NULL);
#endif
                assert(capture && capture->capture_failed);
                obs_properties_t *props = obs_properties_create();
                obs_property_t *button = obs_properties_add_button2(props, "restart", "restart", reactivate_capture, capture);
                fail_output_number = 0;
                fail_start = false;
                stop_after_start = false;
                assert(reactivate_capture(props, button, capture));
                assert(!capture->capture_failed && !obs_property_enabled(button) && capture->disp);
                obs_properties_destroy(props);
                destroy_capture(capture);
            }
        }
        obs_data_release(settings);
        assert(bnum_allocs() == baseline);
        puts("PASS: initial output/start failures retain a source that recovers on a later successful retry");
        puts("PASS: a delegate stop immediately after successful start remains failed and retryable");
        method_setImplementation(filter_method, original_filter);
        method_setImplementation(stream_method, original_stream);
        method_setImplementation(method, original);
        return 0;
    }
}
