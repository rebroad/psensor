/*
 * Copyright (C) 2010-2016 jeanfi@gmail.com
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */
#include <locale.h>
#include <libintl.h>
#define _(str) gettext(str)

#include <string.h>

#include <glibtop/cpu.h>
#include <glibtop/mem.h>
#include <dirent.h>

#include <pgtop2.h>
#include <plog.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static float last_used;
static float last_total;

/* CPU spike detection: track average and only log spikes */
#define CPU_AVG_SAMPLES 60  /* Track last 60 samples for average */
static double cpu_samples[CPU_AVG_SAMPLES];
static int cpu_sample_idx = 0;
static int cpu_samples_count = 0;
static double cpu_avg = 0.0;
#define CPU_SPIKE_THRESHOLD 1.5  /* Log if CPU > 1.5x average */

static const char *PROVIDER_NAME = "gtop2";

struct psensor *create_cpu_usage_sensor(int measures_len)
{
	char *label, *id;
	int type;
	struct psensor *psensor;

	id = g_strdup_printf("%s cpu usage", PROVIDER_NAME);
	label = strdup(_("CPU usage"));
	type = SENSOR_TYPE_GTOP | SENSOR_TYPE_CPU_USAGE;

	psensor = psensor_create(id,
				 label,
				 strdup(_("CPU")),
				 type,
				 measures_len);

	return psensor;
}

static struct psensor *create_mem_free_sensor(int measures_len)
{
	char *id;
	int type;

	id = g_strdup_printf("%s mem free", PROVIDER_NAME);
	type = SENSOR_TYPE_GTOP | SENSOR_TYPE_MEMORY | SENSOR_TYPE_PERCENT;

	return psensor_create(id,
				  strdup(_("free memory")),
				  strdup(_("memory")),
				  type,
				  measures_len);
}

static double get_usage(void)
{
	glibtop_cpu cpu;
	unsigned long int used, dt;
	double cpu_rate;

	glibtop_get_cpu(&cpu);

	used = cpu.user + cpu.nice + cpu.sys;

	dt = cpu.total - last_total;

	if (dt)
		cpu_rate = 100.0 * (used - last_used) / dt;
	else
		cpu_rate = UNKNOWN_DBL_VALUE;

	last_used = used;
	last_total = cpu.total;

	return cpu_rate;
}

static double get_mem_free(void)
{
	glibtop_mem mem;
	double v;

	glibtop_get_mem(&mem);
	v = ((double)mem.free) * 100.0 / mem.total;

	return v;
}

void gtop2_psensor_list_append(struct psensor ***sensors, int measures_len)
{
	psensor_list_append(sensors, create_cpu_usage_sensor(measures_len));
	psensor_list_append(sensors, create_mem_free_sensor(measures_len));
}

/* Structure to hold process CPU info */
struct proc_cpu_info {
	pid_t pid;
	char comm[256];
	double cpu_percent;
	double cpu_avg;  /* Average CPU usage for this process */
};

static int compare_proc_cpu(const void *a, const void *b)
{
	const struct proc_cpu_info *pa = (const struct proc_cpu_info *)a;
	const struct proc_cpu_info *pb = (const struct proc_cpu_info *)b;
	if (pa->cpu_percent > pb->cpu_percent) return -1;
	if (pa->cpu_percent < pb->cpu_percent) return 1;
	return 0;
}

/* Track process CPU times between samples */
#define MAX_TRACKED_PROCS 200
#define PROC_AVG_SAMPLES 20  /* Track last 20 samples for per-process average */
static struct {
	pid_t pid;
	unsigned long utime;
	unsigned long stime;
	char comm[32];
	double cpu_samples[PROC_AVG_SAMPLES];
	int cpu_sample_idx;
	int cpu_samples_count;
	double cpu_avg;
} proc_times[MAX_TRACKED_PROCS];
static int proc_times_count = 0;
static unsigned long last_total_cpu_time = 0;
static int times_initialized = 0;
#define PROC_SPIKE_THRESHOLD 2.0  /* Show process if CPU > 2x its average */

/* Helper function to check if a process still exists */
static int proc_exists(pid_t pid)
{
	char path[256];
	snprintf(path, sizeof(path), "/proc/%d/stat", pid);
	FILE *fp = fopen(path, "r");
	if (fp) {
		fclose(fp);
		return 1;
	}
	return 0;
}

/* Clean up dead processes from tracking array */
static void cleanup_dead_procs(void)
{
	int write_idx = 0;
	for (int read_idx = 0; read_idx < proc_times_count; read_idx++) {
		if (proc_exists(proc_times[read_idx].pid)) {
			/* Process still exists, keep it */
			if (write_idx != read_idx) {
				proc_times[write_idx] = proc_times[read_idx];
			}
			write_idx++;
		}
		/* If process doesn't exist, we skip it (don't copy to write_idx) */
	}
	proc_times_count = write_idx;
}

