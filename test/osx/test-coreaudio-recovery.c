/* Exercise real device-selection and AudioUnit state code without touching live audio devices. */
#include <obs-module.h>
#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudio.h>
#include <util/apple/cfstring-utils.h>
#include <util/threading.h>
#include <pthread.h>
#include <stdatomic.h>
#include <dispatch/dispatch.h>

static atomic_bool device_present;
static bool conversion_fails;
static atomic_int name_queries;
static atomic_int starts, stops, disposed_units, output_calls, property_updates, joined_threads;
static OSStatus start_result;
static CFStringRef device_name;
static bool create_fails;
static os_event_t *worker_finished;
static os_event_t *start_entered, *allow_start;
static atomic_bool block_start;
static atomic_int notifications, dedup_updates, dedup_removals;
static atomic_int weak_references, source_acquires, source_releases;
static atomic_bool source_expired;
static pthread_t test_main_thread;
struct test_unit {
	AURenderCallbackStruct callback;
};
struct test_worker {
	void *(*function)(void *);
	void *data;
};
struct test_task {
	obs_task_t function;
	void *data;
};
static struct test_task ui_tasks[1024];
static size_t num_ui_tasks;
static pthread_mutex_t ui_tasks_mutex = PTHREAD_MUTEX_INITIALIZER;
static int test_thread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
static int test_thread_join(pthread_t, void **);

static OSStatus test_property_size(AudioObjectID object, const AudioObjectPropertyAddress *address,
				   UInt32 qualifier_size, const void *qualifier, UInt32 *size)
{
	(void)object;
	(void)qualifier_size;
	(void)qualifier;
	*size = address->mSelector == kAudioHardwarePropertyDevices
			? sizeof(AudioDeviceID)
			: sizeof(AudioStreamID); // Properties uses one deterministic device, not the live machine's audio hardware.
	return noErr;
}

