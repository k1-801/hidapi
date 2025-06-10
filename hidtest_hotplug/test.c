/*******************************************************
 HIDAPI - Multi-Platform library for
 communication with HID devices.

 Alan Ott
 Signal 11 Software

 libusb/hidapi Team

 Copyright 2022.

 This contents of this file may be used by anyone
 for any reason without any conditions and may be
 used as a starting point for your own applications
 which use HIDAPI.
********************************************************/

#include <stdio.h>
#include <wchar.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h> // for "tolower()"

#include <hidapi.h>

// Headers needed for sleeping and thread management
#ifdef _WIN32
	#include <windows.h>
	#include <conio.h>
#else
	#include <fcntl.h>
	#include <termios.h>
	#include <unistd.h>
    #include <pthread.h>
#endif

// Fallback/example
#ifndef HID_API_MAKE_VERSION
#define HID_API_MAKE_VERSION(mj, mn, p) (((mj) << 24) | ((mn) << 8) | (p))
#endif
#ifndef HID_API_VERSION
#define HID_API_VERSION HID_API_MAKE_VERSION(HID_API_VERSION_MAJOR, HID_API_VERSION_MINOR, HID_API_VERSION_PATCH)
#endif

//
// Sample using platform-specific headers
#if defined(__APPLE__) && HID_API_VERSION >= HID_API_MAKE_VERSION(0, 12, 0)
#include <hidapi_darwin.h>
#endif

#if defined(_WIN32) && HID_API_VERSION >= HID_API_MAKE_VERSION(0, 12, 0)
#include <hidapi_winapi.h>
#endif

#if defined(USING_HIDAPI_LIBUSB) && HID_API_VERSION >= HID_API_MAKE_VERSION(0, 12, 0)
#include <hidapi_libusb.h>
#endif

// Windows ddoesn't have pthread, but we can mimic it's behavior with a few simple winapi wrappers
#ifdef _WIN32
typedef struct
{
    ;
} pthread_handle;
typedef pthread_handle* pthread_t;

typedef struct
{
    ;
} pthread_attr_t;

int pthread_create(pthread_t *restrict thread,
                   const pthread_attr_t *restrict attr,
                   typeof(void *(void *)) *start_routine,
                   void *restrict arg)
{
    CreateThread(0, 0, start_routine, arg, 0, 0);
}

int pthread_join(pthread_t thread, void **retval)
{

}
#endif

//
// Report Device info
const char *hid_bus_name(hid_bus_type bus_type) {
	static const char *const HidBusTypeName[] = {
		"Unknown",
		"USB",
		"Bluetooth",
		"I2C",
		"SPI",
	};

	if ((int)bus_type < 0)
		bus_type = HID_API_BUS_UNKNOWN;
	if ((int)bus_type >= (int)(sizeof(HidBusTypeName) / sizeof(HidBusTypeName[0])))
		bus_type = HID_API_BUS_UNKNOWN;

	return HidBusTypeName[bus_type];
}

//
// Normal hotplug testing
int device_callback(
	hid_hotplug_callback_handle callback_handle,
	struct hid_device_info* device,
	hid_hotplug_event event,
	void* user_data)
{
	(void)user_data;

	if (event & HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED)
		printf("Handle %d: New device is connected: %s.\n", callback_handle, device->path);
	else
		printf("Handle %d: Device was disconnected: %s.\n", callback_handle, device->path);

	printf("type: %04hx %04hx\n  serial_number: %ls", device->vendor_id, device->product_id, device->serial_number);
	printf("\n");
	printf("  Manufacturer: %ls\n", device->manufacturer_string);
	printf("  Product:      %ls\n", device->product_string);
	printf("  Release:      %hx\n", device->release_number);
	printf("  Interface:    %d\n", device->interface_number);
	printf("  Usage (page): 0x%hx (0x%hx)\n", device->usage, device->usage_page);
	printf("(Press Q to exit the test)\n");
	printf("\n");

	return 0;
}


void test_hotplug(void)
{
	printf("Starting the Hotplug test\n");
	printf("(Press Q to exit the test)\n");

	hid_hotplug_callback_handle token1, token2;

	hid_hotplug_register_callback(0, 0, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED | HID_API_HOTPLUG_EVENT_DEVICE_LEFT, HID_API_HOTPLUG_ENUMERATE, device_callback, NULL, &token1);
	hid_hotplug_register_callback(0x054c, 0x0ce6, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED | HID_API_HOTPLUG_EVENT_DEVICE_LEFT, HID_API_HOTPLUG_ENUMERATE, device_callback, NULL, &token2);

	while (1)
	{
		int command = tolower(waitkey());
		if ('q' == command)
		{
			break;
		}
	}

	hid_hotplug_deregister_callback(token2);
	hid_hotplug_deregister_callback(token1);

	printf("\n\nHotplug test stopped\n");
}

