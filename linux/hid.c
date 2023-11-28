/*******************************************************
 HIDAPI - Multi-Platform library for
 communication with HID devices.

 Alan Ott
 Signal 11 Software

 libusb/hidapi Team

 Copyright 2022, All Rights Reserved.

 At the discretion of the user of this library,
 this software may be licensed under the terms of the
 GNU General Public License v3, a BSD-Style license, or the
 original HIDAPI license as outlined in the LICENSE.txt,
 LICENSE-gpl3.txt, LICENSE-bsd.txt, and LICENSE-orig.txt
 files located at the root of the source distribution.
 These files may also be found in the public source
 code repository located at:
        https://github.com/libusb/hidapi .
********************************************************/

/* C */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <locale.h>
#include <errno.h>

/* Unix */
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>

/* Linux */
#include <linux/hidraw.h>
#include <linux/version.h>
#include <linux/input.h>
#include <libudev.h>

#include "hidapi.h"

#ifdef HIDAPI_ALLOW_BUILD_WORKAROUND_KERNEL_2_6_39
/* This definitions first appeared in Linux Kernel 2.6.39 in linux/hidraw.h.
    hidapi doesn't support kernels older than that,
    so we don't define macros below explicitly, to fail builds on old kernels.
    For those who really need this as a workaround (e.g. to be able to build on old build machines),
    can workaround by defining the macro above.
*/
#ifndef HIDIOCSFEATURE
#define HIDIOCSFEATURE(len)    _IOC(_IOC_WRITE|_IOC_READ, 'H', 0x06, len)
#endif
#ifndef HIDIOCGFEATURE
#define HIDIOCGFEATURE(len)    _IOC(_IOC_WRITE|_IOC_READ, 'H', 0x07, len)
#endif

#endif


// HIDIOCGINPUT is not defined in Linux kernel headers < 5.11.
// This definition is from hidraw.h in Linux >= 5.11.
// https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/?id=f43d3870cafa2a0f3854c1819c8385733db8f9ae
#ifndef HIDIOCGINPUT
#define HIDIOCGINPUT(len)    _IOC(_IOC_WRITE|_IOC_READ, 'H', 0x0A, len)
#endif

struct hid_device_ {
	int device_handle;
	int blocking;
	wchar_t *last_error_str;
	struct hid_device_info* device_info;
};

static struct hid_api_version api_version = {
	.major = HID_API_VERSION_MAJOR,
	.minor = HID_API_VERSION_MINOR,
	.patch = HID_API_VERSION_PATCH
};

static wchar_t *last_global_error_str = NULL;


static hid_device *new_hid_device(void)
{
	hid_device *dev = (hid_device*) calloc(1, sizeof(hid_device));
	if (dev == NULL) {
		return NULL;
	}

	dev->device_handle = -1;
	dev->blocking = 1;
	dev->last_error_str = NULL;
	dev->device_info = NULL;

	return dev;
}


/* The caller must free the returned string with free(). */
static wchar_t *utf8_to_wchar_t(const char *utf8)
{
	wchar_t *ret = NULL;

	if (utf8) {
		size_t wlen = mbstowcs(NULL, utf8, 0);
		if ((size_t) -1 == wlen) {
			return wcsdup(L"");
		}
		ret = (wchar_t*) calloc(wlen+1, sizeof(wchar_t));
		if (ret == NULL) {
			/* as much as we can do at this point */
			return NULL;
		}
		mbstowcs(ret, utf8, wlen+1);
		ret[wlen] = 0x0000;
	}

	return ret;
}


/* Makes a copy of the given error message (and decoded according to the
 * currently locale) into the wide string pointer pointed by error_str.
 * The last stored error string is freed.
 * Use register_error_str(NULL) to free the error message completely. */
static void register_error_str(wchar_t **error_str, const char *msg)
{
	free(*error_str);
	*error_str = utf8_to_wchar_t(msg);
}

/* Semilar to register_error_str, but allows passing a format string with va_list args into this function. */
static void register_error_str_vformat(wchar_t **error_str, const char *format, va_list args)
{
	char msg[256];
	vsnprintf(msg, sizeof(msg), format, args);

	register_error_str(error_str, msg);
}

/* Set the last global error to be reported by hid_error(NULL).
 * The given error message will be copied (and decoded according to the
 * currently locale, so do not pass in string constants).
 * The last stored global error message is freed.
 * Use register_global_error(NULL) to indicate "no error". */
static void register_global_error(const char *msg)
{
	register_error_str(&last_global_error_str, msg);
}

/* Similar to register_global_error, but allows passing a format string into this function. */
static void register_global_error_format(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	register_error_str_vformat(&last_global_error_str, format, args);
	va_end(args);
}

/* Set the last error for a device to be reported by hid_error(dev).
 * The given error message will be copied (and decoded according to the
 * currently locale, so do not pass in string constants).
 * The last stored device error message is freed.
 * Use register_device_error(dev, NULL) to indicate "no error". */
static void register_device_error(hid_device *dev, const char *msg)
{
	register_error_str(&dev->last_error_str, msg);
}

/* Similar to register_device_error, but you can pass a format string into this function. */
static void register_device_error_format(hid_device *dev, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	register_error_str_vformat(&dev->last_error_str, format, args);
	va_end(args);
}

/* Get an attribute value from a udev_device and return it as a whar_t
   string. The returned string must be freed with free() when done.*/
static wchar_t *copy_udev_string(struct udev_device *dev, const char *udev_name)
{
	return utf8_to_wchar_t(udev_device_get_sysattr_value(dev, udev_name));
}

/*
 * Gets the size of the HID item at the given position
 * Returns 1 if successful, 0 if an invalid key
 * Sets data_len and key_size when successful
 */