static OSStatus test_property(AudioObjectID id, const AudioObjectPropertyAddress *address, UInt32 qualifier_size,
			      const void *qualifier, UInt32 *size, void *data)
{
	(void)id;
	(void)qualifier_size;
	(void)qualifier;
	(void)size;
	if (address->mSelector == kAudioHardwarePropertyDevices) {
		*(AudioDeviceID *)data = 42;
		return noErr;
	}
	if (address->mSelector == kAudioDevicePropertyDeviceUID) {
		*(CFStringRef *)data = CFRetain(CFSTR("BlackHole:test-device"));
		return noErr;
	}
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
	if (address->mSelector == kAudioObjectPropertyElementName) {
		*(CFStringRef *)data = CFRetain(CFSTR("Test channel"));
		return noErr;
	}
	if (address->mSelector == kAudioDevicePropertyNominalSampleRate) {
		*(Float64 *)data = 48000;
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
	if (atomic_load(&block_start)) {
		assert(!pthread_equal(pthread_self(), test_main_thread));
		os_event_signal(start_entered);
		os_event_wait(allow_start);
	}
	return start_result;
}

static OSStatus test_stop(AudioUnit unit)
{
	(void)unit;
	stops++;
	return noErr;
}

static AudioComponent test_find_component(const AudioComponentDescription *description)
{
	(void)description;
	return (AudioComponent)1;
}

static OSStatus test_new_unit(AudioComponent component, AudioComponentInstance *unit)
{
	(void)component;
	*unit = (AudioComponentInstance)bzalloc(sizeof(struct test_unit));
	return noErr;
}

static OSStatus test_dispose(AudioComponentInstance unit)
{
	assert(!pthread_equal(pthread_self(), test_main_thread));
	bfree(unit);
	disposed_units++;
	return noErr;
}

static OSStatus test_unit_operation(AudioUnit unit)
{
	(void)unit;
	assert(!pthread_equal(pthread_self(), test_main_thread));
	return noErr;
}

static OSStatus test_set(AudioUnit unit, AudioUnitPropertyID property, AudioUnitScope scope, AudioUnitElement element,
			 const void *value, UInt32 size)
{
	(void)scope;
	(void)element;
	(void)size;
	if (property == kAudioOutputUnitProperty_SetInputCallback)
		((struct test_unit *)unit)->callback = *(const AURenderCallbackStruct *)value;
	return noErr;
}

static OSStatus test_get(AudioUnit unit, AudioUnitPropertyID property, AudioUnitScope scope, AudioUnitElement element,
			 void *value, UInt32 *size)
{
	(void)unit;
	(void)scope;
	(void)element;
	(void)size;
	if (property == kAudioDevicePropertyBufferFrameSize) {
		*(UInt32 *)value = 64;
		return noErr;
	}
	if (property == kAudioUnitProperty_StreamFormat) {
		*(AudioStreamBasicDescription *)value = (AudioStreamBasicDescription){
			.mSampleRate = 48000,
			.mFormatID = kAudioFormatLinearPCM,
			.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsNonInterleaved,
			.mBitsPerChannel = 32,
			.mChannelsPerFrame = 2,
			.mBytesPerFrame = 4,
			.mBytesPerPacket = 4,
			.mFramesPerPacket = 1};
		return noErr;
	}
	return kAudioUnitErr_InvalidProperty;
}

static OSStatus test_listener(AudioObjectID object, const AudioObjectPropertyAddress *address, dispatch_queue_t queue,
			      AudioObjectPropertyListenerBlock block)
{
	(void)object;
	(void)address;
	(void)queue;
	(void)block;
	return noErr;
}

static bool test_audio_info(struct obs_audio_info *info)
{
	*info = (struct obs_audio_info){.samples_per_sec = 48000, .speakers = SPEAKERS_STEREO};
	return true;
}

static OSStatus test_render(AudioUnit unit, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *timestamp,
			    UInt32 bus, UInt32 frames, AudioBufferList *buffers)
{
	(void)unit;
	(void)flags;
	(void)timestamp;
	(void)bus;
	(void)frames;
	(void)buffers;
	return noErr;
}

static void test_output(obs_source_t *source, const struct obs_source_audio *audio)
{
	(void)source;
	(void)audio;
	output_calls++;
}

static void test_refresh(obs_source_t *source)
{
	(void)source;
	assert(pthread_equal(pthread_self(), test_main_thread));
	property_updates++;
}

static obs_weak_source_t *test_get_weak_source(obs_source_t *source)
{
	(void)source;
	weak_references++;
	return (obs_weak_source_t *)1;
}

static void test_release_weak_source(obs_weak_source_t *weak)
{
	assert(weak);
	weak_references--;
}

static obs_source_t *test_get_source(obs_weak_source_t *weak)
{
	assert(weak && pthread_equal(pthread_self(), test_main_thread));
	if (source_expired)
		return NULL;
	source_acquires++;
	return (obs_source_t *)1;
}

static void test_release_source(obs_source_t *source)
{
	assert(source && pthread_equal(pthread_self(), test_main_thread));
	source_releases++;
}

static void test_queue(enum obs_task_type type, obs_task_t function, void *data, bool wait)
{
	assert(type == OBS_TASK_UI && !wait);
	pthread_mutex_lock(&ui_tasks_mutex);
	assert(num_ui_tasks < 1024);
	ui_tasks[num_ui_tasks++] = (struct test_task){function, data};
	pthread_mutex_unlock(&ui_tasks_mutex);
}

static void test_dedup(obs_source_t *source, const char *device)
{
	(void)source;
	assert(pthread_equal(pthread_self(), test_main_thread));
	if (device)
		dedup_updates++;
	else
		dedup_removals++;
}

#define AudioObjectGetPropertyData test_property
#define AudioObjectGetPropertyDataSize test_property_size
#define AudioOutputUnitStart test_start
#define AudioOutputUnitStop test_stop
#define AudioComponentFindNext(previous, description) test_find_component(description)
#define AudioComponentInstanceNew test_new_unit
#define AudioComponentInstanceDispose test_dispose
#define AudioUnitInitialize test_unit_operation
#define AudioUnitUninitialize test_unit_operation
#define AudioUnitSetProperty test_set
#define AudioUnitGetProperty test_get
#define AudioObjectAddPropertyListenerBlock test_listener
#define AudioObjectRemovePropertyListenerBlock test_listener
#define AudioUnitRender test_render
#define obs_get_audio_info test_audio_info
#define obs_source_output_audio test_output
#define obs_source_update_properties test_refresh
#define obs_source_get_weak_source test_get_weak_source
#define obs_weak_source_release test_release_weak_source
#define obs_weak_source_get_source test_get_source
#define obs_source_release test_release_source
#define obs_queue_task test_queue
#define obs_source_audio_output_capture_device_changed test_dedup
#define cfstr_copy_cstr test_copy_name
#define pthread_create test_thread_create
#define pthread_join test_thread_join
#include "../../plugins/mac-capture/audio-device-enum.c"
#include "../../plugins/mac-capture/mac-audio.c"
#undef AudioObjectGetPropertyData
#undef AudioObjectGetPropertyDataSize
#undef AudioOutputUnitStart
#undef AudioOutputUnitStop
#undef AudioComponentFindNext
#undef AudioComponentInstanceNew
#undef AudioComponentInstanceDispose
#undef AudioUnitInitialize
#undef AudioUnitUninitialize
#undef AudioUnitSetProperty
#undef AudioUnitGetProperty
#undef AudioObjectAddPropertyListenerBlock
#undef AudioObjectRemovePropertyListenerBlock
#undef AudioUnitRender
#undef obs_get_audio_info
#undef obs_source_output_audio
#undef obs_source_update_properties
#undef obs_source_get_weak_source
#undef obs_weak_source_release
#undef obs_weak_source_get_source
#undef obs_source_release
#undef obs_queue_task
#undef obs_source_audio_output_capture_device_changed
#undef cfstr_copy_cstr
#undef pthread_create
#undef pthread_join

OBS_DECLARE_MODULE()

const char *obs_module_text(const char *text)
{
	return text;
}

static void *test_finished_worker(void *data)
{
	struct test_worker *worker = data;
	worker->function(worker->data);
	bfree(worker);
	os_event_signal(worker_finished);
	return NULL;
}

static int test_thread_create(pthread_t *thread, const pthread_attr_t *attributes, void *(*function)(void *),
			      void *data)
{
	if (create_fails)
		return EAGAIN;
	struct test_worker *worker = bzalloc(sizeof(*worker));
	*worker = (struct test_worker){function, data};
	int result = pthread_create(thread, attributes, test_finished_worker, worker);
	if (result)
		bfree(worker);
	return result;
}

static int test_thread_join(pthread_t thread, void **result)
{
	joined_threads++;
	return pthread_join(thread, result);
}

static void drain_ui_tasks(void)
{
	for (;;) {
		pthread_mutex_lock(&ui_tasks_mutex);
		if (!num_ui_tasks) {
			pthread_mutex_unlock(&ui_tasks_mutex);
			break;
		}
		struct test_task task = ui_tasks[--num_ui_tasks];
		pthread_mutex_unlock(&ui_tasks_mutex);
		task.function(task.data);
	}
}

static void wait_ready(struct coreaudio_data *capture)
{
	for (int attempt = 0; attempt < 3000; attempt++) {
		pthread_mutex_lock(&capture->mutex);
		bool ready = capture->properties_ready;
		pthread_mutex_unlock(&capture->mutex);
		if (ready)
			return;
		usleep(1000);
	}
	assert(!"worker did not become ready");
}

static obs_data_t *make_settings(const char *uid)
{
	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "device_id", uid);
	obs_data_set_bool(settings, "enable_downmix", true);
	return settings;
}

