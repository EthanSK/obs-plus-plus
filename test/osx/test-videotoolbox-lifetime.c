/* Standalone ownership regression test; includes the real encoder implementation.
 * No OBS instance, capture source, network output, or hardware encoder is started. */
#include <obs-module.h>
#include <VideoToolbox/VideoToolbox.h>
#include <CoreMedia/CoreMedia.h>
#include <stdio.h>
#include <spawn.h>
#include <mach-o/dyld.h>
#include <sys/wait.h>
#include <unistd.h>

static CVPixelBufferPoolRef test_pool;
static CVPixelBufferRef submitted_buffer;
static OSStatus submission_status;
static bool native_submission;

static CVPixelBufferPoolRef test_get_pool(VTCompressionSessionRef session)
{
	(void)session;
	return native_submission ? VTCompressionSessionGetPixelBufferPool(session) : test_pool;
}

static OSStatus test_submit(VTCompressionSessionRef session, CVImageBufferRef image, CMTime pts, CMTime duration,
			    CFDictionaryRef properties, void *source, VTEncodeInfoFlags *flags)
{
	if (native_submission)
		return VTCompressionSessionEncodeFrame(session, image, pts, duration, properties, source, flags);
	(void)session;
	(void)pts;
	(void)duration;
	(void)properties;
	(void)source;
	(void)flags;
	submitted_buffer =
		(CVPixelBufferRef)CFRetain(image); // Keep the object inspectable after OBS releases its ownership.
	return submission_status;
}

static OSStatus test_create(CFAllocatorRef allocator, int32_t width, int32_t height, CMVideoCodecType codec,
			    CFDictionaryRef specification, CFDictionaryRef attributes,
			    CFAllocatorRef compressed_allocator, VTCompressionOutputCallback callback, void *refcon,
			    VTCompressionSessionRef *session)
{
	(void)allocator;
	(void)width;
	(void)height;
	(void)codec;
	(void)specification;
	(void)attributes;
	(void)compressed_allocator;
	(void)callback;
	(void)refcon;
	*session = NULL;
	return kVTAllocationFailedErr;
}

static const char *test_codec(const obs_encoder_t *encoder)
{
	(void)encoder;
	return "h264";
}

static const char *test_name(const obs_encoder_t *encoder)
{
	(void)encoder;
	return "ownership-test";
}

#define VTCompressionSessionGetPixelBufferPool test_get_pool
#define VTCompressionSessionEncodeFrame test_submit
#define VTCompressionSessionCreate test_create
#define obs_encoder_get_codec test_codec
#define obs_encoder_get_name test_name
#include "../../plugins/mac-videotoolbox/encoder.c"
#undef VTCompressionSessionGetPixelBufferPool
#undef VTCompressionSessionEncodeFrame
#undef VTCompressionSessionCreate
#undef obs_encoder_get_codec
#undef obs_encoder_get_name

static int failures;

static void check(bool passed, const char *name)
{
	printf("%s: %s\n", passed ? "PASS" : "FAIL", name);
	failures += !passed;
}

static CMSampleBufferRef make_sample(void)
{
	CMSampleBufferRef sample = NULL;
	assert(CMSampleBufferCreateReady(NULL, NULL, NULL, 0, 0, NULL, 0, NULL, &sample) == noErr);
	return sample;
}

static void test_full_queue(void)
{
	CMSimpleQueueRef queue = NULL;
	assert(CMSimpleQueueCreate(NULL, 1, &queue) == noErr);
	CMSampleBufferRef first = make_sample();
	CMSampleBufferRef rejected = make_sample();
	assert(CMSimpleQueueEnqueue(queue, first) == noErr);
	CVPixelBufferRef source = NULL;
	assert(CVPixelBufferCreate(NULL, 16, 16, kCVPixelFormatType_32BGRA, NULL, &source) == noErr);
	CFRetain(
		source); // The old callback consumes one reference; keep another so both versions can be tested safely.
	sample_encoded_callback(queue, source, noErr, 0, rejected);
	check(CFGetRetainCount(rejected) == 1, "full queue releases the rejected sample");
	CFIndex count = CFGetRetainCount(rejected);
	while (count--)
		CFRelease(rejected);
	count = CFGetRetainCount(source);
	while (count--)
		CFRelease(source);
	assert(CMSimpleQueueDequeue(queue) == first);
	CFRelease(first);
	CFRelease(queue);
}

static void test_destroy_queue(void)
{
	struct vt_encoder *encoder = bzalloc(sizeof(*encoder));
	assert(CMSimpleQueueCreate(NULL, 2, &encoder->queue) == noErr);
	CMSimpleQueueRef queue = (CMSimpleQueueRef)CFRetain(encoder->queue);
	CMSampleBufferRef sample = make_sample();
	CFRetain(sample);
	assert(CMSimpleQueueEnqueue(queue, sample) == noErr);
	vt_destroy(encoder);
	check(CMSimpleQueueGetCount(queue) == 0, "shutdown drains queued samples");
	check(CFGetRetainCount(sample) == 1, "shutdown releases queued sample ownership");
	check(CFGetRetainCount(queue) == 1, "shutdown releases its queue");
	CMSimpleQueueDequeue(queue);
	CFIndex count = CFGetRetainCount(sample);
	while (count--)
		CFRelease(sample);
	count = CFGetRetainCount(queue);
	while (count--)
		CFRelease(queue);
}

static void test_missing_pool(void)
{
	struct vt_encoder encoder = {0};
	CVPixelBufferRef buffer = NULL;
	check(!get_cached_pixel_buffer(&encoder, &buffer), "missing pixel buffer pool reports failure");
	check(buffer == NULL, "missing pixel buffer pool produces no buffer");
}