static int get_hid_item_size(__u8 *report_descriptor, unsigned int pos, __u32 size, int *data_len, int *key_size)
{
	int key = report_descriptor[pos];
	int size_code;

	/*
	 * This is a Long Item. The next byte contains the
	 * length of the data section (value) for this key.
	 * See the HID specification, version 1.11, section
	 * 6.2.2.3, titled "Long Items."
	 */
	if ((key & 0xf0) == 0xf0) {
		if (pos + 1 < size)
		{
			*data_len = report_descriptor[pos + 1];
			*key_size = 3;
			return 1;
		}
		*data_len = 0; /* malformed report */
		*key_size = 0;
	}

	/*
	 * This is a Short Item. The bottom two bits of the
	 * key contain the size code for the data section
	 * (value) for this key. Refer to the HID
	 * specification, version 1.11, section 6.2.2.2,
	 * titled "Short Items."
	 */
	size_code = key & 0x3;
	switch (size_code) {
	case 0:
	case 1:
	case 2:
		*data_len = size_code;
		*key_size = 1;
		return 1;
	case 3:
		*data_len = 4;
		*key_size = 1;
		return 1;
	default:
		/* Can't ever happen since size_code is & 0x3 */
		*data_len = 0;
		*key_size = 0;
		break;
	};

	/* malformed report */
	return 0;
}

/*
 * Get bytes from a HID Report Descriptor.
 * Only call with a num_bytes of 0, 1, 2, or 4.
 */
static __u32 get_hid_report_bytes(__u8 *rpt, size_t len, size_t num_bytes, size_t cur)
{
	/* Return if there aren't enough bytes. */
	if (cur + num_bytes >= len)
		return 0;

	if (num_bytes == 0)
		return 0;
	else if (num_bytes == 1)
		return rpt[cur + 1];
	else if (num_bytes == 2)
		return (rpt[cur + 2] * 256 + rpt[cur + 1]);
	else if (num_bytes == 4)
		return (
			rpt[cur + 4] * 0x01000000 +
			rpt[cur + 3] * 0x00010000 +
			rpt[cur + 2] * 0x00000100 +
			rpt[cur + 1] * 0x00000001
		);
	else
		return 0;
}

/*
 * Retrieves the device's Usage Page and Usage from the report descriptor.
 * The algorithm returns the current Usage Page/Usage pair whenever a new
 * Collection is found and a Usage Local Item is currently in scope.
 * Usage Local Items are consumed by each Main Item (See. 6.2.2.8).
 * The algorithm should give similar results as Apple's:
 *   https://developer.apple.com/documentation/iokit/kiohiddeviceusagepairskey?language=objc
 * Physical Collections are also matched (macOS does the same).
 *
 * This function can be called repeatedly until it returns non-0
 * Usage is found. pos is the starting point (initially 0) and will be updated
 * to the next search position.
 *
 * The return value is 0 when a pair is found.
 * 1 when finished processing descriptor.
 * -1 on a malformed report.
 */
static int get_next_hid_usage(__u8 *report_descriptor, __u32 size, unsigned int *pos, unsigned short *usage_page, unsigned short *usage)
{
	int data_len, key_size;
	int initial = *pos == 0; /* Used to handle case where no top-level application collection is defined */
	int usage_pair_ready = 0;

	/* Usage is a Local Item, it must be set before each Main Item (Collection) before a pair is returned */
	int usage_found = 0;

	while (*pos < size) {
		int key = report_descriptor[*pos];
		int key_cmd = key & 0xfc;

		/* Determine data_len and key_size */
		if (!get_hid_item_size(report_descriptor, *pos, size, &data_len, &key_size))
			return -1; /* malformed report */

		switch (key_cmd) {
		case 0x4: /* Usage Page 6.2.2.7 (Global) */
			*usage_page = get_hid_report_bytes(report_descriptor, size, data_len, *pos);
			break;

		case 0x8: /* Usage 6.2.2.8 (Local) */
			*usage = get_hid_report_bytes(report_descriptor, size, data_len, *pos);
			usage_found = 1;
			break;

		case 0xa0: /* Collection 6.2.2.4 (Main) */
			/* A Usage Item (Local) must be found for the pair to be valid */
			if (usage_found)
				usage_pair_ready = 1;

			/* Usage is a Local Item, unset it */
			usage_found = 0;
			break;

		case 0x80: /* Input 6.2.2.4 (Main) */
		case 0x90: /* Output 6.2.2.4 (Main) */
		case 0xb0: /* Feature 6.2.2.4 (Main) */
		case 0xc0: /* End Collection 6.2.2.4 (Main) */
			/* Usage is a Local Item, unset it */
			usage_found = 0;
			break;
		}

		/* Skip over this key and its associated data */
		*pos += data_len + key_size;

		/* Return usage pair */
		if (usage_pair_ready)
			return 0;
	}

	/* If no top-level application collection is found and usage page/usage pair is found, pair is valid
	   https://docs.microsoft.com/en-us/windows-hardware/drivers/hid/top-level-collections */
	if (initial && usage_found)
		return 0; /* success */

	return 1; /* finished processing */
}

/*
 * Retrieves the hidraw report descriptor from a file.
 * When using this form, <sysfs_path>/device/report_descriptor, elevated priviledges are not required.
 */
static int get_hid_report_descriptor(const char *rpt_path, struct hidraw_report_descriptor *rpt_desc)
{
	int rpt_handle;
	ssize_t res;

	rpt_handle = open(rpt_path, O_RDONLY | O_CLOEXEC);
	if (rpt_handle < 0) {
		register_global_error_format("open failed (%s): %s", rpt_path, strerror(errno));
		return -1;
	}

	/*
	 * Read in the Report Descriptor
	 * The sysfs file has a maximum size of 4096 (which is the same as HID_MAX_DESCRIPTOR_SIZE) so we should always
	 * be ok when reading the descriptor.
	 * In practice if the HID descriptor is any larger I suspect many other things will break.
	 */
	memset(rpt_desc, 0x0, sizeof(*rpt_desc));
	res = read(rpt_handle, rpt_desc->value, HID_MAX_DESCRIPTOR_SIZE);
	if (res < 0) {
		register_global_error_format("read failed (%s): %s", rpt_path, strerror(errno));
	}
	rpt_desc->size = (__u32) res;

	close(rpt_handle);
	return (int) res;
}