static void test_blocked_start(void)
{
	atomic_store(&block_start, true);
	os_event_reset(allow_start);
	obs_data_t *settings = make_settings("first-device");
	uint64_t before = os_gettime_ns();
	struct coreaudio_data *capture = coreaudio_create(settings, (obs_source_t *)1, true);
	assert(capture && os_gettime_ns() - before < 500000000);
	assert(os_event_timedwait(start_entered, 3000) == 0);
	int stop_count = stops;
	AudioObjectPropertyAddress change = {PROPERTY_DEFAULT_DEVICE, kAudioObjectPropertyScopeGlobal,
					     kAudioObjectPropertyElementMain};
	before = os_gettime_ns();
	notification_callback(42, 1, &change, capture);
	obs_data_set_string(settings, "device_id", "second-device");
	coreaudio_update(capture, settings);
	obs_data_set_string(settings, "device_id", "third-device");
	coreaudio_update(capture, settings);
	obs_properties_t *properties = coreaudio_properties(true, capture);
	obs_properties_destroy(properties);
	coreaudio_destroy(capture);
	assert(os_gettime_ns() - before < 500000000 && stops == stop_count);
	AudioUnitRenderActionFlags flags = 0;
	AudioTimeStamp timestamp = {0};
	int previous_output = output_calls;
	input_callback(capture, &flags, &timestamp, 1, 64, NULL);
	assert(output_calls == previous_output);
	atomic_store(&block_start, false);
	os_event_signal(allow_start);
	assert(os_event_timedwait(worker_finished, 3000) == 0);
	drain_ui_tasks();
	coreaudio_reap_workers(false);
	assert(!workers);
	obs_data_release(settings);
	puts("PASS: blocked start never blocks create/update/notification/properties/destroy; late audio is suppressed and cleanup is joined");
}

