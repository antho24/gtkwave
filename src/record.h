/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#ifndef WAVE_RECORD_H
#define WAVE_RECORD_H

#include "analyzer.h"

/*
 * VHDL record groups: the begin trace of a group flagged with TR_RECORD
 * shows the composite value of the traces in the group, e.g.
 * "{1 05 run {0 3}}".  The composite history is synthesized from the field
 * traces, so it follows their display formats.  In save files the begin
 * trace is stored as a blank group begin and rebuilt when the group is
 * loaded.
 */

void record_trace_build(GwTrace *t);
void record_trace_free(GwTrace *t);
void record_traces_refresh(void);
TraceFlagsType record_trace_save_flags(GwTrace *t);

#endif