/* Helper function to initialize a new process entry */
static int init_new_proc_entry(pid_t pid, unsigned long utime, unsigned long stime, const char *comm)
{
	/* If we're at max capacity, try to clean up dead processes first */
	if (proc_times_count >= MAX_TRACKED_PROCS) {
		cleanup_dead_procs();
		/* If still at max after cleanup, we can't track more */
		if (proc_times_count >= MAX_TRACKED_PROCS)
			return -1;
	}

	int idx = proc_times_count;
	proc_times[idx].pid = pid;
	proc_times[idx].utime = utime;
	proc_times[idx].stime = stime;
	snprintf(proc_times[idx].comm, sizeof(proc_times[idx].comm), "%s", comm);
	proc_times[idx].cpu_sample_idx = 0;
	proc_times[idx].cpu_samples_count = 0;
	proc_times[idx].cpu_avg = 0.0;
	proc_times_count++;
	return idx;
}

static void log_top_cpu_processes_sync(int during_spike)
{
	FILE *fp;
	char line[1024];
	char path[256];
	pid_t pid;
	unsigned long utime, stime;
	unsigned long total_cpu_time = 0;
	struct proc_cpu_info *procs = NULL;
	int proc_count = 0;
	int proc_capacity = 0;
	DIR *dir;
	struct dirent *entry;

	/* Read /proc/stat for total CPU time */
	fp = fopen("/proc/stat", "r");
	if (fp == NULL) return;

	while (fgets(line, sizeof(line), fp)) {
		if (strncmp(line, "cpu ", 4) == 0) {
			unsigned long user, nice, sys, idle, iowait, irq, softirq;
			if (sscanf(line, "cpu %lu %lu %lu %lu %lu %lu %lu",
				   &user, &nice, &sys, &idle, &iowait, &irq, &softirq) == 7) {
				total_cpu_time = user + nice + sys + idle + iowait + irq + softirq;
			}
			break;
		}
	}
	fclose(fp);

	if (total_cpu_time == 0) return;

	/* Scan /proc for process directories - limit to reasonable number */
	dir = opendir("/proc");
	if (dir == NULL) return;

	int scanned = 0;
	while ((entry = readdir(dir)) != NULL && scanned < 500) {
		if (entry->d_name[0] < '0' || entry->d_name[0] > '9')
			continue;

		pid = atoi(entry->d_name);
		if (pid <= 0) continue;
		scanned++;

		snprintf(path, sizeof(path), "/proc/%d/stat", pid);
		fp = fopen(path, "r");
		if (fp == NULL) continue;

		if (fgets(line, sizeof(line), fp)) {
			/* Parse: pid (comm) state ppid ... utime stime ... */
			/* Find the closing paren of comm field */
			char *paren_end = strrchr(line, ')');
			if (paren_end) {
				/* utime is 2 fields after the closing paren */
				if (sscanf(paren_end + 2, "%*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %lu %lu",
				   &utime, &stime) == 2) {

					/* Get command name */
					char comm[32] = "unknown";
					snprintf(path, sizeof(path), "/proc/%d/comm", pid);
					FILE *comm_fp = fopen(path, "r");
					if (comm_fp) {
						if (fgets(comm, sizeof(comm), comm_fp)) {
							char *nl = strchr(comm, '\n');
							if (nl) *nl = '\0';
						}
						fclose(comm_fp);
					}

					/* Calculate CPU % since last sample */
					double cpu_percent = 0.0;
					int proc_idx = -1;

					if (times_initialized && last_total_cpu_time > 0) {
						unsigned long dt_total = total_cpu_time - last_total_cpu_time;
						unsigned long proc_time = utime + stime;

						/* Find previous time for this PID */
						unsigned long last_proc_time = 0;
						for (int i = 0; i < proc_times_count; i++) {
							if (proc_times[i].pid == pid) {
								proc_idx = i;
								last_proc_time = proc_times[i].utime + proc_times[i].stime;
								/* Update stored time */
								proc_times[i].utime = utime;
								proc_times[i].stime = stime;
								break;
							}
						}

						if (last_proc_time == 0) {
							/* New process, add it */
							proc_idx = init_new_proc_entry(pid, utime, stime, comm);
						} else if (last_proc_time > 0 && proc_idx >= 0) {
							unsigned long dt_proc = proc_time - last_proc_time;
							if (dt_total > 0) {
								cpu_percent = 100.0 * dt_proc / dt_total;

								/* Save the PREVIOUS average and sample count before updating (for threshold check) */
								double prev_avg = proc_times[proc_idx].cpu_avg;
								int prev_samples_count = proc_times[proc_idx].cpu_samples_count;

								/* Update per-process average */
								proc_times[proc_idx].cpu_samples[proc_times[proc_idx].cpu_sample_idx] = cpu_percent;
								proc_times[proc_idx].cpu_sample_idx =
									(proc_times[proc_idx].cpu_sample_idx + 1) % PROC_AVG_SAMPLES;
								if (proc_times[proc_idx].cpu_samples_count < PROC_AVG_SAMPLES)
									proc_times[proc_idx].cpu_samples_count++;

								/* Calculate per-process average */
								double sum = 0.0;
								for (int j = 0; j < proc_times[proc_idx].cpu_samples_count; j++) {
									sum += proc_times[proc_idx].cpu_samples[j];
								}
								proc_times[proc_idx].cpu_avg =
									sum / proc_times[proc_idx].cpu_samples_count;

								/* Check if process should be shown */
								int should_show = 0;
								if (cpu_percent > 0.01) {
									if (during_spike) {
										if (cpu_percent > prev_avg) {
											should_show = 1;
										}
									}

									if (should_show) {
										if (proc_count >= proc_capacity) {
											proc_capacity = proc_capacity ? proc_capacity * 2 : 64;
											procs = realloc(procs, proc_capacity * sizeof(struct proc_cpu_info));
											if (!procs) {
												fclose(fp);
												break;
											}
										}
										procs[proc_count].pid = pid;
										procs[proc_count].cpu_percent = cpu_percent;
										snprintf(procs[proc_count].comm, sizeof(procs[proc_count].comm), "%s", comm);
										if (prev_samples_count >= 5 && prev_avg > 0.0) {
											procs[proc_count].cpu_avg = prev_avg;  /* Use previous average for display */
										} else {
											procs[proc_count].cpu_avg = 0.0;  /* New process, no history */
										}
										proc_count++;
									}
								}
							}
						}
					} else {
						/* First time (times_initialized == 0), just store */
						proc_idx = init_new_proc_entry(pid, utime, stime, comm);
					}
				}
			}
		}
		fclose(fp);
	}
	closedir(dir);

	/* Sort by CPU usage and log processes */
	if (proc_count > 0) {
		qsort(procs, proc_count, sizeof(struct proc_cpu_info), compare_proc_cpu);

		log_info("Top CPU processes:");
		int top_count = proc_count < 5 ? proc_count : 5;
		for (int i = 0; i < top_count; i++) {
			if (procs[i].cpu_avg > 0.0) {
				double factor = procs[i].cpu_percent / procs[i].cpu_avg;
				log_info("  PID %d (%s): %.1f%% (avg=%.2f%%, %.1fx above avg)",
					procs[i].pid, procs[i].comm, procs[i].cpu_percent,
					procs[i].cpu_avg, factor);
			} else {
				log_info("  PID %d (%s): %.1f%% (new)",
					procs[i].pid, procs[i].comm, procs[i].cpu_percent);
			}
		}
	}

	/* Update tracking state AFTER processing */
	last_total_cpu_time = total_cpu_time;
	times_initialized = 1;

	if (procs) free(procs);
}