/* return size of the descriptor, or -1 on failure */
static int get_hid_report_descriptor_from_sysfs(const char *sysfs_path, struct hidraw_report_descriptor *rpt_desc)
{
	int res = -1;
	/* Construct <sysfs_path>/device/report_descriptor */
	size_t rpt_path_len = strlen(sysfs_path) + 25 + 1;
	char* rpt_path = (char*) calloc(1, rpt_path_len);
	snprintf(rpt_path, rpt_path_len, "%s/device/report_descriptor", sysfs_path);

	res = get_hid_report_descriptor(rpt_path, rpt_desc);
	free(rpt_path);

	return res;
}

/* return non-zero if successfully parsed */
static int parse_hid_vid_pid_from_uevent(const char *uevent, unsigned *bus_type, unsigned short *vendor_id, unsigned short *product_id)
{
	char tmp[1024];
	size_t uevent_len = strlen(uevent);
	if (uevent_len > sizeof(tmp) - 1)
		uevent_len = sizeof(tmp) - 1;
	memcpy(tmp, uevent, uevent_len);
	tmp[uevent_len] = '\0';

	char *saveptr = NULL;
	char *line;
	char *key;
	char *value;

	line = strtok_r(tmp, "\n", &saveptr);
	while (line != NULL) {
		/* line: "KEY=value" */
		key = line;
		value = strchr(line, '=');
		if (!value) {
			goto next_line;
		}
		*value = '\0';
		value++;

		if (strcmp(key, "HID_ID") == 0) {
			/**
			 *        type vendor   product
			 * HID_ID=0003:000005AC:00008242
			 **/
			int ret = sscanf(value, "%x:%hx:%hx", bus_type, vendor_id, product_id);
			if (ret == 3) {
				return 1;
			}
		}

next_line:
		line = strtok_r(NULL, "\n", &saveptr);
	}

	register_global_error("Couldn't find/parse HID_ID");
	return 0;
}

/* return non-zero if successfully parsed */
static int parse_hid_vid_pid_from_uevent_path(const char *uevent_path, unsigned *bus_type, unsigned short *vendor_id, unsigned short *product_id)
{
	int handle;
	ssize_t res;

	handle = open(uevent_path, O_RDONLY | O_CLOEXEC);
	if (handle < 0) {
		register_global_error_format("open failed (%s): %s", uevent_path, strerror(errno));
		return 0;
	}

	char buf[1024];
	res = read(handle, buf, sizeof(buf));
	close(handle);

	if (res < 0) {
		register_global_error_format("read failed (%s): %s", uevent_path, strerror(errno));
		return 0;
	}

	buf[res] = '\0';
	return parse_hid_vid_pid_from_uevent(buf, bus_type, vendor_id, product_id);
}

/* return non-zero if successfully read/parsed */
static int parse_hid_vid_pid_from_sysfs(const char *sysfs_path, unsigned *bus_type, unsigned short *vendor_id, unsigned short *product_id)
{
	int res = 0;
	/* Construct <sysfs_path>/device/uevent */
	size_t uevent_path_len = strlen(sysfs_path) + 14 + 1;
	char* uevent_path = (char*) calloc(1, uevent_path_len);
	snprintf(uevent_path, uevent_path_len, "%s/device/uevent", sysfs_path);

	res = parse_hid_vid_pid_from_uevent_path(uevent_path, bus_type, vendor_id, product_id);
	free(uevent_path);

	return res;
}

static int get_hid_report_descriptor_from_hidraw(hid_device *dev, struct hidraw_report_descriptor *rpt_desc)
{
	int desc_size = 0;

	/* Get Report Descriptor Size */
	int res = ioctl(dev->device_handle, HIDIOCGRDESCSIZE, &desc_size);
	if (res < 0) {
		register_device_error_format(dev, "ioctl(GRDESCSIZE): %s", strerror(errno));
		return res;
	}

	/* Get Report Descriptor */
	memset(rpt_desc, 0x0, sizeof(*rpt_desc));
	rpt_desc->size = desc_size;
	res = ioctl(dev->device_handle, HIDIOCGRDESC, rpt_desc);
	if (res < 0) {
		register_device_error_format(dev, "ioctl(GRDESC): %s", strerror(errno));
	}

	return res;
}

/*
 * The caller is responsible for free()ing the (newly-allocated) character
 * strings pointed to by serial_number_utf8 and product_name_utf8 after use.
 */
static int parse_uevent_info(const char *uevent, unsigned *bus_type,
	unsigned short *vendor_id, unsigned short *product_id,
	char **serial_number_utf8, char **product_name_utf8)
{
	char tmp[1024];
	size_t uevent_len = strlen(uevent);
	if (uevent_len > sizeof(tmp) - 1)
		uevent_len = sizeof(tmp) - 1;
	memcpy(tmp, uevent, uevent_len);
	tmp[uevent_len] = '\0';

	char *saveptr = NULL;
	char *line;
	char *key;
	char *value;

	int found_id = 0;
	int found_serial = 0;
	int found_name = 0;

	line = strtok_r(tmp, "\n", &saveptr);
	while (line != NULL) {
		/* line: "KEY=value" */
		key = line;
		value = strchr(line, '=');
		if (!value) {
			goto next_line;
		}
		*value = '\0';
		value++;

		if (strcmp(key, "HID_ID") == 0) {
			/**
			 *        type vendor   product
			 * HID_ID=0003:000005AC:00008242
			 **/
			int ret = sscanf(value, "%x:%hx:%hx", bus_type, vendor_id, product_id);
			if (ret == 3) {
				found_id = 1;
			}
		} else if (strcmp(key, "HID_NAME") == 0) {
			/* The caller has to free the product name */
			*product_name_utf8 = strdup(value);
			found_name = 1;
		} else if (strcmp(key, "HID_UNIQ") == 0) {
			/* The caller has to free the serial number */
			*serial_number_utf8 = strdup(value);
			found_serial = 1;
		}

next_line:
		line = strtok_r(NULL, "\n", &saveptr);
	}

	return (found_id && found_name && found_serial);
}


static struct hid_device_info * create_device_info_for_device(struct udev_device *raw_dev)
{
	struct hid_device_info *root = NULL;
	struct hid_device_info *cur_dev = NULL;