static void test_pending_updates(void)
{
	atomic_store(&block_start, true);
	os_event_reset(allow_start);
	obs_data_t *settings = make_settings("first-device");
	struct coreaudio_data *capture = coreaudio_create(settings, (obs_source_t *)1, true);
	assert(os_event_timedwait(start_entered, 3000) == 0);
	obs_data_set_string(settings, "device_id", "second-device");
	coreaudio_update(capture, settings);
	obs_data_set_string(settings, "device_id", "last-device");
	coreaudio_update(capture, settings);
	obs_data_set_string(settings, "device_id", "mutation-not-submitted");
	atomic_store(&block_start, false);
	os_event_signal(allow_start);
	wait_ready(capture);
	pthread_mutex_lock(&capture->mutex);
	assert(strcmp(capture->device_uid, "last-device") == 0);
	pthread_mutex_unlock(&capture->mutex);
	drain_ui_tasks();
	assert(property_updates > 0);
	coreaudio_destroy(capture);
	assert(os_event_timedwait(worker_finished, 3000) == 0);
	drain_ui_tasks();
	coreaudio_reap_workers(false);
	assert(!workers);
	obs_data_release(settings);
	puts("PASS: rapid updates coalesce into an independent newest-settings snapshot and refresh Properties on the UI thread");
}

static void test_shutdown_live_retry(void)
{
	device_present = false;
	obs_data_t *settings = make_settings("absent-device");
	struct coreaudio_data *capture = coreaudio_create(settings, (obs_source_t *)1, true);
	assert(capture);
	coreaudio_free_type_data(NULL);
	assert(os_event_timedwait(worker_finished, 3000) == 0);
	assert(capture->worker_joined && capture->source_closed);
	coreaudio_update(capture, settings);
	assert(!capture->pending_settings && !capture->configured_device_uid);
	coreaudio_destroy(capture);
	drain_ui_tasks();
	coreaudio_reap_workers(false);
	assert(!workers);
	obs_data_release(settings);
	device_present = true;
	puts("PASS: module shutdown stops live missing-device retries before libobs destroys leftover sources");
}

