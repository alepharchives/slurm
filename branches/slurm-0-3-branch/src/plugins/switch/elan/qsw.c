/*****************************************************************************\
 *  qsw.c - Library routines for initiating jobs on QsNet. 
 *****************************************************************************
 *  Copyright (C) 2002 The Regents of the University of California.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Jim Garlick <garlick@llnl.gov>
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

#if     HAVE_CONFIG_H
#  include "config.h"
#endif

#ifdef WITH_PTHREADS
#  include <pthread.h>
#endif /* WITH_PTHREADS */

#include <sys/param.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <syslog.h>
#include <errno.h>
#include <string.h>
#include <paths.h>
#include <stdarg.h>
#include <ctype.h>
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>	/* INT_MAX */
#include <stdio.h>


#if HAVE_LIBELANCTRL
# include <elan/elanctrl.h>
# include <elan/capability.h>

/* These are taken from elan3/elanvp.h, which we don't
 *  want to include here since we are using the new
 *  version-nonspecific libelanctrl.
 *  (XXX: What is the equivalent in libelanctrl?)
 *
 * slurm/482: the elan USER context range is now split
 *  into two segments, regular user context and RMS 
 *  context ranges. Do not allow a context range
 *  (lowcontext -- highcontext) to span these two segments,
 *  as this will generate and elan initialization error
 *  when MPI tries to attach to the capability. For now,
 *  restrict SLURM's range to the RMS one (starting at 0x400)
 * 
 */
# define ELAN_USER_BASE_CONTEXT_NUM    0x400 /* act. RMS_BASE_CONTEXT_NUM */
# define ELAN_USER_TOP_CONTEXT_NUM     0x7ff

# define Version      cap_version
# define HighNode     cap_highnode
# define LowNode      cap_lownode
# define HighContext  cap_highcontext
# define LowContext   cap_lowcontext
# define MyContext    cap_mycontext
# define Bitmap       cap_bitmap
# define Type         cap_type
# define UserKey      cap_userkey
# define RailMask     cap_railmask
# define Values       key_values
#elif HAVE_LIBELAN3 
# include <elan3/elan3.h>
# include <elan3/elanvp.h>
#else
# error "Must have either libelan3 or libelanctrl to compile this module!"
#endif /* HAVE_LIBELANCTRL */

#include <rms/rmscall.h>
#include <elanhosts.h>

#include <slurm/slurm_errno.h>

#include "src/common/slurm_xlator.h"

#include "src/plugins/switch/elan/qsw.h"

/*
 * Definitions local to this module.
 */
#define QSW_JOBINFO_MAGIC 	0xf00ff00e
#define QSW_LIBSTATE_MAGIC 	0xf00ff00f

/* we will allocate program descriptions in this range */
/* XXX note: do not start at zero as libelan shifts to get unique shm id */
#define QSW_PRG_START  		1
#define QSW_PRG_END    		INT_MAX
#define QSW_PRG_INVAL		(-1)

/* we allocate elan hardware context numbers in this range */
#define QSW_CTX_START		ELAN_USER_BASE_CONTEXT_NUM

/* XXX: Temporary workaround for slurm/222 (qws sw-kernel/5478) 
 *      (sys_validate_cap does not allow ELAN_USER_TOP_CONTEXT_NUM)
 */
#define QSW_CTX_END		ELAN_USER_TOP_CONTEXT_NUM - 1 
#define QSW_CTX_INVAL		(-1)

/* 
 * We are going to some trouble to keep these defs private so slurm
 * hackers not interested in the interconnect details can just pass around
 * the opaque types.  All use of the data structure internals is local to this
 * module.
 */
struct qsw_libstate {
	int ls_magic;
	int ls_prognum;
	int ls_hwcontext;
};

struct qsw_jobinfo {
	int             j_magic;
	int             j_prognum;
	ELAN_CAPABILITY j_cap;
};

/* Copy library state */
#define _copy_libstate(dest, src) do { 			\
	assert((src)->ls_magic == QSW_LIBSTATE_MAGIC); 	\
	assert((dest)->ls_magic == QSW_LIBSTATE_MAGIC); \
	memcpy(dest, src, sizeof(struct qsw_libstate));	\
} while (0)