	const char *sysfs_path;
	const char *dev_path;
	const char *str;
	struct udev_device *hid_dev; /* The device's HID udev node. */
	struct udev_device *usb_dev; /* The device's USB udev node. */
	struct udev_device *intf_dev; /* The device's interface (in the USB sense). */
	unsigned short dev_vid;
	unsigned short dev_pid;
	char *serial_number_utf8 = NULL;
	char *product_name_utf8 = NULL;
	unsigned bus_type;
	int result;
	struct hidraw_report_descriptor report_desc;

	sysfs_path = udev_device_get_syspath(raw_dev);
	dev_path = udev_device_get_devnode(raw_dev);

	hid_dev = udev_device_get_parent_with_subsystem_devtype(
		raw_dev,
		"hid",
		NULL);

	if (!hid_dev) {
		/* Unable to find parent hid device. */
		goto end;
	}

	result = parse_uevent_info(
		udev_device_get_sysattr_value(hid_dev, "uevent"),
		&bus_type,
		&dev_vid,
		&dev_pid,
		&serial_number_utf8,
		&product_name_utf8);

	if (!result) {
		/* parse_uevent_info() failed for at least one field. */
		goto end;
	}

	/* Filter out unhandled devices right away */
	switch (bus_type) {
		case BUS_BLUETOOTH:
		case BUS_I2C:
		case BUS_USB:
		case BUS_SPI:
			break;

		default:
			goto end;
	}

	/* Create the record. */
	root = (struct hid_device_info*) calloc(1, sizeof(struct hid_device_info));
	if (!root)
		goto end;

	cur_dev = root;

	/* Fill out the record */
	cur_dev->next = NULL;
	cur_dev->path = dev_path? strdup(dev_path): NULL;

	/* VID/PID */
	cur_dev->vendor_id = dev_vid;
	cur_dev->product_id = dev_pid;

	/* Serial Number */
	cur_dev->serial_number = utf8_to_wchar_t(serial_number_utf8);

	/* Release Number */
	cur_dev->release_number = 0x0;

	/* Interface Number */
	cur_dev->interface_number = -1;

	switch (bus_type) {
		case BUS_USB:
			/* The device pointed to by raw_dev contains information about
				the hidraw device. In order to get information about the
				USB device, get the parent device with the
				subsystem/devtype pair of "usb"/"usb_device". This will
				be several levels up the tree, but the function will find
				it. */
			usb_dev = udev_device_get_parent_with_subsystem_devtype(
					raw_dev,
					"usb",
					"usb_device");

			/* uhid USB devices
			 * Since this is a virtual hid interface, no USB information will
			 * be available. */
			if (!usb_dev) {
				/* Manufacturer and Product strings */
				cur_dev->manufacturer_string = wcsdup(L"");
				cur_dev->product_string = utf8_to_wchar_t(product_name_utf8);
				break;
			}

			cur_dev->manufacturer_string = copy_udev_string(usb_dev, "manufacturer");
			cur_dev->product_string = copy_udev_string(usb_dev, "product");

			cur_dev->bus_type = HID_API_BUS_USB;

			str = udev_device_get_sysattr_value(usb_dev, "bcdDevice");
			cur_dev->release_number = (str)? strtol(str, NULL, 16): 0x0;

			/* Get a handle to the interface's udev node. */
			intf_dev = udev_device_get_parent_with_subsystem_devtype(
					raw_dev,
					"usb",
					"usb_interface");
			if (intf_dev) {
				str = udev_device_get_sysattr_value(intf_dev, "bInterfaceNumber");
				cur_dev->interface_number = (str)? strtol(str, NULL, 16): -1;
			}

			break;

		case BUS_BLUETOOTH:
			cur_dev->manufacturer_string = wcsdup(L"");
			cur_dev->product_string = utf8_to_wchar_t(product_name_utf8);

			cur_dev->bus_type = HID_API_BUS_BLUETOOTH;

			break;
		case BUS_I2C:
			cur_dev->manufacturer_string = wcsdup(L"");
			cur_dev->product_string = utf8_to_wchar_t(product_name_utf8);

			cur_dev->bus_type = HID_API_BUS_I2C;

			break;

		case BUS_SPI:
			cur_dev->manufacturer_string = wcsdup(L"");
			cur_dev->product_string = utf8_to_wchar_t(product_name_utf8);

			cur_dev->bus_type = HID_API_BUS_SPI;

			break;

		default:
			/* Unknown device type - this should never happen, as we
			 * check for USB and Bluetooth devices above */
			break;
	}

	/* Usage Page and Usage */
	result = get_hid_report_descriptor_from_sysfs(sysfs_path, &report_desc);
	if (result >= 0) {
		unsigned short page = 0, usage = 0;
		unsigned int pos = 0;
		/*
		 * Parse the first usage and usage page
		 * out of the report descriptor.
		 */
		if (!get_next_hid_usage(report_desc.value, report_desc.size, &pos, &page, &usage)) {
			cur_dev->usage_page = page;
			cur_dev->usage = usage;
		}

		/*
		 * Parse any additional usage and usage pages
		 * out of the report descriptor.
		 */
		while (!get_next_hid_usage(report_desc.value, report_desc.size, &pos, &page, &usage)) {
			/* Create new record for additional usage pairs */
			struct hid_device_info *tmp = (struct hid_device_info*) calloc(1, sizeof(struct hid_device_info));
			struct hid_device_info *prev_dev = cur_dev;

			if (!tmp)
				continue;
			cur_dev->next = tmp;
			cur_dev = tmp;

			/* Update fields */
			cur_dev->path = dev_path? strdup(dev_path): NULL;
			cur_dev->vendor_id = dev_vid;
			cur_dev->product_id = dev_pid;
			cur_dev->serial_number = prev_dev->serial_number? wcsdup(prev_dev->serial_number): NULL;
			cur_dev->release_number = prev_dev->release_number;
			cur_dev->interface_number = prev_dev->interface_number;
			cur_dev->manufacturer_string = prev_dev->manufacturer_string? wcsdup(prev_dev->manufacturer_string): NULL;
			cur_dev->product_string = prev_dev->product_string? wcsdup(prev_dev->product_string): NULL;
			cur_dev->usage_page = page;
			cur_dev->usage = usage;
			cur_dev->bus_type = prev_dev->bus_type;
		}
	}

end:
	free(serial_number_utf8);
	free(product_name_utf8);

	return root;
}

