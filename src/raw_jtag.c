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

/* This file implements parsing and execution of jtag tokens
 *
 * Jtag Tokens are sent in the form
 * mon raw(_jtag) <PAYLOAD>
 *
 * Where Payload is:
 * uint32_t 0xaa550000 | (~CMD & 0xff) << 8 | CMD
 * uint16_t length in ticks
 * uint8_t[(ticks+7)>>3] if there is TDI Data to transmit
 *
 * TODO: Allow multiple payloads per transfer to spare USB turn arounds!
 *
 * xs3sprog has a "blackmagic" branch to talk this protocol
 */
#include <stdio.h>
#include <stdlib.h>

#include "general.h"

#include "general.h"
#include "platform.h"
#include "jtagtap.h"
#include "raw_jtag.h"

#include "gdb_packet.h"

#include "target.h"

bool cmd_raw_jtag(target *t, int argc, char **argv)
{
    (void) t;
    uint8_t * token = argv[1];
    uint16_t ticks;
    const uint8_t *jtag_data_in;
    uint8_t *jtag_data_out, final;

    if (argc >2)
        return false;

    ticks = token[4]<<8| token[5];
    jtag_data_in = (char*) & token[6];
    if ((token[0] != 0xaa) || (token[1] != 0x55) || ((~token[2] &0xff) != token[3]))
        return false;

    final = token[3] & RAW_JTAG_FINAL;
    token[3] &= ~RAW_JTAG_FINAL;

    switch(token[3])
    {
    case RAW_JTAG_INIT:
        jtagtap_init();
        break;
    case RAW_JTAG_DELAY:
        platform_udelay(ticks);
        break;
    case RAW_JTAG_TMS:
    {
        uint32_t tms_seq = 0;
        int i;
        for (i = 0; i < ((ticks +7) >> 3); i++)
            tms_seq |= (*jtag_data_in++ << (i*8));
        jtagtap_tms_seq(tms_seq, ticks);
        break;
    }
    case RAW_JTAG_TDI:
        jtagtap_tdi_seq(final, jtag_data_in, ticks);
        break;
    case RAW_JTAG_TDO:
        jtag_data_in = 0;
        /* Fall through*/
    case RAW_JTAG_TDITDO:
    {
        int len = (ticks + 7)>>3;
        jtag_data_out = alloca(len);
        if (jtag_data_out == 0) {
            return false;
        }
        jtagtap_tdi_tdo_seq(jtag_data_out, final, jtag_data_in, ticks);
        gdb_n_out(jtag_data_out, len);
        break;
    }
    default:
        return false;
    }
    return true;
}