/* Lock on library state */
#define _lock_qsw() do {				\
	int err;					\
	err = pthread_mutex_lock(&qsw_lock);		\
	assert(err == 0);				\
} while (0)
#define _unlock_qsw() do {				\
	int err;					\
	err = pthread_mutex_unlock(&qsw_lock);		\
	assert(err == 0);				\
} while (0)

/*
 * Globals
 */
static qsw_libstate_t qsw_internal_state = NULL;
static pthread_mutex_t qsw_lock = PTHREAD_MUTEX_INITIALIZER;
static elanhost_config_t elanconf = NULL;


/*
 * Allocate a qsw_libstate_t.
 *   lsp (IN)		store pointer to new instantiation here
 *   RETURN		0 on success, -1 on failure (sets errno)
 */
int
qsw_alloc_libstate(qsw_libstate_t *lsp)
{
	qsw_libstate_t new;

	assert(lsp != NULL);
	new = (qsw_libstate_t)malloc(sizeof(struct qsw_libstate));
	if (!new)
		slurm_seterrno_ret(ENOMEM);
	new->ls_magic = QSW_LIBSTATE_MAGIC;
	*lsp = new;
	return 0;
}

/*
 * Free a qsw_libstate_t.
 *   ls (IN)		qsw_libstate_t to free
 */
void
qsw_free_libstate(qsw_libstate_t ls)
{
	assert(ls->ls_magic == QSW_LIBSTATE_MAGIC);
	ls->ls_magic = 0;
	free(ls);
}

/*
 * Pack libstate structure in a format that can be shipped over the
 * network and unpacked on a different architecture.
 *   ls (IN)		libstate structure to be packed
 *   buffer (IN/OUT)	where to store packed data
 *   RETURN		#bytes unused in 'data'
 */
int
qsw_pack_libstate(qsw_libstate_t ls, Buf buffer)
{
	int offset;

	assert(ls->ls_magic == QSW_LIBSTATE_MAGIC);
	offset = get_buf_offset(buffer);

	pack32(ls->ls_magic, buffer);
	pack32(ls->ls_prognum, buffer);
	pack32(ls->ls_hwcontext, buffer);

	return (get_buf_offset(buffer) - offset);
}

/*
 * Unpack libstate packed by qsw_pack_libstate.
 *   ls (IN/OUT)	where to put libstate structure
 *   buffer (IN/OUT)	where to get packed data
 *   RETURN		#bytes unused or -1 on error (sets errno)
 */
int
qsw_unpack_libstate(qsw_libstate_t ls, Buf buffer)
{
	int offset;

	assert(ls->ls_magic == QSW_LIBSTATE_MAGIC);
	offset = get_buf_offset(buffer);

	safe_unpack32(&ls->ls_magic, buffer);
	safe_unpack32(&ls->ls_prognum, buffer);
	safe_unpack32(&ls->ls_hwcontext, buffer);

	if (ls->ls_magic != QSW_LIBSTATE_MAGIC)
		goto unpack_error;

	return SLURM_SUCCESS; 

    unpack_error:
	slurm_seterrno_ret(EBADMAGIC_QSWLIBSTATE); /* corrupted libstate */
	return SLURM_ERROR;
}

/*
 * Seed the random number generator.  This can be called multiple times,
 * but srand48 will only be called once per program invocation.
 */
static void
_srand_if_needed(void)
{
	static int done = 0;

	if (!done) {
		srand48(getpid());
		done = 1;
	}
}

/*
 * Initialize this library, optionally restoring a previously saved state.
 *   oldstate (IN)	old state retrieved from qsw_fini() or NULL
 *   RETURN		0 on success, -1 on failure (sets errno)
 */
int
qsw_init(qsw_libstate_t oldstate)
{
	qsw_libstate_t new;

	assert(qsw_internal_state == NULL);
	_srand_if_needed();
	if (qsw_alloc_libstate(&new) < 0)
		return -1; /* errno set by qsw_alloc_libstate */
	if (oldstate)
		_copy_libstate(new, oldstate);
	else {
		new->ls_prognum = QSW_PRG_START;
		new->ls_hwcontext = QSW_CTX_START;
	}
	qsw_internal_state = new;
	return 0;
}

/*
 * Finalize use of this library.  If 'savestate' is non-NULL, final
 * state is copied there before it is destroyed.
 *   savestate (OUT)	place to put state
 */