static struct hid_device_info * create_device_info_for_hid_device(hid_device *dev) {
	struct udev *udev;
	struct udev_device *udev_dev;
	struct stat s;
	int ret = -1;
	struct hid_device_info *root = NULL;

	register_device_error(dev, NULL);

	/* Get the dev_t (major/minor numbers) from the file handle. */
	ret = fstat(dev->device_handle, &s);
	if (-1 == ret) {
		register_device_error(dev, "Failed to stat device handle");
		return NULL;
	}

	/* Create the udev object */
	udev = udev_new();
	if (!udev) {
		register_device_error(dev, "Couldn't create udev context");
		return NULL;
	}

	/* Open a udev device from the dev_t. 'c' means character device. */
	udev_dev = udev_device_new_from_devnum(udev, 'c', s.st_rdev);
	if (udev_dev) {
		root = create_device_info_for_device(udev_dev);
	}

	if (!root) {
		/* TODO: have a better error reporting via create_device_info_for_device */
		register_device_error(dev, "Couldn't create hid_device_info");
	}

	udev_device_unref(udev_dev);
	udev_unref(udev);

	return root;
}

HID_API_EXPORT const struct hid_api_version* HID_API_CALL hid_version(void)
{
	return &api_version;
}

HID_API_EXPORT const char* HID_API_CALL hid_version_str(void)
{
	return HID_API_VERSION_STR;
}

static struct hid_hotplug_context {
	/* UDEV context that handles the monitor */
	struct udev* udev_ctx;

	/* UDEV monitor that receives events */
	struct udev_monitor* mon;

	/* File descriptor for the UDEV monitor that allows to check for new events with select() */
	int monitor_fd;

	/* Thread for the UDEV monitor */
	pthread_t thread;

	pthread_mutex_t mutex;

	/* HIDAPI unique callback handle counter */
	hid_hotplug_callback_handle next_handle;

	/* Linked list of the hotplug callbacks */
	struct hid_hotplug_callback *hotplug_cbs;

	/* Linked list of the device infos (mandatory when the device is disconnected) */
	struct hid_device_info *devs;
} hid_hotplug_context = {
	.udev_ctx = NULL,
	.monitor_fd = -1,
	.next_handle = 1,
	.hotplug_cbs = NULL,
	.devs = NULL
};

static void hid_internal_hotplug_cleanup()
{
	if (hid_hotplug_context.hotplug_cbs == NULL) {
		/* Cleanup connected device list */
		hid_free_enumeration(hid_hotplug_context.devs);
		hid_hotplug_context.devs = NULL;
		/* Disarm the udev monitor */
		udev_monitor_unref(hid_hotplug_context.mon);
		udev_unref(hid_hotplug_context.udev_ctx);
	}
}

static void hid_internal_hotplug_init()
{
	pthread_mutex_init(&hid_hotplug_context.mutex, NULL);
}

static void hid_internal_hotplug_exit()
{
	pthread_mutex_lock(&hid_hotplug_context.mutex);
	hid_hotplug_callback** current = &hid_hotplug_context.hotplug_cbs
	/* Remove all callbacks from the list */
	while(*current)
	{
		hid_hotplug_callback* next = (*current)->next;
		free(*current);
		*current = next;
	}
	hid_internal_hotplug_cleanup();
	pthread_mutex_unlock(&hid_hotplug_context.mutex);
	pthread_mutex_destroy(&hid_hotplug_context.mutex);
}

int HID_API_EXPORT hid_init(void)
{
	const char *locale;

	/* indicate no error */
	register_global_error(NULL);

	/* Set the locale if it's not set. */
	locale = setlocale(LC_CTYPE, NULL);
	if (!locale)
		setlocale(LC_CTYPE, "");

	hid_internal_hotplug_init();

	return 0;
}


int HID_API_EXPORT hid_exit(void)
{
	/* Free global error message */
	register_global_error(NULL);

	hid_internal_hotplug_exit();
	
	return 0;
}

static int hid_internal_match_device_id(unsigned short vendor_id, unsigned short product_id, unsigned short expected_vendor_id, unsigned short expected_product_id)
{
    return (expected_vendor_id == 0x0 || vendor_id == expected_vendor_id) && (expected_product_id == 0x0 || product_id == expected_product_id);
}

struct hid_device_info  HID_API_EXPORT *hid_enumerate(unsigned short vendor_id, unsigned short product_id)
{
	struct udev *udev;
	struct udev_enumerate *enumerate;
	struct udev_list_entry *devices, *dev_list_entry;

	struct hid_device_info *root = NULL; /* return object */
	struct hid_device_info *cur_dev = NULL;

	hid_init();
	/* register_global_error: global error is reset by hid_init */

	/* Create the udev object */
	udev = udev_new();
	if (!udev) {
		register_global_error("Couldn't create udev context");
		return NULL;
	}

	/* Create a list of the devices in the 'hidraw' subsystem. */
	enumerate = udev_enumerate_new(udev);
	udev_enumerate_add_match_subsystem(enumerate, "hidraw");
	udev_enumerate_scan_devices(enumerate);
	devices = udev_enumerate_get_list_entry(enumerate);
	/* For each item, see if it matches the vid/pid, and if so
	   create a udev_device record for it */
	udev_list_entry_foreach(dev_list_entry, devices) {
		const char *sysfs_path;
		unsigned short dev_vid = 0;
		unsigned short dev_pid = 0;
		unsigned bus_type = 0;
		struct udev_device *raw_dev; /* The device's hidraw udev node. */
		struct hid_device_info * tmp;

		/* Get the filename of the /sys entry for the device
		   and create a udev_device object (dev) representing it */
		sysfs_path = udev_list_entry_get_name(dev_list_entry);
		if (!sysfs_path)
			continue;

		if (vendor_id != 0 || product_id != 0) {
			if (!parse_hid_vid_pid_from_sysfs(sysfs_path, &bus_type, &dev_vid, &dev_pid))
				continue;

			if (vendor_id != 0 && vendor_id != dev_vid)
				continue;
			if (product_id != 0 && product_id != dev_pid)
				continue;
		}

		raw_dev = udev_device_new_from_syspath(udev, sysfs_path);
		if (!raw_dev)
			continue;

		tmp = create_device_info_for_device(raw_dev);
		if (tmp) {
			if (cur_dev) {
				cur_dev->next = tmp;
			}
			else {
				root = tmp;
			}
			cur_dev = tmp;

			/* move the pointer to the tail of returnd list */
			while (cur_dev->next != NULL) {
				cur_dev = cur_dev->next;
			}
		}

		udev_device_unref(raw_dev);
	}
	/* Free the enumerator and udev objects. */
	udev_enumerate_unref(enumerate);
	udev_unref(udev);

