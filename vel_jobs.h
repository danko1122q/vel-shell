/*Copyright (c) 2026, danko1122q
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef VEL_JOBS_H
#define VEL_JOBS_H

/*
 * vel_jobs.h  --  job control and shell pipeline for vel
 *
 * Provides:
 *   - sh        : run a shell command string via /bin/sh -c
 *   - shpipe    : Unix-style pipeline  cmd1 | cmd2 | ...
 *   - spawn     : fork+exec, returns pid
 *   - wait      : wait for pid or job id
 *   - waitall   : wait for all background jobs
 *   - kill      : send signal to pid
 *   - jobs      : list background jobs
 *   - fg        : bring job to foreground
 *   - jobstatus : get exit status of finished job
 *   - sigaction : set signal handler (vel code)
 *
 * Job table:
 *   Each background job has:
 *     jid  (job id, 1-based, reused)
 *     pid
 *     status (running / stopped / done)
 *     exit_code
 *     cmd  (display string)
 */

#ifndef WIN32

#include <sys/types.h>
#include "vel.h"

/* job states */
#define VJOB_RUNNING  0
#define VJOB_STOPPED  1
#define VJOB_DONE     2

#define VEL_MAX_JOBS  256

typedef struct vel_job_s {
    int     jid;           /* 1-based job id */
    pid_t   pid;
    int     state;         /* VJOB_RUNNING / STOPPED / DONE */
    int     exit_code;
    char   *cmd;           /* display string */
    int     pgid;          /* process group id (for job control) */
} vel_job_t;

/* global job table */
extern vel_job_t  g_jobs[VEL_MAX_JOBS];
extern int        g_njobs;
extern int        g_job_control;   /* 1 if terminal job control enabled */

/* init / cleanup */
void vel_jobs_init(void);
void vel_jobs_cleanup(void);

/* register all job-control commands into vel instance */
void register_job_builtins(vel_t vel);

/* internal: reap finished children (non-blocking) */
void vel_jobs_reap(void);

/*
 * vel_jobs_dispatch_signals -- proses sinyal yang tertunda (pending).
 *
 * HARUS dipanggil dari main loop (konteks aman), BUKAN dari signal handler.
 * Signal handler hanya set flag; fungsi ini yang menjalankan vel code-nya.
 * Panggil setelah vel_jobs_reap() di setiap iterasi REPL.
 */
void vel_jobs_dispatch_signals(vel_t vel);

#endif /* !WIN32 */
#endif /* VEL_JOBS_H */