void
qsw_fini(qsw_libstate_t savestate)
{
	assert(qsw_internal_state != NULL);
	_lock_qsw();
	if (savestate)
		_copy_libstate(savestate, qsw_internal_state);
	qsw_free_libstate(qsw_internal_state);
	qsw_internal_state = NULL;
	_unlock_qsw();
}

/*
 * Allocate a qsw_jobinfo_t.
 *   jp (IN)		store pointer to new instantiation here
 *   RETURN		0 on success, -1 on failure (sets errno)
 */
int
qsw_alloc_jobinfo(qsw_jobinfo_t *jp)
{
	qsw_jobinfo_t new; 

	assert(jp != NULL);
	new = (qsw_jobinfo_t)malloc(sizeof(struct qsw_jobinfo));
	if (!new)
		slurm_seterrno_ret(ENOMEM);
	new->j_magic = QSW_JOBINFO_MAGIC;
	
	*jp = new;
	return 0;
}

/*
 * Make a copy of a qsw_jobinfo_t.
 *   j (IN)		qsw_jobinfo_t to be copied
 *   RETURN		qsw_jobinfo_t on success, NULL on failure
 */
qsw_jobinfo_t
qsw_copy_jobinfo(qsw_jobinfo_t j)
{
	qsw_jobinfo_t new; 
	if (qsw_alloc_jobinfo(&new))
		return NULL;
	memcpy(new, j, sizeof(struct qsw_jobinfo));

	return new;
}

/*
 * Free a qsw_jobinfo_t.
 *   ls (IN)		qsw_jobinfo_t to free
 */
void
qsw_free_jobinfo(qsw_jobinfo_t j)
{
	if (j == NULL)
		return;
	assert(j->j_magic == QSW_JOBINFO_MAGIC);
	j->j_magic = 0;
	free(j);
}

/*
 * Pack jobinfo structure in a format that can be shipped over the
 * network and unpacked on a different architecture.
 *   j (IN)		jobinfo structure to be packed
 *   buffer (OUT)		where to store packed data
 *   RETURN		#bytes unused in 'data' or -1 on error (sets errno)
 * NOTE: Keep in sync with QSW_PACK_SIZE above
 */
int
qsw_pack_jobinfo(qsw_jobinfo_t j, Buf buffer)
{
	int i, offset;

	assert(j->j_magic == QSW_JOBINFO_MAGIC);
	offset = get_buf_offset(buffer);

	pack32(j->j_magic, 		buffer);
	pack32(j->j_prognum, 		buffer);
	for (i = 0; i < 4; i++)
		pack32(j->j_cap.UserKey.Values[i], buffer);
	pack16(j->j_cap.Type, 		buffer);
#if HAVE_LIBELANCTRL
#  ifdef ELAN_CAP_ELAN3
	pack16(j->j_cap.cap_elan_type,  buffer);
#  else
	j->j_cap.cap_spare = ELAN_CAP_UNINITIALISED;
	pack16(j->j_cap.cap_spare,      buffer);
#  endif 
#endif
#if HAVE_LIBELAN3
	pack16(j->j_cap.padding, 	buffer);
#endif 
	pack32(j->j_cap.Version,	buffer);
	pack32(j->j_cap.LowContext, 	buffer);
	pack32(j->j_cap.HighContext, 	buffer);
	pack32(j->j_cap.MyContext, 	buffer);
	pack32(j->j_cap.LowNode, 	buffer);
	pack32(j->j_cap.HighNode, 	buffer);
#if HAVE_LIBELAN3
	pack32(j->j_cap.Entries, 	buffer);
#endif 
	pack32(j->j_cap.RailMask, 	buffer);
	for (i = 0; i < ELAN_BITMAPSIZE; i++)
		pack32(j->j_cap.Bitmap[i], buffer);

	return (get_buf_offset(buffer) - offset);
}

/*
 * Unpack jobinfo structure packed by qsw_pack_jobinfo.
 *   j (IN/OUT)		where to store libstate structure
 *   buffer (OUT)		where to load packed data
 *   RETURN		#bytes unused in 'data' or -1 on error (sets errno)
 */
