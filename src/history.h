/*
 * Copyright (C) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */
#ifndef _PSENSOR_HISTORY_H_
#define _PSENSOR_HISTORY_H_

#include <psensor.h>

void history_load(struct psensor **sensors, int duration_minutes);
void history_save(struct psensor **sensors, int duration_minutes);

#endif