	if (root == NULL) {
		if (vendor_id == 0 && product_id == 0) {
			register_global_error("No HID devices found in the system.");
		} else {
			register_global_error("No HID devices with requested VID/PID found in the system.");
		}
	}

	return root;
}

void  HID_API_EXPORT hid_free_enumeration(struct hid_device_info *devs)
{
	struct hid_device_info *d = devs;
	while (d) {
		struct hid_device_info *next = d->next;
		free(d->path);
		free(d->serial_number);
		free(d->manufacturer_string);
		free(d->product_string);
		free(d);
		d = next;
	}
}

struct hid_hotplug_callback {
    hid_hotplug_callback_handle handle;
    unsigned short vendor_id;
    unsigned short product_id;
    hid_hotplug_event events;
    void *user_data;
    hid_hotplug_callback_fn callback;

    /* Pointer to the next notification */
    struct hid_hotplug_callback *next;
};

static void hid_internal_invoke_callbacks(struct hid_device_info *info, hid_hotplug_event event)
{
	struct hid_hotplug_callback **current = &hid_hotplug_context.hotplug_cbs;
	while (*current) {
		struct hid_hotplug_callback *callback = *current;
		if ((callback->events & event) && hid_internal_match_device_id(info->vendor_id, info->product_id,
																	   callback->vendor_id, callback->product_id)) {
			int result = callback->callback(callback->handle, info, event, callback->user_data);
			/* If the result is non-zero, we remove the callback and proceed */
			/* Do not use the deregister call as it locks the mutex, and we are currently in a lock */
			if (result) {
				struct hid_hotplug_callback *callback = *current;
				*current = (*current)->next;
				free(callback);
				continue;
			}
		}
		current = &callback->next;
	}
}

static int match_udev_to_info(struct udev_device* raw_dev, struct hid_device_info *info)
{
	const char *path = udev_device_get_devnode(raw_dev);
	if (!strcmp(path, info->path)) {
		return 1;
	}
	return 0;
}

static void* hotplug_thread(void* user_data)
{
	while (hid_hotplug_context.monitor_fd > 0) {
		fd_set fds;
		struct timeval tv;
		int ret;

		FD_ZERO(&fds);
		FD_SET(hid_hotplug_context.monitor_fd, &fds);
		/* 5 msec timeout seems reasonable; don't set too low to avoid high CPU usage */
		/* This timeout only affects how much time it takes to stop the thread */
		tv.tv_sec = 0;
		tv.tv_usec = 5000;

		ret = select(hid_hotplug_context.monitor_fd+1, &fds, NULL, NULL, &tv);

		/* Check if our file descriptor has received data. */
		if (ret > 0 && FD_ISSET(hid_hotplug_context.monitor_fd, &fds)) {

			/* Make the call to receive the device.
			   select() ensured that this will not block. */
			struct udev_device *raw_dev = udev_monitor_receive_device(hid_hotplug_context.mon);
			if (raw_dev) {
				/* Lock the mutex so callback/device lists don't change elsewhere from here on */
				pthread_mutex_lock(&hid_hotplug_context.mutex);

				const char* action = udev_device_get_action(raw_dev);
				if (!strcmp(action, "add")) {
					// We create a list of all usages on this UDEV device
					struct hid_device_info *info = create_device_info_for_device(raw_dev);
					struct hid_device_info *info_cur = info;
					while (info_cur) {
						/* For each device, call all matching callbacks */
						/* TODO: possibly make the `next` field NULL to match the behavior on other systems */
						hid_internal_invoke_callbacks(info_cur, HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED);
						info_cur = info_cur->next;
					}

					/* Append all we got to the end of the device list */
					if (info) {
						if (hid_hotplug_context.devs != NULL) {
							struct hid_device_info *last = hid_hotplug_context.devs;
							while (last->next != NULL) {
								last = last->next;
							}
							last->next = info;
						} else {
							hid_hotplug_context.devs = info;
						}
					}
				} else if (!strcmp(action, "remove")) {
					for (struct hid_device_info **current = &hid_hotplug_context.devs; *current;) {
						struct hid_device_info* info = *current;
						if (match_udev_to_info(raw_dev, *current)) {
							/* If the libusb device that's left matches this HID device, we detach it from the list */
							*current = (*current)->next;
							info->next = NULL;
							hid_internal_invoke_callbacks(info, HID_API_HOTPLUG_EVENT_DEVICE_LEFT);
							/* Free every removed device */
							free(info);
						} else {
							current = &info->next;
						}
					}
				}
				udev_device_unref(raw_dev);
				pthread_mutex_unlock(&hid_hotplug_context.mutex);
			}
		}
	}
	return NULL;
}