int
qsw_unpack_jobinfo(qsw_jobinfo_t j, Buf buffer)
{
	int i, offset;

	assert(j->j_magic == QSW_JOBINFO_MAGIC);
	offset = get_buf_offset(buffer);
 
	safe_unpack32(&j->j_magic, 		buffer);
	safe_unpack32(&j->j_prognum, 		buffer);
	for (i = 0; i < 4; i++)
		safe_unpack32(&j->j_cap.UserKey.Values[i], buffer);
	safe_unpack16(&j->j_cap.Type,		buffer);
#if HAVE_LIBELANCTRL
#  ifdef ELAN_CAP_ELAN3
	safe_unpack16(&j->j_cap.cap_elan_type,  buffer);
#  else
	safe_unpack16(&j->j_cap.cap_spare,      buffer);
#  endif
#endif
#if HAVE_LIBELAN3  
	safe_unpack16(&j->j_cap.padding, 	buffer);	    
#endif
	safe_unpack32(&j->j_cap.Version,	buffer); 	    
	safe_unpack32(&j->j_cap.LowContext, 	buffer);
	safe_unpack32(&j->j_cap.HighContext,	buffer);
	safe_unpack32(&j->j_cap.MyContext,	buffer);
	safe_unpack32(&j->j_cap.LowNode, 	buffer);
	safe_unpack32(&j->j_cap.HighNode,	buffer);
#if HAVE_LIBELAN3
	safe_unpack32(&j->j_cap.Entries, 	buffer);
#endif
	safe_unpack32(&j->j_cap.RailMask, 	buffer);
	for (i = 0; i < ELAN_BITMAPSIZE; i++)
		safe_unpack32(&j->j_cap.Bitmap[i], buffer);
	
	if (j->j_magic != QSW_JOBINFO_MAGIC)
		goto unpack_error;

	return SLURM_SUCCESS;

    unpack_error:
	slurm_seterrno_ret(EBADMAGIC_QSWJOBINFO);
	return SLURM_ERROR;
}

/*
 * Allocate a program description number.  Program descriptions, which are the
 * key abstraction maintained by the rms.o kernel module, must not be used
 * more than once simultaneously on a single node.  We allocate one to each
 * parallel job which more than meets this requirement.  A program description
 * can be compared to a process group, except there is no way for a process to
 * disassociate itself or its children from the program description.  
 * If the library is initialized, we allocate these consecutively, otherwise 
 * we generate a random one, assuming we are being called by a transient 
 * program like pdsh.  Ref: rms_prgcreate(3).
 */
static int
_generate_prognum(void)
{
	int new;

	if (qsw_internal_state) {
		_lock_qsw();
		new = qsw_internal_state->ls_prognum;
		if (new == QSW_PRG_END)
			qsw_internal_state->ls_prognum = QSW_PRG_START;
		else
			qsw_internal_state->ls_prognum++;
		_unlock_qsw();
	} else {
		_srand_if_needed();
		new = lrand48() % (QSW_PRG_END - QSW_PRG_START + 1);
		new += QSW_PRG_START;
	}
	return new;
}

/*
 * Elan hardware context numbers are an adapter resource that must not be used
 * more than once on a single node.  One is allocated to each process on the
 * node that will be communication over Elan.  In order for processes on the 
 * same node to communicate with one another and with other nodes across QsNet,
 * they must use contexts in the hi-lo range of a common capability.
 * If the library is initialized, we allocate these consecutively, otherwise 
 * we generate a random one, assuming we are being called by a transient 
 * program like pdsh.  Ref: rms_setcap(3).
 */
static int
_generate_hwcontext(int num)
{
	int new;

	if (qsw_internal_state) {
		_lock_qsw();
		if (qsw_internal_state->ls_hwcontext + num - 1 > QSW_CTX_END)
			qsw_internal_state->ls_hwcontext = QSW_CTX_START;
		new = qsw_internal_state->ls_hwcontext;
		qsw_internal_state->ls_hwcontext += num;
		_unlock_qsw();
	} else {
		_srand_if_needed();
		new = lrand48() % 
		      (QSW_CTX_END - (QSW_CTX_START + num - 1) - 1);
		new +=  QSW_CTX_START;
	}
	return new;
}

/*
 * Initialize the elan capability for this job.
 */