static void test_expired_properties_refresh(void)
{
	obs_data_t *settings = make_settings("expired-source");
	struct coreaudio_data *capture = coreaudio_create(settings, (obs_source_t *)1, true);
	wait_ready(capture);
	for (int i = 0; i < 3000; i++) {
		pthread_mutex_lock(&capture->mutex);
		bool queued = capture->pending_property_updates > 0;
		pthread_mutex_unlock(&capture->mutex);
		if (queued)
			break;
		assert(i != 2999);
		usleep(1000);
	}
	int previous_updates = property_updates, previous_acquires = source_acquires;
	source_expired = true;
	drain_ui_tasks();
	assert(property_updates == previous_updates && source_acquires == previous_acquires);
	source_expired = false;
	coreaudio_destroy(capture);
	assert(os_event_timedwait(worker_finished, 3000) == 0);
	drain_ui_tasks();
	coreaudio_reap_workers(false);
	assert(!workers && weak_references == 0);
	obs_data_release(settings);
	puts("PASS: queued Properties refresh skips a fully released source before deferred .destroy closes its gate");
}

static void test_queued_notification(void)
{
	os_event_t *notification_entered, *allow_notification;
	os_event_init(&notification_entered, OS_EVENT_TYPE_AUTO);
	os_event_init(&allow_notification, OS_EVENT_TYPE_MANUAL);
	atomic_store(&block_start, true);
	os_event_reset(allow_start);
	obs_data_t *settings = make_settings("notification-device");
	struct coreaudio_data *capture = coreaudio_create(settings, (obs_source_t *)1, true);
	assert(os_event_timedwait(start_entered, 3000) == 0);
	dispatch_async(capture->notification_queue, ^{
		os_event_signal(notification_entered);
		os_event_wait(allow_notification);
		AudioObjectPropertyAddress address = {PROPERTY_FORMATS, kAudioObjectPropertyScopeGlobal,
						      kAudioObjectPropertyElementMain};
		capture->notification_block(1, &address);
		notifications++;
	});
	assert(os_event_timedwait(notification_entered, 3000) == 0);
	coreaudio_destroy(capture);
	atomic_store(&block_start, false);
	os_event_signal(allow_start);
	assert(os_event_timedwait(worker_finished, 50) == ETIMEDOUT);
	os_event_signal(allow_notification);
	assert(os_event_timedwait(worker_finished, 3000) == 0);
	assert(notifications == 1);
	drain_ui_tasks();
	coreaudio_reap_workers(false);
	assert(!workers);
	obs_data_release(settings);
	os_event_destroy(notification_entered);
	os_event_destroy(allow_notification);
	puts("PASS: teardown drains an already queued late device notification before releasing its context");
}

static void test_properties_and_defaults(void)
{
	obs_data_t *settings = obs_data_create();
	coreaudio_defaults(settings);
	struct coreaudio_data *capture = coreaudio_create(settings, (obs_source_t *)1, true);
	wait_ready(capture);
	pthread_mutex_lock(&capture->mutex);
	assert(capture->enable_downmix && strcmp(capture->configured_device_uid, "default") == 0);
	pthread_mutex_unlock(&capture->mutex);
	obs_data_set_bool(settings, "enable_downmix", false);
	for (int i = 0; i < 100; i++) {
		coreaudio_update(capture, settings);
		obs_properties_t *properties = coreaudio_properties(true, capture);
		obs_properties_destroy(properties);
	}
	wait_ready(capture);
	obs_properties_t *properties = coreaudio_properties(true, capture);
	obs_property_t *channel = obs_properties_get(properties, "output-default-1");
	assert(channel && obs_property_visible(channel) && obs_property_list_item_count(channel) == 3);
	obs_properties_destroy(properties);
	coreaudio_destroy(capture);
	assert(os_event_timedwait(worker_finished, 3000) == 0);
	drain_ui_tasks();
	coreaudio_reap_workers(false);
	assert(!workers);
	obs_data_release(settings);
	puts("PASS: worker snapshots preserve default settings; Properties stays safe during rapid channel-map updates and shows ready channels");
}