int HID_API_EXPORT HID_API_CALL hid_hotplug_register_callback(unsigned short vendor_id, unsigned short product_id, int events, int flags, hid_hotplug_callback_fn callback, void *user_data, hid_hotplug_callback_handle *callback_handle)
{
	struct hid_hotplug_callback* hotplug_cb;

	/* Check params */
	if (events == 0
		|| (events & ~(HID_API_HOTPLUG_EVENT_DEVICE_ARRIVED | HID_API_HOTPLUG_EVENT_DEVICE_LEFT))
		|| (flags & ~(HID_API_HOTPLUG_ENUMERATE))
		|| callback == NULL) {
		return -1;
	}

	hotplug_cb = (struct hid_hotplug_callback*)calloc(1, sizeof(struct hid_hotplug_callback));

	if (hotplug_cb == NULL) {
		return -1;
	}

	/* Fill out the record */
	hotplug_cb->next = NULL;
	hotplug_cb->vendor_id = vendor_id;
	hotplug_cb->product_id = product_id;
	hotplug_cb->events = events;
	hotplug_cb->user_data = user_data;
	hotplug_cb->callback = callback;

	/* TODO: protect the handle by the context hotplug lock */
	hotplug_cb->handle = hid_hotplug_context.next_handle++;

	/* handle the unlikely case of handle overflow */
	if (hid_hotplug_context.next_handle < 0)
	{
		hid_hotplug_context.next_handle = 1;
	}

	/* Return allocated handle */
	if (callback_handle != NULL) {
		*callback_handle = hotplug_cb->handle;
	}

	/* Append a new callback to the end */
	if (hid_hotplug_context.hotplug_cbs != NULL) {
		struct hid_hotplug_callback *last = hid_hotplug_context.hotplug_cbs;
		while (last->next != NULL) {
			last = last->next;
		}
		last->next = hotplug_cb;
	}
	else {
		// Prepare a UDEV context to run monitoring on
		hid_hotplug_context.udev_ctx = udev_new();
		if(!hid_hotplug_context.udev_ctx)
		{
			return -1;
		}

		hid_hotplug_context.mon = udev_monitor_new_from_netlink(hid_hotplug_context.udev_ctx, "udev");
		udev_monitor_filter_add_match_subsystem_devtype(hid_hotplug_context.mon, "hidraw", NULL);
		udev_monitor_enable_receiving(hid_hotplug_context.mon);
		hid_hotplug_context.monitor_fd = udev_monitor_get_fd(hid_hotplug_context.mon);

		/* After monitoring is all set up, enumerate all devices */
		hid_hotplug_context.devs = hid_enumerate(0, 0);

		/* Don't forget to actually register the callback */
		hid_hotplug_context.hotplug_cbs = hotplug_cb;

		/* Start the thread that will be doing the event scanning */
		pthread_create(&hid_hotplug_context.thread, NULL, &hotplug_thread, NULL);
	}

	return -1;
}

int HID_API_EXPORT HID_API_CALL hid_hotplug_deregister_callback(hid_hotplug_callback_handle callback_handle)
{
	pthread_mutex_lock(&hid_hotplug_context.mutex);

	if (hid_hotplug_context.hotplug_cbs == NULL) {
		pthread_mutex_unlock(&hid_hotplug_context.mutex);
		return -1;
	}

	int result = -1;

	/* Remove this notification */
	for (struct hid_hotplug_callback **current = &hid_hotplug_context.hotplug_cbs; *current != NULL; current = &(*current)->next) {
		if ((*current)->handle == callback_handle) {
			struct hid_hotplug_callback *next = (*current)->next;
			free(*current);
			*current = next;
			result = 0;
			break;
		}
	}

	hid_internal_cleanup_hotplugs();

	pthread_mutex_unlock(&hid_hotplug_context.mutex);

	return result;
}

hid_device * hid_open(unsigned short vendor_id, unsigned short product_id, const wchar_t *serial_number)
{
	struct hid_device_info *devs, *cur_dev;
	const char *path_to_open = NULL;
	hid_device *handle = NULL;

	/* register_global_error: global error is reset by hid_enumerate/hid_init */
	devs = hid_enumerate(vendor_id, product_id);
	if (devs == NULL) {
		/* register_global_error: global error is already set by hid_enumerate */
		return NULL;
	}

	cur_dev = devs;
	while (cur_dev) {
		if (cur_dev->vendor_id == vendor_id &&
		    cur_dev->product_id == product_id) {
			if (serial_number) {
				if (wcscmp(serial_number, cur_dev->serial_number) == 0) {
					path_to_open = cur_dev->path;
					break;
				}
			}
			else {
				path_to_open = cur_dev->path;
				break;
			}
		}
		cur_dev = cur_dev->next;
	}

	if (path_to_open) {
		/* Open the device */
		handle = hid_open_path(path_to_open);
	} else {
		register_global_error("Device with requested VID/PID/(SerialNumber) not found");
	}

	hid_free_enumeration(devs);

	return handle;
}

hid_device * HID_API_EXPORT hid_open_path(const char *path)
{
	hid_device *dev = NULL;

	hid_init();
	/* register_global_error: global error is reset by hid_init */

	dev = new_hid_device();
	if (!dev) {
		register_global_error("Couldn't allocate memory");
		return NULL;
	}

	dev->device_handle = open(path, O_RDWR | O_CLOEXEC);

	if (dev->device_handle >= 0) {
		int res, desc_size = 0;

		/* Make sure this is a HIDRAW device - responds to HIDIOCGRDESCSIZE */
		res = ioctl(dev->device_handle, HIDIOCGRDESCSIZE, &desc_size);
		if (res < 0) {
			hid_close(dev);
			register_device_error_format(dev, "ioctl(GRDESCSIZE) error for '%s', not a HIDRAW device?: %s", path, strerror(errno));
			return NULL;
		}

		return dev;
	}
	else {
		/* Unable to open a device. */
		free(dev);
		register_global_error_format("Failed to open a device with path '%s': %s", path, strerror(errno));
		return NULL;
	}
}


int HID_API_EXPORT hid_write(hid_device *dev, const unsigned char *data, size_t length)
{
	int bytes_written;

	if (!data || (length == 0)) {
		errno = EINVAL;
		register_device_error(dev, strerror(errno));
		return -1;
	}

	bytes_written = write(dev->device_handle, data, length);

	register_device_error(dev, (bytes_written == -1)? strerror(errno): NULL);

	return bytes_written;
}