static void
_init_elan_capability(ELAN_CAPABILITY *cap, int nprocs, int nnodes,
		bitstr_t *nodeset, int cyclic_alloc)
{
	int i, node_num, full_node_cnt, min_procs_per_node, max_procs_per_node;

	/* Task count may not be identical for all nodes */
	full_node_cnt = nprocs % nnodes;
	min_procs_per_node = nprocs / nnodes;
	max_procs_per_node = (nprocs + nnodes - 1) / nnodes;

	_srand_if_needed();

	/* start with a clean slate */
#if HAVE_LIBELANCTRL
	elan_nullcap(cap);
#else
	elan3_nullcap(cap);
#endif

	/* initialize for single rail and either block or cyclic allocation */
	if (cyclic_alloc)
		cap->Type = ELAN_CAP_TYPE_CYCLIC;
	else
		cap->Type = ELAN_CAP_TYPE_BLOCK;
	cap->Type |= ELAN_CAP_TYPE_MULTI_RAIL;
	cap->RailMask = 1;

#if HAVE_LIBELANCTRL
#  ifdef ELAN_CAP_ELAN3
	cap->cap_elan_type = ELAN_CAP_ELAN3;
#  else
	cap->cap_spare = ELAN_CAP_UNINITIALISED;
#  endif
#endif 

	/* UserKey is 128 bits of randomness which should be kept private */
        for (i = 0; i < 4; i++)
		cap->UserKey.Values[i] = lrand48();

	/* set up hardware context range */
	cap->LowContext = _generate_hwcontext(max_procs_per_node);
	cap->HighContext = cap->LowContext + max_procs_per_node - 1;
	/* Note: not necessary to initialize cap->MyContext */

	/* set the range of nodes to be used and number of processes */
	cap->LowNode = bit_ffs(nodeset);
	assert(cap->LowNode != -1);
	cap->HighNode = bit_fls(nodeset);
	assert(cap->HighNode != -1);

#if HAVE_LIBELAN3
	cap->Entries = nprocs;
#endif

#if USE_OLD_LIBELAN
	/* set the hw broadcast bit if consecutive nodes */
	if (abs(cap->HighNode - cap->LowNode) == nnodes - 1)
		cap->Type |= ELAN_CAP_TYPE_BROADCASTABLE;
#else
	/* set unconditionally per qsw gnat sw-elan/4334 */
	/* only time we don't want this is unsupported rev A hardware */
	cap->Type |= ELAN_CAP_TYPE_BROADCASTABLE;
#endif
	/*
	 * Set up cap->Bitmap, which describes the mapping of processes to 
	 * the nodes in the range of cap->LowNode - cap->Highnode.
	 * There are (nprocs * nnodes) significant bits in the mask, each 
 	 * representing a process slot.  Bits are off for process slots 
	 * corresponding to unallocated nodes.  For example, if nodes 4 and 6 
	 * are running two processes per node, bits 0,1 (corresponding to the 
	 * two processes on node 4) and bits 4,5 (corresponding to the two 
	 * processes running on node 6) are set.  
	 */
	node_num = 0;
	for (i = cap->LowNode; i <= cap->HighNode; i++) {
		if (bit_test(nodeset, i)) {
			int j, bit, task_cnt;

			if (node_num++ < full_node_cnt)
				task_cnt = max_procs_per_node;
			else
				task_cnt = min_procs_per_node;

			for (j = 0; j < task_cnt; j++) {
				if (cyclic_alloc)
					bit = (i-cap->LowNode) + ( j * 
					 (cap->HighNode - cap->LowNode + 1));
				else
					bit = ((i-cap->LowNode)
					       * max_procs_per_node) + j;

				assert(bit < (sizeof(cap->Bitmap) * 8));
				BT_SET(cap->Bitmap, bit);
			}
		}
	}
}

/*
 * Create all the QsNet related information needed to set up a QsNet parallel
 * program and store it in the qsw_jobinfo struct.  
 * Call this on the "client" process, e.g. pdsh, srun, slurmctld, etc..
 */
int
qsw_setup_jobinfo(qsw_jobinfo_t j, int nprocs, bitstr_t *nodeset, 
		int cyclic_alloc)
{
	int nnodes = bit_set_count(nodeset);

	assert(j != NULL);
	assert(j->j_magic == QSW_JOBINFO_MAGIC);

	/* sanity check on args */
	/* Note: ELAN_MAX_VPS is 512 on "old" Elan driver, 16384 on new. */
	if ((nprocs <= 0) || (nprocs > ELAN_MAX_VPS) || (nnodes <= 0)) {
		slurm_seterrno_ret(EINVAL);
	}
      
	/* initialize jobinfo */
	j->j_prognum = _generate_prognum();
	_init_elan_capability(&j->j_cap, nprocs, nnodes, nodeset, 
	                      cyclic_alloc);

	return 0;
}