//
// Stress-testing weird edge cases in hotplugs
int cb1_handle;
int cb2_handle;
int cb_test1_triggered;

int cb2_func(hid_hotplug_callback_handle callback_handle,
             struct hid_device_info *device,
             hid_hotplug_event event,
             void *user_data)
{
	(void) callback_handle;
	(void) device;
	(void) event;
	(void) user_data;
	// TIP: only perform the test once
	if(cb_test1_triggered)
	{
		return 1;
	}

	printf("Callback 2 fired\n");

	// Deregister the first callback
	// It should be placed in the list at an index prior to the current one, which will make the pointer to the current one invalid on some implementations
	hid_hotplug_deregister_callback(cb1_handle);

	cb_test1_triggered = 1;

	// As long as we are inside this callback, nothing goes wrong; however, returning from here will cause a use-after-free error on flawed implementations
	// as to retrieve the next element (or to check for it's presence) it will look those dereference a pointer located in an already freed area
	// Undefined behavior
	return 1;
}

int cb1_func(hid_hotplug_callback_handle callback_handle,
             struct hid_device_info *device,
             hid_hotplug_event event,
             void *user_data)
{
	(void) callback_handle;
	(void) device;
	(void) event;
	(void) user_data;

	// TIP: only perform the test once
	if(cb_test1_triggered)
	{
		return 1;
	}

	printf("Callback 1 fired\n");

	// Register the second callback and make it be called immediately by enumeration attempt
	// Will cause a deadlock on Linux immediately
	hid_hotplug_register_callback(0, 0, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED | HID_API_HOTPLUG_EVENT_DEVICE_LEFT, HID_API_HOTPLUG_ENUMERATE, cb2_func, NULL, &cb2_handle);
	return 1;
}

void test_hotplug_deadlocks(void)
{
	cb_test1_triggered = 0;
	printf("Starting the Hotplug callbacks deadlocks test\n");
	printf("TIP: if you don't see a message that it succeeded, it means the test failed and the system is now deadlocked\n");
	// Register the first callback and make it be called immediately by enumeration attempt (if at least 1 device is present)
	hid_hotplug_register_callback(0, 0, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED | HID_API_HOTPLUG_EVENT_DEVICE_LEFT, HID_API_HOTPLUG_ENUMERATE, cb1_func, NULL, &cb1_handle);

	printf("Test finished successfully (at least no deadlocks were found)\n");

    // Intentionally leave a callback registered to test how hid_exit handles it
    //hid_hotplug_deregister_callback(cb2_handle);
}


//
// CLI

void print_version_check(void)
{
	printf("hidapi test/example tool. Compiled with hidapi version %s, runtime version %s.\n", HID_API_VERSION_STR, hid_version_str());
	if (HID_API_VERSION == HID_API_MAKE_VERSION(hid_version()->major, hid_version()->minor, hid_version()->patch)) {
		printf("Compile-time version matches runtime version of hidapi.\n\n");
	}
	else {
		printf("Compile-time version is different than runtime version of hidapi.\n]n");
	}
}

//
// Main
int main(int argc, char* argv[])
{
	(void)argc;
	(void)argv;

	/* --- HIDAPI R&D: this is just to force the compiler to ensure
	       each of those functions are implemented (even as a stub)
	       by each backend. --- */
	(void)&hid_open;
	(void)&hid_open_path;
	(void)&hid_read_timeout;
	(void)&hid_get_input_report;
#if HID_API_VERSION >= HID_API_MAKE_VERSION(0, 15, 0)
	(void)&hid_send_output_report;
#endif
	(void)&hid_get_feature_report;
	(void)&hid_send_feature_report;
#if HID_API_VERSION >= HID_API_MAKE_VERSION(0, 14, 0)
	(void)&hid_get_report_descriptor;
#endif
	/* --- */

	if (hid_init())
		return -1;

#if defined(__APPLE__) && HID_API_VERSION >= HID_API_MAKE_VERSION(0, 12, 0)
	// To work properly needs to be called before hid_open/hid_open_path after hid_init.
	// Best/recommended option - call it right after hid_init.
	hid_darwin_set_open_exclusive(0);
#endif

    // Step 1: Version check
    print_version_check();

    // Step 2: Check implementation for deadlocks (takes a few seconds)
	test_hotplug_deadlocks();

	/* Free static HIDAPI objects. */
	hid_exit();

	return 0;
}
