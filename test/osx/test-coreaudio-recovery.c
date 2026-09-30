/* Exercise real device-selection and AudioUnit state code without touching live audio devices. */
#include <obs-module.h>
#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudio.h>
#include <util/apple/cfstring-utils.h>
#include <util/threading.h>
#include <pthread.h>

static bool device_present, conversion_fails;
static int name_queries, starts, stops;
static OSStatus start_result;
static CFStringRef device_name;
static bool create_fails;
static int joined_threads;
static os_event_t *worker_finished;
static int test_thread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
static int test_thread_join(pthread_t, void **);

static OSStatus test_property(AudioObjectID id, const AudioObjectPropertyAddress *address, UInt32 qualifier_size,
			    const void *qualifier, UInt32 *size, void *data)
{
	(void)id;
	(void)qualifier_size;
	(void)qualifier;
	(void)size;
	if (address->mSelector == kAudioHardwarePropertyDeviceForUID) {
		AudioValueTranslation *translation = data;
		*(AudioDeviceID *)translation->mOutputData = device_present ? 42 : kAudioObjectUnknown;
		return noErr;
	}
	if (address->mSelector == kAudioHardwarePropertyDefaultInputDevice) {
		*(AudioDeviceID *)data = device_present ? 42 : kAudioObjectUnknown;
		return noErr;
	}
	if (address->mSelector == kAudioDevicePropertyDeviceNameCFString) {
		name_queries++;
		*(CFStringRef *)data = CFRetain(device_name);
		return noErr;
	}
	return kAudioHardwareUnknownPropertyError;
}

static char *test_copy_name(CFStringRef string, CFStringEncoding encoding)
{
	return conversion_fails ? NULL : cfstr_copy_cstr(string, encoding);
}

static OSStatus test_start(AudioUnit unit)
{
	(void)unit;
	starts++;
	return start_result;
}

static OSStatus test_stop(AudioUnit unit)
{
	(void)unit;
	stops++;
	return noErr;
}

#define AudioObjectGetPropertyData test_property
#define AudioOutputUnitStart test_start
#define AudioOutputUnitStop test_stop
#define cfstr_copy_cstr test_copy_name
#define pthread_create test_thread_create
#define pthread_join test_thread_join
#include "../../plugins/mac-capture/audio-device-enum.c"
#include "../../plugins/mac-capture/mac-audio.c"
#undef AudioObjectGetPropertyData
#undef AudioOutputUnitStart
#undef AudioOutputUnitStop
#undef cfstr_copy_cstr
#undef pthread_create
#undef pthread_join

OBS_DECLARE_MODULE()

const char *obs_module_text(const char *text) { return text; }

static void *test_finished_worker(void *data)
{
	struct coreaudio_data *capture = data;
	capture->reconnecting = false;
	os_event_signal(worker_finished);
	return NULL;
}

static int test_thread_create(pthread_t *thread, const pthread_attr_t *attributes, void *(*function)(void *), void *data)
{
	(void)function;
	assert(((struct coreaudio_data *)data)->reconnecting);
	return create_fails ? EAGAIN : pthread_create(thread, attributes, test_finished_worker, data);
}

static int test_thread_join(pthread_t thread, void **result)
{
	joined_threads++;
	return pthread_join(thread, result);
}

int main(void)
{
	/* Confirm the native HAL contract independently of the deterministic missing-device stub. */
	CFStringRef missing_uid = CFSTR("OBS++-regression-test-nonexistent-device");
	AudioDeviceID id = 123;
	AudioValueTranslation translation = {&missing_uid, sizeof(missing_uid), &id, sizeof(id)};
	AudioObjectPropertyAddress address = {kAudioHardwarePropertyDeviceForUID, kAudioObjectPropertyScopeGlobal,
					     kAudioObjectPropertyElementMain};
	UInt32 size = sizeof(translation);
	OSStatus result = AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, NULL, &size, &translation);
	assert(result == noErr && id == kAudioObjectUnknown);
	puts("PASS: native HAL returns success plus unknown ID for an absent UID");

	struct coreaudio_data capture = {.device_uid = bstrdup("missing-device"), .input = true};
	for (int i = 0; i < 100; i++) {
		assert(!find_device_id_by_uid(&capture));
		assert(capture.device_id == kAudioObjectUnknown);
	}
	assert(name_queries == 0);
	puts("PASS: absent devices are rejected before retrying a name query");
	bfree(capture.device_uid);
	capture.device_uid = bstrdup("default");
	assert(!find_device_id_by_uid(&capture));
	device_present = true;
	assert(find_device_id_by_uid(&capture) && capture.device_id == 42);
	puts("PASS: missing default input remains absent and a reconnected input resolves normally");

	device_name = CFStringCreateWithFormat(NULL, NULL, CFSTR("Test device %d"), 42);
	CFIndex baseline = CFGetRetainCount(device_name);
	conversion_fails = true;
	assert(!coreaudio_get_device_name(&capture));
	assert(CFGetRetainCount(device_name) == baseline);
	conversion_fails = false;
	assert(coreaudio_get_device_name(&capture));
	assert(CFGetRetainCount(device_name) == baseline);
	puts("PASS: device-name ownership is balanced on conversion failure and success");

	assert(coreaudio_start(&capture) && capture.active);
	assert(coreaudio_start(&capture) && starts == 1);
	coreaudio_stop(&capture);
	coreaudio_stop(&capture);
	assert(!capture.active && stops == 1);
	start_result = -1;
	assert(!coreaudio_start(&capture) && !capture.active);
	puts("PASS: AudioUnit start/stop state matches success and prevents duplicate calls");
	CFRelease(device_name);
	bfree(capture.device_name);
	bfree(capture.device_uid);
	struct coreaudio_data reconnect = {0};
	os_event_init(&reconnect.exit_event, OS_EVENT_TYPE_MANUAL);
	os_event_init(&worker_finished, OS_EVENT_TYPE_AUTO);
	for (int i = 0; i < 10; i++) {
		coreaudio_begin_reconnect(&reconnect);
		os_event_wait(worker_finished);
		assert(reconnect.reconnect_thread_joinable && !reconnect.reconnecting);
		assert(joined_threads == i);
	}
	coreaudio_shutdown(&reconnect);
	assert(joined_threads == 10 && !reconnect.reconnect_thread_joinable);
	create_fails = true;
	coreaudio_begin_reconnect(&reconnect);
	assert(!reconnect.reconnecting && !reconnect.reconnect_thread_joinable);
	coreaudio_shutdown(&reconnect);
	assert(joined_threads == 10);
	os_event_destroy(worker_finished);
	os_event_destroy(reconnect.exit_event);
	puts("PASS: completed reconnect workers are reaped before replacement and shutdown; creation failure owns no thread");
	return 0;
}