static void test_output_dedup(void)
{
	int previous_updates = dedup_updates, previous_removals = dedup_removals;
	obs_data_t *settings = make_settings("output-device");
	struct coreaudio_data *capture = coreaudio_create(settings, (obs_source_t *)1, false);
	wait_ready(capture);
	assert(dedup_updates == previous_updates + 1);
	coreaudio_update(capture, settings);
	assert(dedup_updates == previous_updates + 1);
	coreaudio_destroy(capture);
	assert(os_event_timedwait(worker_finished, 3000) == 0);
	drain_ui_tasks();
	coreaudio_reap_workers(false);
	assert(!workers && dedup_removals == previous_removals + 1);
	create_fails = true;
	assert(!coreaudio_create(settings, (obs_source_t *)1, false));
	assert(dedup_updates == previous_updates + 2 && dedup_removals == previous_removals + 2);
	create_fails = false;
	obs_data_release(settings);
	puts("PASS: output-monitoring dedup changes only for configured device changes and cleans up on worker-creation failure");
}

static void test_output_default_channels(void)
{
	obs_data_t *settings = make_settings("default");
	obs_data_set_bool(settings, "enable_downmix", false);
	struct coreaudio_data *capture = coreaudio_create(settings, (obs_source_t *)1, false);
	wait_ready(capture);
	pthread_mutex_lock(&capture->mutex);
	assert(strcmp(capture->configured_device_uid, "default") == 0);
	assert(strcmp(capture->device_uid, "BlackHole:test-device") == 0);
	pthread_mutex_unlock(&capture->mutex);
	obs_properties_t *properties = coreaudio_properties(false, capture);
	assert(obs_properties_get(properties, "output-default-1"));
	assert(!obs_properties_get(properties, "output-BlackHole_test-device-1"));
	obs_properties_destroy(properties);
	coreaudio_destroy(capture);
	assert(os_event_timedwait(worker_finished, 3000) == 0);
	drain_ui_tasks();
	coreaudio_reap_workers(false);
	assert(!workers);
	obs_data_release(settings);
	puts("PASS: resolved output default keeps the configured device's channel-map setting keys");
}

static void test_reconnect_delay_and_absent_retry(void)
{
	obs_data_t *settings = make_settings("reconnect-device");
	struct coreaudio_data *capture = coreaudio_create(settings, (obs_source_t *)1, true);
	wait_ready(capture);
	int previous_starts = starts;
	AudioObjectPropertyAddress address = {PROPERTY_DEFAULT_DEVICE, kAudioObjectPropertyScopeGlobal,
					      kAudioObjectPropertyElementMain};
	uint64_t before = os_gettime_ns();
	notification_callback(42, 1, &address, capture);
	for (int i = 0; i < 3000 && starts == previous_starts; i++)
		usleep(1000);
	assert(starts == previous_starts + 1 && os_gettime_ns() - before >= 250000000);
	wait_ready(capture);
	coreaudio_destroy(capture);
	assert(os_event_timedwait(worker_finished, 3000) == 0);
	drain_ui_tasks();
	coreaudio_reap_workers(false);
	assert(!workers);
	device_present = false;
	int previous_queries = name_queries;
	capture = coreaudio_create(settings, (obs_source_t *)1, true);
	usleep(20000);
	assert(name_queries == previous_queries);
	device_present = true;
	wait_ready(capture);
	pthread_mutex_lock(&capture->mutex);
	assert(strcmp(capture->device_uid, "reconnect-device") == 0);
	pthread_mutex_unlock(&capture->mutex);
	coreaudio_destroy(capture);
	assert(os_event_timedwait(worker_finished, 3000) == 0);
	drain_ui_tasks();
	coreaudio_reap_workers(false);
	assert(!workers);
	obs_data_release(settings);
	puts("PASS: default-device notifications preserve reconnect delay; absent devices retry and recover without changing the saved UID");
}

