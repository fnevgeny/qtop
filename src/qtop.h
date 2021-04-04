/**
 *
 * This file is part of qtop.
 *
 * Copyright 2021 Evgeny Stambulchik
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef QTOP_H_
#define QTOP_H_

#define QTOP_VERSION "1.0"

#define RESOURCE_TYPE_NONE  0
#define RESOURCE_TYPE_MEM   1
#define RESOURCE_TYPE_TIME  2
#define RESOURCE_TYPE_CPLX  3

#define HEADER_NROWS        3

#define COLOR_PAIR_HEADER    1
#define COLOR_PAIR_JHEADER   2
#define COLOR_PAIR_JOB_R     3
#define COLOR_PAIR_JOB_Q     4
#define COLOR_PAIR_JOB_W     5
#define COLOR_PAIR_JOB_H     6
#define COLOR_PAIR_JOB_OTHER 7
#define COLOR_PAIR_JOB_BAD   8
#define COLOR_PAIR_JOB_S     9

#define DEFAULT_REFRESH     30
#define DEFAULT_HISTORY     24

typedef struct {
    char *servername;

    int conn;

    /* filters */
    char *username;
    char *queue;
    char *state;
    bool finished;
    int history_span;
    bool failed;
    bool subjobs;

    WINDOW *jwin;
} qtop_t;

typedef struct {
    struct batch_status *qstatus;

    char *host;
    char *version;

    bool active;
    unsigned int total_jobs;
    unsigned int njobs_r;
    unsigned int njobs_q;
    unsigned int njobs_w;
    unsigned int njobs_t;
    unsigned int njobs_h;
    unsigned int njobs_e;
    unsigned int njobs_b;

    long mem;
    long vmem;
    unsigned int ncpus;
    unsigned int mpiprocs;
} server_t;

typedef enum {
    JOB_RUNNING   = 'R',
    JOB_QUEUED    = 'Q',
    JOB_WAITING   = 'W',
    JOB_HELD      = 'H',
    JOB_SUSPENDED = 'S',
    JOB_EXITING   = 'E',
    JOB_TRANSIT   = 'T',
    JOB_BEGUN     = 'B',
    JOB_FINISHED  = 'F'
} job_state_t;

typedef struct {
    unsigned int id;
    char *name;
    char *queue;
    char *user;

    bool is_array;
    unsigned int aid;
    bool is_last_subjob;

    job_state_t state;

    /* requested values */
    long mem_r;
    long vmem_r;
    unsigned int ncpus_r;
    long cput_r;
    long walltime_r;
    double io_r;

    /* used values */
    long mem_u;
    long vmem_u;
    unsigned int ncpus_u;
    long cput_u;
    long walltime_u;

    double cpupercent;

    int exit_status;
} job_t;

#endif /* QTOP_H_ */
