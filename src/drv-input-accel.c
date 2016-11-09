/*
 * Copyright (c) 2014 Bastien Nocera <hadess@hadess.net>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3 as published by
 * the Free Software Foundation.
 */

#include "drivers.h"

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

#include <linux/input.h>

typedef struct DrvData {
	guint              timeout_id;
	ReadingsUpdateFunc callback_func;
	gpointer           user_data;

	GUdevClient *client;
	GUdevDevice *dev, *parent;
	const char *dev_path;
	const char *name;
	gboolean sends_kevent;
} DrvData;

static DrvData *drv_data = NULL;

static void input_accel_set_polling (gboolean state);

static gboolean
input_accel_discover (GUdevDevice *device)
{
	const char *path;

	if (g_strcmp0 (g_udev_device_get_subsystem (device), "input") != 0)
		return FALSE;

	if (!g_udev_device_get_property_as_boolean (device, "ID_INPUT_ACCELEROMETER"))
		return FALSE;

	path = g_udev_device_get_device_file (device);
	if (!path)
		return FALSE;
	if (strstr (path, "/event") == NULL)
		return FALSE;

	g_debug ("Found input accel at %s", g_udev_device_get_sysfs_path (device));
	return TRUE;
}

#define READ_AXIS(axis, var) { memzero(&abs_info, sizeof(abs_info)); r = ioctl(fd, EVIOCGABS(axis), &abs_info); if (r < 0) return; var = abs_info.value; }
#define memzero(x,l) (memset((x), 0, (l)))

static void
accelerometer_changed (void)
{
	struct input_absinfo abs_info;
	int accel_x = 0, accel_y = 0, accel_z = 0;
	int fd, r;
	AccelReadings readings;

	fd = open (drv_data->dev_path, O_RDONLY|O_CLOEXEC);
	if (fd < 0)
		return;

	READ_AXIS(ABS_X, accel_x);
	READ_AXIS(ABS_Y, accel_y);
	READ_AXIS(ABS_Z, accel_z);

	close (fd);

	if (g_strcmp0 ("platform-lis3lv02d", g_udev_device_get_property (drv_data->dev, "ID_PATH")) == 0) {
		/* Quirk for lis3lv02d device,
		 * take opposite x */
		readings.accel_x = -accel_x;
		readings.accel_y = accel_y;
		readings.accel_z = accel_z;
	} else {
		readings.accel_x = accel_x;
		readings.accel_y = accel_y;
		readings.accel_z = accel_z;
	}

	/* Scale from 1G ~= 256 to a value in m/s² */
	readings.scale = 1.0 / 256 * 9.81;

	g_debug ("Accel read from input on '%s': %d, %d, %d (scale %lf)", drv_data->name, accel_x, accel_y, accel_z, readings.scale);

	drv_data->callback_func (&input_accel, (gpointer) &readings, drv_data->user_data);
}

static void
uevent_received (GUdevClient *client,
		 gchar       *action,
		 GUdevDevice *device,
		 gpointer     user_data)
{
	if (g_strcmp0 (action, "change") != 0)
		return;

	if (g_strcmp0 (g_udev_device_get_sysfs_path (device), g_udev_device_get_sysfs_path (drv_data->parent)) != 0)
		return;

	if (!drv_data->sends_kevent) {
		drv_data->sends_kevent = TRUE;
		g_debug ("Received kevent, let's stop polling for accelerometer data on %s", drv_data->dev_path);
		input_accel_set_polling (FALSE);
	}

	accelerometer_changed ();
}

static gboolean
first_values (gpointer user_data)
{
	accelerometer_changed ();
	return G_SOURCE_REMOVE;
}

static gboolean
input_accel_open (GUdevDevice        *device,
		  ReadingsUpdateFunc  callback_func,
		  gpointer            user_data)
{
	const gchar * const subsystems[] = { "input", NULL };

	drv_data = g_new0 (DrvData, 1);
	drv_data->dev = g_object_ref (device);
	drv_data->parent = g_udev_device_get_parent (drv_data->dev);
	drv_data->dev_path = g_udev_device_get_device_file (device);
	drv_data->name = g_udev_device_get_property (device, "NAME");
	drv_data->client = g_udev_client_new (subsystems);

	drv_data->callback_func = callback_func;
	drv_data->user_data = user_data;

	g_signal_connect (drv_data->client, "uevent",
			  G_CALLBACK (uevent_received), NULL);

	g_idle_add (first_values, NULL);

	return TRUE;
}

static gboolean
read_accel_poll (gpointer user_data)
{
	accelerometer_changed ();
	return G_SOURCE_CONTINUE;
}

static void
input_accel_set_polling (gboolean state)
{
	if (drv_data->timeout_id > 0 && state)
		return;
	if (drv_data->timeout_id == 0 && !state)
		return;

	if (drv_data->timeout_id) {
		g_source_remove (drv_data->timeout_id);
		drv_data->timeout_id = 0;
	}

	if (state && !drv_data->sends_kevent) {
		drv_data->timeout_id = g_timeout_add (700, read_accel_poll, drv_data);
		g_source_set_name_by_id (drv_data->timeout_id, "[input_accel_set_polling] read_accel_poll");
	}
}

static void
input_accel_close (void)
{
	input_accel_set_polling (FALSE);
	g_clear_object (&drv_data->client);
	g_clear_object (&drv_data->dev);
	g_clear_object (&drv_data->parent);
	g_clear_pointer (&drv_data->dev_path, g_free);

	g_clear_pointer (&drv_data, g_free);
}

SensorDriver input_accel = {
	.name = "Input accelerometer",
	.type = DRIVER_TYPE_ACCEL,
	.specific_type = DRIVER_TYPE_ACCEL_INPUT,

	.discover = input_accel_discover,
	.open = input_accel_open,
	.set_polling = input_accel_set_polling,
	.close = input_accel_close,
};