static void test_multiple_blocked_sources(void)
{
	struct coreaudio_data *captures[25];
	obs_data_t *settings = make_settings("parallel-device");
	atomic_store(&block_start, true);
	os_event_reset(allow_start);
	int previous_starts = starts, previous_joins = joined_threads;
	long allocations_before = bnum_allocs();
	for (size_t i = 0; i < 25; i++)
		captures[i] = coreaudio_create(settings, (obs_source_t *)1, true);
	for (int i = 0; i < 3000 && starts != previous_starts + 25; i++)
		usleep(1000);
	assert(starts == previous_starts + 25);
	uint64_t before = os_gettime_ns();
	for (size_t i = 0; i < 25; i++)
		coreaudio_destroy(captures[i]);
	assert(os_gettime_ns() - before < 500000000);
	atomic_store(&block_start, false);
	for (size_t i = 0; i < 25; i++)
		os_event_signal(
			allow_start); // libobs manual events preserve their signaled state but wake one waiter per signal, not an entire batch.
	coreaudio_free_type_data(NULL);
	drain_ui_tasks();
	coreaudio_reap_workers(false);
	assert(!workers && joined_threads == previous_joins + 25 && bnum_allocs() == allocations_before);
	os_event_reset(
		worker_finished); // This batch joins every thread directly; coalesced completion signals must not leak into the next case.
	os_event_reset(start_entered);
	obs_data_release(settings);
	puts("PASS: removing 25 simultaneously blocked sources stays responsive and shutdown joins/frees every worker");
}

int main(void)
{
	test_main_thread = pthread_self();
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
	pthread_mutex_init(&capture.mutex, NULL);
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
	bfree(capture.device_name);
	bfree(capture.device_uid);
	pthread_mutex_destroy(&capture.mutex);
	os_event_init(&worker_finished, OS_EVENT_TYPE_AUTO);
	os_event_init(&start_entered, OS_EVENT_TYPE_AUTO);
	os_event_init(&allow_start, OS_EVENT_TYPE_MANUAL);
	start_result = noErr;
	test_blocked_start();
	test_pending_updates();
	test_shutdown_live_retry();
	test_expired_properties_refresh();
	test_queued_notification();
	test_properties_and_defaults();
	test_output_dedup();
	test_output_default_channels();
	test_reconnect_delay_and_absent_retry();
	test_multiple_blocked_sources();
	drain_ui_tasks();
	int joins_before = joined_threads;
	long allocations_before = bnum_allocs();
	for (int i = 0; i < 100; i++) {
		obs_data_t *settings = make_settings("test-device");
		struct coreaudio_data *source = coreaudio_create(settings, (obs_source_t *)1, true);
		assert(source);
		coreaudio_destroy(source);
		assert(os_event_timedwait(worker_finished, 3000) == 0);
		drain_ui_tasks();
		coreaudio_reap_workers(false);
		obs_data_release(settings);
	}
	assert(!workers && joined_threads == joins_before + 100 && bnum_allocs() == allocations_before);
	create_fails = true;
	obs_data_t *settings = make_settings("test-device");
	assert(!coreaudio_create(settings, (obs_source_t *)1, true));
	obs_data_release(settings);
	assert(!workers);
	CFRelease(device_name);
	assert(weak_references == 0 && source_acquires == source_releases);
	os_event_destroy(worker_finished);
	os_event_destroy(start_entered);
	os_event_destroy(allow_start);
	puts("PASS: 100 create/destroy cycles reap every worker and thread-creation failure leaves no owned context");
	return 0;
}