void cpu_usage_sensor_update(struct psensor *s)
{
	double v;
	static int update_count = 0;

	v = get_usage();

	if (v != UNKNOWN_DBL_VALUE) {
		psensor_set_current_value(s, v);

		/* Update running average */
		cpu_samples[cpu_sample_idx] = v;
		cpu_sample_idx = (cpu_sample_idx + 1) % CPU_AVG_SAMPLES;
		if (cpu_samples_count < CPU_AVG_SAMPLES)
			cpu_samples_count++;

		/* Calculate average */
		double sum = 0.0;
		for (int i = 0; i < cpu_samples_count; i++) {
			sum += cpu_samples[i];
		}
		cpu_avg = sum / cpu_samples_count;

		/* Update process tracking every 10 samples to keep data fresh */
		/* This ensures we have recent data when a spike occurs */
		update_count++;
		if (update_count % 10 == 0) {
			log_top_cpu_processes_sync(0);  /* Regular tracking: only show processes > 2x average */
		}

		/* Log spike if CPU is significantly above average */
		if (cpu_samples_count >= 10 && v > cpu_avg * CPU_SPIKE_THRESHOLD && v > 10.0) {
			log_info("CPU spike detected: usage=%.1f%% (avg=%.1f%%, %.1fx above avg)",
				v, cpu_avg, v / cpu_avg);
			log_top_cpu_processes_sync(1);  /* During spike: also show new processes */
		}
	}
}

static void mem_free_sensor_update(struct psensor *s)
{
	double v;

	v = get_mem_free();

	if (v != UNKNOWN_DBL_VALUE)
		psensor_set_current_value(s, v);
}

void gtop2_psensor_list_update(struct psensor **sensors)
{
	struct psensor *s;

	while (*sensors) {
		s = *sensors;

		if (!(s->type & SENSOR_TYPE_REMOTE)
			&& s->type & SENSOR_TYPE_GTOP) {
			if (s->type & SENSOR_TYPE_CPU)
				cpu_usage_sensor_update(s);
			else if (s->type & SENSOR_TYPE_MEMORY)
				mem_free_sensor_update(s);
		}

		sensors++;
	}
}