/*
 * Here are the necessary steps to set up to run an Elan MPI parallel program
 * (set of processes) on a node (possibly one of many allocated to the prog):
 *
 * Process 1	Process 2	|	Process 3
 * read args			|
 * fork	-------	rms_prgcreate	|
 * waitpid 	elan3_create	|
 * 		rms_prgaddcap	|
 *		fork N procs ---+------	rms_setcap
 *		wait all	|	setup RMS_ env	
 *				|	setuid, etc.
 *				|	exec mpi process
 *				|	
 *		exit		|
 * rms_prgdestroy		|
 * exit				|     (one pair of processes per mpi proc!)
 *
 * - The first fork is required because rms_prgdestroy can't occur in the 
 *   process that calls rms_prgcreate (since it is a member, ECHILD).
 * - The second fork is required when running multiple processes per node 
 *   because each process must announce its use of one of the hw contexts 
 *   in the range allocated in the capability.
 */

/*
 * Process 1: issue the rms_prgdestroy for the job.
 */
int
qsw_prgdestroy(qsw_jobinfo_t jobinfo)
{
	if (rms_prgdestroy(jobinfo->j_prognum) < 0) {
		/* translate errno values to more descriptive ones */
		switch (errno) {
			case ECHILD:
				slurm_seterrno(ECHILD_PRGDESTROY);
				break;
			case EEXIST:
				slurm_seterrno(EEXIST_PRGDESTROY);
				break;
			default:
				break;
		}
		return -1;
	}
	return 0;
}

/*
 * Process 2: Destroy the context after children are dead.
 */
void
qsw_prog_fini(qsw_jobinfo_t jobinfo)
{
	/* Do nothing... apparently this will be handled by
	 *  callbacks in the kernel exit handlers ... 
	 */
#if 0
	if (jobinfo->j_ctx) {
		elan3_control_close(jobinfo->j_ctx);
		jobinfo->j_ctx = NULL;
	}
#endif
}

/*
 * Process 2: Create the context and make capability available to children.
 */
int
qsw_prog_init(qsw_jobinfo_t jobinfo, uid_t uid)
{
	int err;
	int i, nrails;
#if HAVE_LIBELANCTRL
	nrails = elan_nrails(&jobinfo->j_cap);

	for (i = 0; i < nrails; i++) {
		ELANCTRL_HANDLE handle;
		/*
		 *  Open up the Elan control device so we can create
		 *   a new capability.
		 */
		if (elanctrl_open(&handle) != 0) {
			slurm_seterrno(EELAN3CONTROL);
			goto fail;
		}

		/*  Push capability into device driver */
		if (elanctrl_create_cap(handle, &jobinfo->j_cap) < 0) {
			error("elanctrl_create_cap: %m");
			slurm_seterrno(EELAN3CREATE);
			/* elanctrl_close(handle); */
			goto fail;
		}

		/* elanctrl_close (handle); */
	}

#else /* !HAVE_LIBELANCTRL */
	nrails = elan3_nrails(&jobinfo->j_cap);

	for (i = 0; i < nrails; i++) {

		ELAN3_CTX *ctx;

		/* see qsw gnat sw-elan/4334: elan3_control_open can ret -1 */
		if ((ctx = elan3_control_open(i)) == NULL 
				|| ctx == (void *)-1) {
			slurm_seterrno(EELAN3CONTROL);
			goto fail;
		}
		
	
		/* make cap known via rms_getcap/rms_ncaps to members 
		 * of this prgnum */
		if (elan3_create(ctx, &jobinfo->j_cap) < 0) {
			/* XXX masking errno value better than not knowing 
			 * which function failed? */
		        error("elan3_create(%d): %m", i);
			slurm_seterrno(EELAN3CREATE); 
			goto fail;
		}
	}
#endif
	/* associate this process and its children with prgnum */
	if (rms_prgcreate(jobinfo->j_prognum, uid, 1) < 0) {
		/* translate errno values to more descriptive ones */
		switch (errno) {
			case EINVAL:
				slurm_seterrno(EINVAL_PRGCREATE);
				break;
			default:
				break;
		}
		goto fail;
	}

	if (rms_prgaddcap(jobinfo->j_prognum, 0, &jobinfo->j_cap) < 0) {
		/* translate errno values to more descriptive ones */
		switch (errno) {
			case ESRCH:
				slurm_seterrno(ESRCH_PRGADDCAP);
				break;
			case EFAULT:
				slurm_seterrno(EFAULT_PRGADDCAP);
				break;
			default:
				break;
		}
		goto fail;
	}

	/* note: _elan3_fini() destroys context and makes capability unavail */
	/* do it in qsw_prog_fini() after app terminates */
	return 0;
fail:
	err = errno; /* presrve errno in case _elan3_fini touches it */
	qsw_prog_fini(jobinfo); 
	slurm_seterrno(err);
	return -1;
}

