/*
 * This file and its contents are licensed under the Timescale License.
 * Please see the included NOTICE for copyright information and
 * LICENSE-TIMESCALE for a copy of the license.
 */
#pragma once

#include <postgres.h>
#include <foreign/foreign.h>

extern int set_transmission_modes(void);
extern void reset_transmission_modes(int nestlevel);
