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

#ifndef _PSENSOR_PHONE_SENSOR_H_
#define _PSENSOR_PHONE_SENSOR_H_

#include <bool.h>
#include <psensor.h>

void phone_sensor_psensor_list_append(struct psensor ***sensors, int values_length);
void phone_sensor_psensor_list_update(struct psensor **sensors);

#endif