static void test_submission(OSStatus status)
{
	int size = 16;
	int format = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
	CFNumberRef dimension = CFNumberCreate(NULL, kCFNumberIntType, &size);
	CFNumberRef pixel_format = CFNumberCreate(NULL, kCFNumberIntType, &format);
	const void *keys[] = {kCVPixelBufferWidthKey, kCVPixelBufferHeightKey, kCVPixelBufferPixelFormatTypeKey};
	const void *values[] = {dimension, dimension, pixel_format};
	CFDictionaryRef attributes = CFDictionaryCreate(NULL, keys, values, 3, &kCFTypeDictionaryKeyCallBacks,
							&kCFTypeDictionaryValueCallBacks);
	assert(CVPixelBufferPoolCreate(NULL, NULL, attributes, &test_pool) == noErr);
	CFRelease(attributes);
	CFRelease(pixel_format);
	CFRelease(dimension);

	struct vt_encoder encoder = {.fps_num = 30, .fps_den = 1, .colorspace = VIDEO_CS_709};
	assert(CMSimpleQueueCreate(NULL, 2, &encoder.queue) == noErr);
	struct encoder_frame frame = {0};
	struct encoder_packet packet = {0};
	bool received = false;
	submission_status = status;
	check(vt_encode(&encoder, &frame, &packet, &received) == (status == noErr),
	      status == noErr ? "accepted asynchronous submission succeeds" : "rejected submission reports failure");
	check(CFGetRetainCount(submitted_buffer) == 1,
	      status == noErr ? "accepted submission releases caller pixel buffer ownership"
			      : "rejected submission releases caller pixel buffer ownership");
	CFIndex count = CFGetRetainCount(submitted_buffer);
	while (count--)
		CFRelease(submitted_buffer);
	submitted_buffer = NULL;
	CFRelease(encoder.queue);
	CFRelease(test_pool);
	test_pool = NULL;
}

static void test_creation_failure(void)
{
	fflush(NULL);
	char executable[4096];
	uint32_t size = sizeof(executable);
	assert(_NSGetExecutablePath(executable, &size) == 0);
	char *arguments[] = {executable, "--creation-failure", NULL};
	extern char **environ;
	pid_t child;
	assert(posix_spawn(&child, executable, NULL, NULL, arguments, environ) ==
	       0); // Use exec, not unsafe post-Cocoa fork.
	int status;
	assert(waitpid(child, &status, 0) == child);
	check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "session creation failure returns the original error");
}

static void test_native_encoding(void)
{
	const void *keys[] = {kVTVideoEncoderSpecification_EncoderID};
	const void *values[] = {CFSTR("com.apple.videotoolbox.videoencoder.h264")};
	CFDictionaryRef specification = CFDictionaryCreate(NULL, keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
							   &kCFTypeDictionaryValueCallBacks);
	native_submission = true;
	for (int cycle = 0; cycle < 6; cycle++) {
		struct vt_encoder *encoder = bzalloc(sizeof(*encoder));
		encoder->fps_num = 30;
		encoder->fps_den = 1;
		encoder->colorspace = VIDEO_CS_709;
		encoder->codec_type = kCMVideoCodecType_H264;
		encoder->vt_pix_fmt = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
		encoder->width = 64;
		encoder->height = 64;
		assert(CMSimpleQueueCreate(NULL, 100, &encoder->queue) == noErr);
		CFDictionaryRef pixels = create_pixbuf_spec(encoder);
		OSStatus result = VTCompressionSessionCreate(NULL, 64, 64, kCMVideoCodecType_H264, specification,
							     pixels, NULL, sample_encoded_callback, encoder->queue,
							     &encoder->session);
		CFRelease(pixels);
		assert(result == noErr);
		uint8_t luma[64 * 64] = {0};
		uint8_t chroma[64 * 32];
		memset(chroma, 128, sizeof(chroma));
		struct encoder_frame frame = {.data = {luma, chroma}, .linesize = {64, 64}};
		struct encoder_packet packet = {0};
		bool received = false;
		for (int i = 0; i < 12; i++) {
			frame.pts = i;
			assert(vt_encode(encoder, &frame, &packet, &received));
		}
		if (cycle % 2 == 0) {
			assert(VTCompressionSessionCompleteFrames(encoder->session, kCMTimeInvalid) == noErr);
			check(received || CMSimpleQueueGetCount(encoder->queue) > 0,
			      "native software encoder produced samples");
		}
		CMSimpleQueueRef queue = (CMSimpleQueueRef)CFRetain(encoder->queue);
		vt_destroy(encoder);
		check(CFGetRetainCount(queue) == 1 && CMSimpleQueueGetCount(queue) == 0,
		      cycle % 2 == 0 ? "native completed session releases queue"
				     : "native pending session releases queue");
		CFRelease(queue);
	}
	native_submission = false;
	CFRelease(specification);
}

int main(int argc, char **argv)
{
	if (argc == 2 && strcmp(argv[1], "--creation-failure") == 0) {
		struct vt_encoder encoder = {.vt_encoder_id = "invalid-for-test", .width = 16, .height = 16};
		OSStatus status = create_encoder(&encoder);
		return status == kVTAllocationFailedErr && encoder.session == NULL ? 0 : 1;
	}
	test_full_queue();
	test_destroy_queue();
	test_missing_pool();
	test_submission(kVTVideoEncoderMalfunctionErr);
	test_submission(noErr);
	test_creation_failure();
	test_native_encoding();
	printf("%d ownership failures\n", failures);
	return failures != 0;
}
