/*
 * Copyright (C) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <glib.h>

#include <history.h>

static char *history_path(void)
{
	char *dir = g_build_filename(g_get_user_data_dir(), "psensor", NULL);
	g_mkdir_with_parents(dir, 0700);
	char *path = g_build_filename(dir, "history.ini", NULL);
	g_free(dir);
	return path;
}

static void history_group_name(char *group, gsize size, int index)
{
	g_snprintf(group, size, "sensor-%d", index);
}

void history_load(struct psensor **sensors, int duration_minutes)
{
	GKeyFile *file = g_key_file_new();
	GError *error = NULL;
	char *path = history_path();
	time_t cutoff = time(NULL) - (time_t)duration_minutes * 60;

	if (!g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, &error)) {
		g_clear_error(&error);
		goto done;
	}

	gsize group_count = 0;
	char **groups = g_key_file_get_groups(file, &group_count);
	for (int index = 0; sensors && sensors[index]; index++) {
		struct psensor *sensor = sensors[index];
		const char *group = NULL;
		char *id;
		gsize count = 0;
		char **records;
		struct measure *loaded;
		int loaded_count = 0;

		for (gsize j = 0; j < group_count; j++) {
			id = g_key_file_get_string(file, groups[j], "id", NULL);
			if (id && strcmp(id, sensor->id) == 0)
				group = groups[j];
			g_free(id);
			if (group)
				break;
		}
		if (!group)
			continue;

		records = g_key_file_get_string_list(file, group, "samples",
							 &count, NULL);
		loaded = g_new0(struct measure, count);
		for (gsize i = 0; i < count; i++) {
			char *separator = strchr(records[i], ',');
			char *end;
			long long timestamp;
			double value;

			if (!separator)
				continue;
			*separator = '\0';
			timestamp = g_ascii_strtoll(records[i], &end, 10);
			if (end == records[i] || *end || timestamp <= cutoff)
				continue;
			value = g_ascii_strtod(separator + 1, &end);
			if (end == separator + 1 || *end || !isfinite(value))
				continue;
			loaded[loaded_count].time.tv_sec = (time_t)timestamp;
			loaded[loaded_count].time.tv_usec = 0;
			loaded[loaded_count].value = value;
			loaded_count++;
		}

		if (sensor->values_max_length <= 1)
			loaded_count = 0;
		else if (loaded_count > sensor->values_max_length - 1)
			loaded_count = sensor->values_max_length - 1;
		if (loaded_count > 0) {
			size_t copy_count = (size_t)loaded_count;
			memcpy(&sensor->measures[sensor->values_max_length - loaded_count],
				   loaded, copy_count * sizeof(*loaded));
			sensor->sess_lowest = loaded[0].value;
			sensor->sess_highest = loaded[0].value;
			for (int i = 1; i < loaded_count; i++) {
				if (loaded[i].value < sensor->sess_lowest)
					sensor->sess_lowest = loaded[i].value;
				if (loaded[i].value > sensor->sess_highest)
					sensor->sess_highest = loaded[i].value;
			}
		}
		g_strfreev(records);
		g_free(loaded);
	}
	g_strfreev(groups);

done:
	g_free(path);
	g_key_file_unref(file);
}

void history_save(struct psensor **sensors, int duration_minutes)
{
	GKeyFile *file = g_key_file_new();
	char *path = history_path();
	time_t cutoff = time(NULL) - (time_t)duration_minutes * 60;

	for (int index = 0; sensors && sensors[index]; index++) {
		struct psensor *sensor = sensors[index];
		char group[32];
		GPtrArray *records = g_ptr_array_new_with_free_func(g_free);

		history_group_name(group, sizeof(group), index);
		g_key_file_set_string(file, group, "id", sensor->id);
		for (int i = 0; i < sensor->values_max_length; i++) {
			struct measure *measure = &sensor->measures[i];
			char *record;

			if (!measure->time.tv_sec || measure->time.tv_sec <= cutoff
				|| measure->value == UNKNOWN_DBL_VALUE)
				continue;
			char value[64];
			g_ascii_formatd(value, sizeof(value), "%.17g", measure->value);
			record = g_strdup_printf("%lld,%s",
						 (long long)measure->time.tv_sec, value);
			g_ptr_array_add(records, record);
		}
		g_ptr_array_add(records, NULL);
		g_key_file_set_string_list(file, group, "samples",
					   (const gchar * const *)records->pdata,
					   records->len - 1);
		g_ptr_array_unref(records);
	}

	gsize length;
	char *contents = g_key_file_to_data(file, &length, NULL);
	if (!g_file_set_contents(path, contents, length, NULL))
		g_warning("Could not write sensor history to %s", path);
	g_free(contents);
	g_free(path);
	g_key_file_unref(file);
}