/*
 * Process 3: Do the rms_setcap.
 */
int
qsw_setcap(qsw_jobinfo_t jobinfo, int procnum)
{
	/*
	 * Assign elan hardware context to current process.
	 * - arg1 (0 below) is an index into the kernel's list of caps for this 
	 *   program desc (added by rms_prgaddcap).  There will be
	 *   one per rail.
	 * - arg2 indexes the hw ctxt range in the capability
	 *   [cap->LowContext, cap->HighContext]
	 */
	if (rms_setcap(0, procnum) < 0) {
		/* translate errno values to more descriptive ones */
		switch (errno) {
			case EINVAL:
				slurm_seterrno(EINVAL_SETCAP);
				break;
			case EFAULT:
				slurm_seterrno(EFAULT_SETCAP);
				break;
			default:
				break;
		}
		return -1;
	}
	return 0;
}


/*
 * Return the local elan address (for rail 0) or -1 on failure.
 */
int
qsw_getnodeid(void)
{
	int nodeid = -1;
#if HAVE_LIBELANCTRL
	ELAN_DEV_IDX    devidx = 0;
	ELANCTRL_HANDLE handle;
	ELAN_POSITION   position;

	if (elanctrl_open(&handle) != 0) 
		slurm_seterrno_ret(EGETNODEID);

	if (elanctrl_get_position(handle, devidx, &position) != 0) {
		elanctrl_close (handle);
		slurm_seterrno_ret(EGETNODEID);
	}

	nodeid = position.pos_nodeid;

	elanctrl_close (handle);
#else
	ELAN3_CTX *ctx = _elan3_init(0); /* rail 0 */
	if (ctx) {
		nodeid = ctx->devinfo.Position.NodeId;
		elan3_control_close(ctx);
	}
#endif
	if (nodeid == -1)
		slurm_seterrno(EGETNODEID);
	return nodeid;

}

static int 
_read_elanhost_config (void)
{
	int rc;

	if (!(elanconf = elanhost_config_create ()))
		return (-1);

	if ((rc = elanhost_config_read (elanconf, NULL)) < 0) {
		error ("Unable to read Elan config: %s", 
		       elanhost_config_err (elanconf));
		elanhost_config_destroy (elanconf);
		elanconf = NULL;
		return (-1);
	}

	return (0);
}

int
qsw_maxnodeid(void)
{
	int maxid = -1;

	_lock_qsw();
	if (!elanconf && (_read_elanhost_config() < 0))
		goto done;

	maxid = elanhost_config_maxid (elanconf);

    done:
	_unlock_qsw();
	return maxid;
}

/*
 * Given a hostname, return the elanid or -1 on error.  
 *  Initializes the elanconfig from the default /etc/elanhosts
 *  config file.
 */
int
qsw_getnodeid_byhost(char *host)
{
	int id = -1;

	if (host == NULL)
		return (-1);

	_lock_qsw();
	if (!elanconf && (_read_elanhost_config() < 0))
		goto done;

	xassert (elanconf != NULL);

	id = elanhost_host2elanid (elanconf, host);

    done:
	_unlock_qsw();
	return id;
}

/*
 * Given an elanid, determine the hostname.  Returns -1 on error or the number
 * of characters copied on success.  
 * XXX - assumes RMS style hostnames (see above)
 */
int
qsw_gethost_bynodeid(char *buf, int len, int id)
{
	int rc = -1;
	char *hostp;

	if (id < 0) slurm_seterrno_ret(EGETHOST_BYNODEID);

	_lock_qsw();
	if (!elanconf && (_read_elanhost_config() < 0))
		goto done;

	if (!(hostp = elanhost_elanid2host (elanconf, ELANHOST_EIP, id))) {
		slurm_seterrno (EGETHOST_BYNODEID);
		goto done;
	}
	
	rc = strlcpy (buf, hostp, len);

    done:
	_unlock_qsw();
	return (rc);
}

