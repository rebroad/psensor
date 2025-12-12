/*
 * Copyright (C) 2024
 * Phone Temperature Sensor Provider for psensor
 * Reads phone temperature from file created by phone_temp_sensor.sh daemon
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 */

#include <locale.h>
#include <libintl.h>
#define _(str) gettext(str)

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <pwd.h>

#include <phone_sensor.h>
#include <psensor.h>

static struct psensor *phone_temp_sensor = NULL;
static struct psensor *phone_battery_sensor = NULL;

/* Get home directory */
static const char *get_home_dir(void)
{
	const char *home = getenv("HOME");
	if (!home) {
		struct passwd *pw = getpwuid(getuid());
		if (pw)
			home = pw->pw_dir;
	}
	return home;
}

/* Get temperature file path */
static char *get_temp_file_path(void)
{
	const char *home = get_home_dir();
	char *path;
	int len;

	if (!home)
		return NULL;

	len = strlen(home) + 50;  /* enough for path */
	path = malloc(len);
	if (!path)
		return NULL;

	snprintf(path, len, "%s/.local/share/phone-sensor/temp1_input", home);
	return path;
}

/* Get battery file path */
static char *get_battery_file_path(void)
{
	const char *home = get_home_dir();
	char *path;
	int len;

	if (!home)
		return NULL;

	len = strlen(home) + 50;  /* enough for path */
	path = malloc(len);
	if (!path)
		return NULL;

	snprintf(path, len, "%s/.local/share/phone-sensor/battery_level", home);
	return path;
}

/* Read temperature from file (in millidegrees Celsius) */
static double read_phone_temperature(void)
{
	char *path;
	FILE *f;
	double temp = UNKNOWN_DBL_VALUE;
	long temp_milli;

	path = get_temp_file_path();
	if (!path)
		return UNKNOWN_DBL_VALUE;

	f = fopen(path, "r");
	free(path);

	if (!f)
		return UNKNOWN_DBL_VALUE;

	if (fscanf(f, "%ld", &temp_milli) == 1) {
		/* -1 is sentinel for invalid/no data, temperature must be > 0 */
		if (temp_milli > 0) {
			/* Convert from millidegrees to degrees Celsius */
			temp = (double)temp_milli / 1000.0;
		}
	}

	fclose(f);
	return temp;
}

/* Read battery level from file (0-100) */
static double read_phone_battery(void)
{
	char *path;
	FILE *f;
	double battery = UNKNOWN_DBL_VALUE;
	int level;

	path = get_battery_file_path();
	if (!path)
		return UNKNOWN_DBL_VALUE;

	f = fopen(path, "r");
	free(path);

	if (!f)
		return UNKNOWN_DBL_VALUE;

	if (fscanf(f, "%d", &level) == 1) {
		/* -1 is sentinel for invalid/no data, 0-100 are valid battery levels */
		if (level >= 0 && level <= 100) {
			battery = (double)level;
		}
	}

	fclose(f);
	return battery;
}

/* Check if phone sensor file exists */
static int phone_sensor_available(void)
{
	char *path;
	int exists = 0;
	FILE *f;

	path = get_temp_file_path();
	if (!path)
		return 0;

	f = fopen(path, "r");
	if (f) {
		exists = 1;
		fclose(f);
	}

	free(path);
	return exists;
}

/* Create phone temperature sensor */
static struct psensor *create_phone_temp_sensor(int values_max_length)
{
	int t;
	char *id, *name;

	if (phone_sensor_available() == 0)
		return NULL;

	t = SENSOR_TYPE_TEMP;
	id = strdup("phone-sensor-temperature");
	name = strdup(_("Phone Temperature"));

	phone_temp_sensor = psensor_create(id, name, strdup("Phone"), t, values_max_length);
	return phone_temp_sensor;
}

/* Create phone battery level sensor */
static struct psensor *create_phone_battery_sensor(int values_max_length)
{
	int t;
	char *id, *name;
	char *battery_path;

	battery_path = get_battery_file_path();
	if (!battery_path)
		return NULL;

	/* Check if battery file exists */
	FILE *f = fopen(battery_path, "r");
	free(battery_path);
	if (!f)
		return NULL;
	fclose(f);

	t = SENSOR_TYPE_PERCENT;
	id = strdup("phone-sensor-battery-level");
	name = strdup(_("Phone Battery Level"));

	phone_battery_sensor = psensor_create(id, name, strdup("Phone"), t, values_max_length);
	return phone_battery_sensor;
}

void phone_sensor_psensor_list_append(struct psensor ***sensors, int values_length)
{
	struct psensor *s;

	/* Try to create phone temperature sensor */
	s = create_phone_temp_sensor(values_length);
	if (s) {
		psensor_list_append(sensors, s);
	}

	/* Try to create phone battery level sensor */
	s = create_phone_battery_sensor(values_length);
	if (s) {
		psensor_list_append(sensors, s);
	}
}

void phone_sensor_psensor_list_update(struct psensor **sensors)
{
	struct psensor **sensor_cur;
	double temp, battery;

	if (!sensors)
		return;

	/* Update phone sensors in list */
	sensor_cur = sensors;
	while (*sensor_cur) {
		if (strcmp((*sensor_cur)->id, "phone-sensor-temperature") == 0) {
			temp = read_phone_temperature();
			if (temp != UNKNOWN_DBL_VALUE) {
				psensor_set_current_value(*sensor_cur, temp);
			}
		} else if (strcmp((*sensor_cur)->id, "phone-sensor-battery-level") == 0) {
			battery = read_phone_battery();
			if (battery != UNKNOWN_DBL_VALUE) {
				psensor_set_current_value(*sensor_cur, battery);
			}
		}
		sensor_cur++;
	}
}

