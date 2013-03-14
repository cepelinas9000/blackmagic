/*
 * This file is part of the Black Magic Debug project.
 *
 * Copyright (C) 2013 Uwe Bonnes (bon@elektron.ikp.physik.tu-darmstadt.de
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __RAW_JTAG_H
#define __RAW_JTAG_H

#include "general.h"

#include "target.h"

bool cmd_raw_jtag(target *t, int argc, char **argv);
#define RAW_JTAG_INIT   1
#define RAW_JTAG_DELAY  2
#define RAW_JTAG_TMS    3
#define RAW_JTAG_TDI    4
#define RAW_JTAG_TDO    5
#define RAW_JTAG_TDITDO 6

#define RAW_JTAG_FINAL 0x80

#endif