/*
 * Send the specified signal to all members of a program description.
 * Returns -1 on failure and sets errno.  Ref: rms_prgsignal(3).
 */
int
qsw_prgsignal(qsw_jobinfo_t jobinfo, int signum)
{
	if (rms_prgsignal(jobinfo->j_prognum, signum) < 0) {
		/* translate errno values to more descriptive ones */
		switch (errno) {
			case EINVAL:
				slurm_seterrno(EINVAL_PRGSIGNAL);
				break;
			case ESRCH:
				slurm_seterrno(ESRCH_PRGSIGNAL);
				break;
			default:
				break;
		}
		return -1;
	}
	return 0;
}

#define _USE_ELAN3_CAPABILITY_STRING 1

#ifndef _USE_ELAN3_CAPABILITY_STRING
#define TRUNC_BITMAP 1
static void
_print_capbitmap(FILE *fp, ELAN_CAPABILITY *cap)
{
	int bit_max = sizeof(cap->Bitmap)*8 - 1;
	int bit;
#if TRUNC_BITMAP
	bit_max = bit_max >= 64 ? 64 : bit_max;
#endif
	for (bit = bit_max; bit >= 0; bit--)
		fprintf(fp, "%c", BT_TEST(cap->Bitmap, bit) ? '1' : '0');
	fprintf(fp, "\n");
}
#endif /* !_USE_ELAN3_CAPABILITY_STRING */

char *
qsw_capability_string(struct qsw_jobinfo *j, char *buf, size_t size)
{
	ELAN_CAPABILITY *cap;

	assert(buf != NULL);
	assert(j->j_magic == QSW_JOBINFO_MAGIC);

	cap = &j->j_cap;

#if HAVE_LIBELANCTRL
	snprintf(buf, size, "prg=%d ctx=%x.%x nodes=%d.%d",
	         j->j_prognum, cap->LowContext, cap->HighContext, 
		 cap->LowNode, cap->HighNode);
#else 
	snprintf(buf, size, "prg=%d ctx=%x.%x nodes=%d.%d entries=%d",
	         j->j_prognum, cap->LowContext, cap->HighContext, 
		 cap->LowNode, cap->HighNode, 
	         cap->Entries);
#endif
         
	return buf;
}

void
qsw_print_jobinfo(FILE *fp, struct qsw_jobinfo *jobinfo)
{
	ELAN_CAPABILITY *cap;
	char str[8192];

	assert(jobinfo->j_magic == QSW_JOBINFO_MAGIC);

	fprintf(fp, "__________________\n");
	fprintf(fp, "prognum=%d\n", jobinfo->j_prognum);

	cap = &jobinfo->j_cap;
	/* use elan3_capability_string as a shorter alternative for now */
#if _USE_ELAN3_CAPABILITY_STRING
#  if HAVE_LIBELANCTRL
	fprintf(fp, "%s\n", elan_capability_string(cap, str));
#  else
	fprintf(fp, "%s\n", elan3_capability_string(cap, str));
#  endif
#else 
	fprintf(fp, "cap.UserKey=%8.8x.%8.8x.%8.8x.%8.8x\n",
			cap->UserKey.Values[0], cap->UserKey.Values[1],
			cap->UserKey.Values[2], cap->UserKey.Values[3]);
	/*fprintf(fp, "cap.Version=%d\n", cap->Version);*/
	fprintf(fp, "cap.Type=0x%hx\n", cap->Type);
	fprintf(fp, "cap.LowContext=%d\n", cap->LowContext);
	fprintf(fp, "cap.HighContext=%d\n", cap->HighContext);
	fprintf(fp, "cap.MyContext=%d\n", cap->MyContext);
	fprintf(fp, "cap.LowNode=%d\n", cap->LowNode);
	fprintf(fp, "cap.HighNode=%d\n", cap->HighNode);
#if HAVE_LIBELAN3
	fprintf(fp, "cap.padding=%hd\n", cap->padding);
	fprintf(fp, "cap.Entries=%d\n", cap->Entries);
#endif
	fprintf(fp, "cap.Railmask=0x%x\n", cap->RailMask);
	fprintf(fp, "cap.Bitmap=");
	_print_capbitmap(fp, cap);
#endif
	fprintf(fp, "\n------------------\n");
}
