/****************************************************************************\
 *  sinfo.h - definitions used for sinfo data functions
 *****************************************************************************
 *  Copyright (C) 2002 The Regents of the University of California.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Joey Ekstrom <ekstrom1@llnl.gov>, Moe Jette <jette1@llnl.gov>
 *  UCRL-CODE-2002-040.
 *
 *  This file is part of SLURM, a resource management program.
 *  For details, see <http://www.llnl.gov/linux/slurm/>.
 *
 *  SLURM is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA  02111-1307  USA.
\*****************************************************************************/

#ifndef __SINFO_H__

#if HAVE_CONFIG_H
#  include "config.h"
#endif

#include <ctype.h>
#include <stdio.h>

#if HAVE_INTTYPES_H
#  include <inttypes.h>
#else  /* !HAVE_INTTYPES_H */
#  if HAVE_STDINT_H
#    include <stdint.h>
#  endif
#endif  /* HAVE_INTTYPES_H */

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <slurm/slurm.h>

#include "src/common/hostlist.h"
#include "src/common/list.h"
#include "src/common/log.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/xmalloc.h"


struct sinfo_parameters {
	char* partition;
	bool state_flag;
	enum node_states state;
	bool node_flag;
	char* nodes;
	bool summarize;
	bool long_output;
	bool line_wrap;
	int verbose;
	int iterate;
	bool exact_match;
};

struct node_state_summary {
	hostlist_t nodes;
	enum node_states state;
	uint32_t node_count;
};

struct partition_summary {
	partition_info_t* info;
	List states;
};

int parse_state( char* str, enum job_states* states );
int parse_command_line( int argc, char* argv[] );
void print_options( void );

#endif