int HID_API_EXPORT hid_read_timeout(hid_device *dev, unsigned char *data, size_t length, int milliseconds)
{
	/* Set device error to none */
	register_device_error(dev, NULL);

	int bytes_read;

	if (milliseconds >= 0) {
		/* Milliseconds is either 0 (non-blocking) or > 0 (contains
		   a valid timeout). In both cases we want to call poll()
		   and wait for data to arrive.  Don't rely on non-blocking
		   operation (O_NONBLOCK) since some kernels don't seem to
		   properly report device disconnection through read() when
		   in non-blocking mode.  */
		int ret;
		struct pollfd fds;

		fds.fd = dev->device_handle;
		fds.events = POLLIN;
		fds.revents = 0;
		ret = poll(&fds, 1, milliseconds);
		if (ret == 0) {
			/* Timeout */
			return ret;
		}
		if (ret == -1) {
			/* Error */
			register_device_error(dev, strerror(errno));
			return ret;
		}
		else {
			/* Check for errors on the file descriptor. This will
			   indicate a device disconnection. */
			if (fds.revents & (POLLERR | POLLHUP | POLLNVAL)) {
				// We cannot use strerror() here as no -1 was returned from poll().
				register_device_error(dev, "hid_read_timeout: unexpected poll error (device disconnected)");
				return -1;
			}
		}
	}

	bytes_read = read(dev->device_handle, data, length);
	if (bytes_read < 0) {
		if (errno == EAGAIN || errno == EINPROGRESS)
			bytes_read = 0;
		else
			register_device_error(dev, strerror(errno));
	}

	return bytes_read;
}

int HID_API_EXPORT hid_read(hid_device *dev, unsigned char *data, size_t length)
{
	return hid_read_timeout(dev, data, length, (dev->blocking)? -1: 0);
}

int HID_API_EXPORT hid_set_nonblocking(hid_device *dev, int nonblock)
{
	/* Do all non-blocking in userspace using poll(), since it looks
	   like there's a bug in the kernel in some versions where
	   read() will not return -1 on disconnection of the USB device */

	dev->blocking = !nonblock;
	return 0; /* Success */
}

int HID_API_EXPORT hid_send_feature_report(hid_device *dev, const unsigned char *data, size_t length)
{
	int res;

	register_device_error(dev, NULL);

	res = ioctl(dev->device_handle, HIDIOCSFEATURE(length), data);
	if (res < 0)
		register_device_error_format(dev, "ioctl (SFEATURE): %s", strerror(errno));

	return res;
}

int HID_API_EXPORT hid_get_feature_report(hid_device *dev, unsigned char *data, size_t length)
{
	int res;

	register_device_error(dev, NULL);

	res = ioctl(dev->device_handle, HIDIOCGFEATURE(length), data);
	if (res < 0)
		register_device_error_format(dev, "ioctl (GFEATURE): %s", strerror(errno));

	return res;
}

int HID_API_EXPORT HID_API_CALL hid_get_input_report(hid_device *dev, unsigned char *data, size_t length)
{
	int res;

	register_device_error(dev, NULL);

	res = ioctl(dev->device_handle, HIDIOCGINPUT(length), data);
	if (res < 0)
		register_device_error_format(dev, "ioctl (GINPUT): %s", strerror(errno));

	return res;
}

void HID_API_EXPORT hid_close(hid_device *dev)
{
	if (!dev)
		return;

	close(dev->device_handle);

	/* Free the device error message */
	register_device_error(dev, NULL);

	hid_free_enumeration(dev->device_info);

	free(dev);
}


int HID_API_EXPORT_CALL hid_get_manufacturer_string(hid_device *dev, wchar_t *string, size_t maxlen)
{
	if (!string || !maxlen) {
		register_device_error(dev, "Zero buffer/length");
		return -1;
	}

	struct hid_device_info *info = hid_get_device_info(dev);
	if (!info) {
		// hid_get_device_info will have set an error already
		return -1;
	}

	if (info->manufacturer_string) {
		wcsncpy(string, info->manufacturer_string, maxlen);
		string[maxlen - 1] = L'\0';
	}
	else {
		string[0] = L'\0';
	}

	return 0;
}

int HID_API_EXPORT_CALL hid_get_product_string(hid_device *dev, wchar_t *string, size_t maxlen)
{
	if (!string || !maxlen) {
		register_device_error(dev, "Zero buffer/length");
		return -1;
	}

	struct hid_device_info *info = hid_get_device_info(dev);
	if (!info) {
		// hid_get_device_info will have set an error already
		return -1;
	}

	if (info->product_string) {
		wcsncpy(string, info->product_string, maxlen);
		string[maxlen - 1] = L'\0';
	}
	else {
		string[0] = L'\0';
	}

	return 0;
}

int HID_API_EXPORT_CALL hid_get_serial_number_string(hid_device *dev, wchar_t *string, size_t maxlen)
{
	if (!string || !maxlen) {
		register_device_error(dev, "Zero buffer/length");
		return -1;
	}

	struct hid_device_info *info = hid_get_device_info(dev);
	if (!info) {
		// hid_get_device_info will have set an error already
		return -1;
	}

	if (info->serial_number) {
		wcsncpy(string, info->serial_number, maxlen);
		string[maxlen - 1] = L'\0';
	}
	else {
		string[0] = L'\0';
	}

	return 0;
}


HID_API_EXPORT struct hid_device_info *HID_API_CALL hid_get_device_info(hid_device *dev) {
	if (!dev->device_info) {
		// Lazy initialize device_info
		dev->device_info = create_device_info_for_hid_device(dev);
	}

	// create_device_info_for_hid_device will set an error if needed
	return dev->device_info;
}

int HID_API_EXPORT_CALL hid_get_indexed_string(hid_device *dev, int string_index, wchar_t *string, size_t maxlen)
{
	(void)string_index;
	(void)string;
	(void)maxlen;

	register_device_error(dev, "hid_get_indexed_string: not supported by hidraw");

	return -1;
}


int HID_API_EXPORT_CALL hid_get_report_descriptor(hid_device *dev, unsigned char *buf, size_t buf_size)
{
	struct hidraw_report_descriptor rpt_desc;
	int res = get_hid_report_descriptor_from_hidraw(dev, &rpt_desc);
	if (res < 0) {
		/* error already registered */
		return res;
	}

	if (rpt_desc.size < buf_size) {
		buf_size = (size_t) rpt_desc.size;
	}

	memcpy(buf, rpt_desc.value, buf_size);

	return (int) buf_size;
}


/* Passing in NULL means asking for the last global error message. */
HID_API_EXPORT const wchar_t * HID_API_CALL  hid_error(hid_device *dev)
{
	if (dev) {
		if (dev->last_error_str == NULL)
			return L"Success";
		return dev->last_error_str;
	}

	if (last_global_error_str == NULL)
		return L"Success";
	return last_global_error_str;
}
