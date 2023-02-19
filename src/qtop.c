/**
 *
 * This file is part of qtop.
 *
 * Copyright 2021-2023 Evgeny Stambulchik
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

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <pwd.h>
#include <math.h>

#include <stdbool.h>

#include <pbs_error.h>
#include <pbs_ifl.h>

#include <ncurses.h>

#include <getopt.h>

#include "qtop.h"

static void xfree(void *p)
{
    if (p) {
        free(p);
    }
}

/* Time in seconds, memory in kB */
static long int parse_resource(const char *resource, const char *svalue, int *type)
{
    long int value = 0;
    if (svalue[0] == '[') {
        *type = RESOURCE_TYPE_CPLX;
    } else
    if (strstr(resource, "mem")) {
        sscanf(svalue, "%ld", &value);
        if (strstr(svalue, "kb")) {
            ;
        } else
        if (strstr(svalue, "mb")) {
            value <<= 10;
        } else
        if (strstr(svalue, "gb")) {
            value <<= 20;
        } else
        if (strstr(svalue, "tb")) {
            value <<= 30;
        } else {
            value >>= 10;
        }
        *type = RESOURCE_TYPE_MEM;
    } else
    if (!strcmp(resource, "walltime") || !strcmp(resource, "cput")) {
        int h, m, s;
        sscanf(svalue, "%d:%d:%d", &h, &m, &s);
        value = s + 60*(m + 60*h);
        *type = RESOURCE_TYPE_TIME;
    } else {
        sscanf(svalue, "%ld", &value);
        *type = RESOURCE_TYPE_NONE;
    }

    return value;
}

static bool is_absolute_time(const struct attrl *qattr)
{
    if (!strcmp(qattr->name + 1, "time") ||
        !strcmp(qattr->name, ATTR_a) ||
        !strcmp(qattr->name, ATTR_history_timestamp) ||
        (qattr->resource != NULL &&
         !strcmp(qattr->resource, "start_time"))) {
        return true;
    } else {
        return false;
    }
}

/* A dumb JSDL decoder */

#define START_JSDL_ARG  "<jsdl-hpcpa:Argument>"
#define END_JSDL_ARG    "</jsdl-hpcpa:Argument>"

static void print_jsdl_args(char *buf, size_t bufsize, const char *str)
{
    bool done = false;
    size_t x = 0;
    char *p = strstr(str, START_JSDL_ARG);
    while (p && !done) {
        p += strlen(START_JSDL_ARG);
        while (p && *p != '<' && !done) {
            buf[x] = *p;
            if (x >= bufsize - 1) {
                done = true;
            } else {
                x++;
                p++;
            }
        }
        if (!done) {
            p = strstr(p, START_JSDL_ARG);
            if (p) {
                buf[x] = ' ';
                x++;
            }
        }
    }
    buf[x] = '\0';
}

static void print_attribs(WINDOW *win, const struct attrl *attribs,
    unsigned int xshift, unsigned int yshift)
{
    int y = 1, maxx, maxy;
    getmaxyx(win, maxy, maxx);
    const struct attrl *qattr = attribs;

    while (qattr && yshift) {
        qattr = qattr->next;
        yshift--;
    }

    while (qattr && y < maxy - 1) {
        char linebuf[1024], tbuf[512], *vstr;
        if (is_absolute_time(qattr)) {
            time_t timer = atol(qattr->value);
            struct tm* tm_info = localtime(&timer);
            strftime(tbuf, 64, "%Y-%m-%d %H:%M:%S %Z", tm_info);
            vstr = tbuf;
        } else
        if (!strcmp(qattr->name, ATTR_submit_arguments)) {
            print_jsdl_args(tbuf, 512, qattr->value);
            vstr = tbuf;
        } else {
            vstr = qattr->value;
        }
        if (qattr->resource != NULL) {
            snprintf(linebuf, 1024, "%s.%s = %s",
                qattr->name, qattr->resource, vstr);
        } else {
            snprintf(linebuf, 1024, "%s = %s", qattr->name, vstr);
        }
        linebuf[1023] = '\0';
        if (strlen(linebuf) > maxx + xshift - 2) {
            linebuf[maxx + xshift - 3] = '>';
            linebuf[maxx + xshift - 2] = '\0';
        }
        if (strlen(linebuf) > xshift) {
            mvwprintw(win, y, 1, "%s", linebuf + xshift);
        }
        y++;

        qattr = qattr->next;
    }
}

static void parse_server_attribs(server_t *pbs)
{
    const struct attrl *qattr = pbs->qstatus->attribs;
    while (qattr) {
        if (!strcmp(qattr->name, ATTR_SvrHost)) {
            pbs->host = qattr->value;
        } else
        if (!strcmp(qattr->name, ATTR_version)) {
            pbs->version = qattr->value;
        } else
        if (!strcmp(qattr->name, ATTR_status)) {
            pbs->active = !strcmp(qattr->value, "Active") ? true:false;
        } else
        if (!strcmp(qattr->name, ATTR_total)) {
            pbs->total_jobs = atoi(qattr->value);
        } else
        if (!strcmp(qattr->name, ATTR_count)) {
            int nt, nq, nh, nw, nr, ne, nb;
            const char *pattern = "Transit:%d Queued:%d Held:%d Waiting:%d Running:%d Exiting:%d Begun:%d";
            if (sscanf(qattr->value, pattern,
                &nt, &nq, &nh, &nw, &nr, &ne, &nb) == 7) {
                pbs->njobs_r = nr;
                pbs->njobs_q = nq;
                pbs->njobs_w = nw;
                pbs->njobs_t = nt;
                pbs->njobs_h = nh;
                pbs->njobs_e = ne;
                pbs->njobs_b = nb;
            }
        } else
        if (!strcmp(qattr->name, ATTR_rescassn)) {
            int type;
            if (!strcmp(qattr->resource, "mem")) {
                pbs->mem = parse_resource(qattr->resource, qattr->value, &type);
            } else
            if (!strcmp(qattr->resource, "vmem")) {
                pbs->vmem = parse_resource(qattr->resource, qattr->value, &type);
            } else
            if (!strcmp(qattr->resource, "ncpus")) {
                pbs->ncpus = atoi(qattr->value);
            } else
            if (!strcmp(qattr->resource, "mpiprocs")) {
                pbs->mpiprocs = atoi(qattr->value);
            }
        }

        qattr = qattr->next;
    }
}

void qtop_free(qtop_t *q)
{
    if (q) {
        xfree(q->servername);
        xfree(q->username);
        xfree(q->queue);
        xfree(q->state);
        xfree(q->exec_host);
        if (q->conn > 0) {
            pbs_disconnect(q->conn);
        }
        xfree(q);
    }
}

qtop_t *qtop_new(char *servername)
{
    qtop_t *q = calloc(1, sizeof(qtop_t));
    if (!q) {
        return NULL;
    }

    if (!servername) {
        servername = pbs_default();
    }
    if (!servername) {
        qtop_free(q);
        return NULL;
    }

    q->servername = strdup(servername);

    q->conn = pbs_connect(servername);
    if (q->conn <= 0) {
        qtop_free(q);
        return NULL;
    }

    return q;
}

bool qtop_reconnect(qtop_t *q)
{
    q->conn = pbs_connect(q->servername);
    if (q->conn <= 0) {
        return false;
    } else {
        return true;
    }
}

void pbs_server_free(server_t *p)
{
    if (p) {
        if (p->qstatus != NULL) {
            pbs_statfree(p->qstatus);
        }
    }
}

server_t *pbs_server_new(void)
{
    server_t *p = calloc(1, sizeof(server_t));
    if (!p) {
        return NULL;
    }

    return p;
}

bool qtop_server_update(const qtop_t *q, server_t *pbs)
{
    struct attrl *qattribs = NULL;

    if (pbs->qstatus != NULL) {
        pbs_statfree(pbs->qstatus);
    }

    pbs->qstatus = pbs_statserver(q->conn, qattribs, NULL);
    if (pbs->qstatus == NULL) {
        return false;
    }

    parse_server_attribs(pbs);

    return true;
}

static void job_free_data(job_t *job)
{
    if (job) {
        xfree(job->name);
        xfree(job->queue);
        xfree(job->user);
        xfree(job->exec_host);
    }
}

static void parse_job_attribs(job_t *job, const struct attrl *attribs)
{
    const struct attrl *qattr = attribs;
    while (qattr) {
        char *euser = NULL, *owner = NULL;
        if (!strcmp(qattr->name, ATTR_name)) {
            job->name = strdup(qattr->value);
        } else
        if (!strcmp(qattr->name, ATTR_euser)) {
            euser = qattr->value;
        } else
        if (!strcmp(qattr->name, ATTR_owner)) {
            owner = qattr->value;
        } else
        if (!strcmp(qattr->name, ATTR_state)) {
            job->state = qattr->value[0];
        } else
        if (!strcmp(qattr->name, ATTR_queue)) {
            job->queue = strdup(qattr->value);
        } else
        if (!strcmp(qattr->name, ATTR_exechost) && qattr->value) {
            job->exec_host = strdup(qattr->value);
        } else
        if (!strcmp(qattr->name, ATTR_l)) {
            int type;
            if (!strcmp(qattr->resource, "mem")) {
                job->mem_r = parse_resource(qattr->resource, qattr->value, &type);
            } else
            if (!strcmp(qattr->resource, "vmem")) {
                job->vmem_r = parse_resource(qattr->resource, qattr->value, &type);
            } else
            if (!strcmp(qattr->resource, "ncpus")) {
                job->ncpus_r = atoi(qattr->value);
            } else
            if (!strcmp(qattr->resource, "nodect")) {
                job->nodect_r = atoi(qattr->value);
            } else
            if (!strcmp(qattr->resource, "walltime")) {
                job->walltime_r = parse_resource(qattr->resource, qattr->value, &type);
            } else
            if (!strcmp(qattr->resource, "cput")) {
                job->cput_r = parse_resource(qattr->resource, qattr->value, &type);
            } else
            if (!strcmp(qattr->resource, "io")) {
                job->io_r = atof(qattr->value);
            }
        } else
        if (!strcmp(qattr->name, ATTR_used)) {
            int type;
            if (!strcmp(qattr->resource, "mem")) {
                job->mem_u = parse_resource(qattr->resource, qattr->value, &type);
            } else
            if (!strcmp(qattr->resource, "vmem")) {
                job->vmem_u = parse_resource(qattr->resource, qattr->value, &type);
            } else
            if (!strcmp(qattr->resource, "ncpus")) {
                job->ncpus_u = atoi(qattr->value);
            } else
            if (!strcmp(qattr->resource, "walltime")) {
                job->walltime_u = parse_resource(qattr->resource, qattr->value, &type);
            } else
            if (!strcmp(qattr->resource, "cput")) {
                job->cput_u = parse_resource(qattr->resource, qattr->value, &type);
            } else
            if (!strcmp(qattr->resource, "cpupercent")) {
                job->cpupercent = atoi(qattr->value);
            }
        }

        if (euser != NULL) {
            job->user = strdup(euser);
        } else
        if (owner != NULL) {
            job->user = strdup(owner);
        }
        char *iat;
        if (job->user && (iat = strchr(job->user, '@')) > job->user) {
            *iat = '\0';
        }

        qattr = qattr->next;
    }
}

void attropl_free(struct attropl *a)
{
    while (a) {
        struct attropl *next = a->next;
        free(a);
        a = next;
    }
}

static struct attropl *attropl_add(struct attropl *a,
    const char *name, const char *value, enum batch_op op)
{
    struct attropl *new = calloc(1, sizeof(struct attropl));
    if (new) {
        new->name = (char *) name;
        new->value = (char *) value;
        new->op = op;
        new->next = a;
    }
    
    return new;
}

job_t *qtop_server_jobs(qtop_t *q, int *njobs, int ajob_id_expanded)
{
    struct batch_status *qstatus, *qstatus_sub = NULL, *qtmp;
    struct attrl *qattribs = NULL;
    struct attropl *criteria_list = NULL;
    char extend[3] = "";
    int nsubjobs = 0, njobs_total;

    if (q->subjobs) {
        strcat(extend, "t");
    }
    if (q->finished) {
        strcat(extend, "x");
    }

    qattribs = calloc(7, sizeof(struct attrl));
    qattribs[0].name = ATTR_name;
    qattribs[0].value = "";
    qattribs[0].next = qattribs + 1;
    qattribs[1].name = ATTR_queue;
    qattribs[1].value = "";
    qattribs[1].next = qattribs + 2;
    qattribs[2].name = ATTR_owner;
    qattribs[2].value = "";
    qattribs[2].next = qattribs + 3;
    qattribs[3].name = ATTR_state;
    qattribs[3].value = "";
    qattribs[3].next = qattribs + 4;
    qattribs[4].name = ATTR_l;
    qattribs[4].value = "";
    qattribs[4].next = qattribs + 5;
    qattribs[5].name = ATTR_exechost;
    qattribs[5].value = "";
    qattribs[5].next = qattribs + 6;
    qattribs[6].name = ATTR_used;
    qattribs[6].value = "";
    qattribs[6].next = NULL;

    if (q->username != NULL) {
        criteria_list = attropl_add(criteria_list, ATTR_u, q->username, EQ);
    }
    if (q->queue != NULL) {
        criteria_list = attropl_add(criteria_list, ATTR_q, q->queue, EQ);
    }
    if (q->state) {
        criteria_list = attropl_add(criteria_list, ATTR_state, q->state, EQ);
    }

    if (q->finished) {
        char buf[16];
        time_t now = time(NULL);
        sprintf(buf, "%ld", now - 3600*q->history_span);
        criteria_list = attropl_add(criteria_list,
            ATTR_history_timestamp, buf, GE);
    }
    if (q->failed) {
        criteria_list = attropl_add(criteria_list, ATTR_exit_status, "0", NE);
    }

    qstatus = pbs_selstat(q->conn, criteria_list, qattribs, extend);
    if (qstatus == NULL) {
        xfree(qattribs);
        attropl_free(criteria_list);
        *njobs = 0;
        return NULL;
    }

    unsigned int jid = 0;
    qtmp = qstatus;
    while (qtmp) {
        jid++;
        qtmp = qtmp->next;
    }
    *njobs = jid;

    char idbuf[32];
    if (ajob_id_expanded > 0) {
        sprintf(idbuf, "%d[]", ajob_id_expanded);
        qstatus_sub = pbs_statjob(q->conn, idbuf, qattribs, "xt");
        if (qstatus_sub != NULL) {
            jid = 0;
            qtmp = qstatus_sub;
            while (qtmp) {
                jid++;
                qtmp = qtmp->next;
            }
            // 1 is for the parent job
            nsubjobs = jid - 1;
            *njobs += nsubjobs;
        }
    }

    njobs_total = *njobs;

    job_t *jobs = calloc(*njobs, sizeof(job_t));
    if (!jobs) {
        *njobs = 0;
        pbs_statfree(qstatus);
        if (qstatus_sub) {
            pbs_statfree(qstatus_sub);
        }
        return NULL;
    }

    qtmp = qstatus;
    jid = 0;
    bool in_subjobs = false;
    while (qtmp) {
        job_t *job = jobs + jid;

        char *idot, *isb1, *isb2;
        if (qtmp->name && (idot = strchr(qtmp->name, '.')) > qtmp->name) {
            *idot = '\0';
        }

        if ((isb1 = strchr(qtmp->name, '[')) &&
            (isb2 = strchr(qtmp->name, ']'))) {
            if (isb1 + 1 == isb2) {
                job->is_array = true;
                sscanf(qtmp->name, "%u[]", &job->id);
            } else {
                sscanf(qtmp->name, "%u[%u]", &job->id, &job->aid);
            }
        } else {
            job->id = atoi(qtmp->name);
        }

        parse_job_attribs(job, qtmp->attribs);
        qtmp = qtmp->next;
        if (qtmp == NULL && !in_subjobs && qstatus_sub != NULL) {
            // Skip the parent array job itself; it's already in the list
            qtmp = qstatus_sub->next;
            in_subjobs = true;
        }
        if (in_subjobs && qtmp == NULL) {
            job->is_last_subjob = true;
        }
        // Filter out undesired jobs
        if (q->exec_host &&
            (!job->exec_host || !strstr(job->exec_host, q->exec_host))) {
            job_free_data(job);
            memset(job, 0, sizeof(job_t));
            (*njobs)--;
        } else {
            jid++;
        }
    }

    /* free unused part of the array */
    if (*njobs < njobs_total) {
        jobs = realloc(jobs, (*njobs)*sizeof(job_t));
    }

    /* free allocated data */
    xfree(qattribs);
    attropl_free(criteria_list);
    pbs_statfree(qstatus);
    if (qstatus_sub) {
        pbs_statfree(qstatus_sub);
    }

    return jobs;
}

void print_server_stats(const server_t *pbs, WINDOW *win)
{
    const double gb_scale = pow(2, 20);

    time_t now = time(NULL);
    struct tm *ptm = localtime(&now);
    char datebuf[32];
    strftime(datebuf, 32, "%T", ptm);

    int njobs_x = pbs->total_jobs - pbs->njobs_r - pbs->njobs_q - pbs->njobs_w
        - pbs->njobs_h - pbs->njobs_t - pbs->njobs_e - pbs->njobs_b;

    wattron(win, COLOR_PAIR(COLOR_PAIR_HEADER));

    mvwprintw(win, 0, 0, "%s PBS-%s %d jobs (%dR %dQ %dW %dH %dT %dE %dB %dF)",
        pbs->host, pbs->version, pbs->total_jobs,
        pbs->njobs_r, pbs->njobs_q, pbs->njobs_w, pbs->njobs_h,
        pbs->njobs_t, pbs->njobs_e, pbs->njobs_b, njobs_x);

    // prepare X coordinate for the timer - if possible,
    // right-aligned to the upper header line
    int x, y;
    getyx(win, y, x);
    if (y > 0) {
        x = COLS;
    }

    mvwprintw(win, 1, 0,
        "Mem: %.1f GiB, VMem: %.1f GiB, Cores: %d (SP:%d + MP:%d)",
        pbs->mem/gb_scale, pbs->vmem/gb_scale, pbs->ncpus,
        pbs->ncpus - pbs->mpiprocs, pbs->mpiprocs);
    mvwprintw(win, 1, x - 8, "%s", datebuf);

    wattroff(win, COLOR_PAIR(COLOR_PAIR_HEADER));
    wrefresh(win);
}

static bool format_time(unsigned int secs, char buf[9])
{
    int hh, mm, ss;
    
    ss = secs % 60;
    mm = (secs/60) % 60;
    hh = secs/3600;

    if (hh <= 99) {
        sprintf(buf, "%02d:%02d:%02d", hh, mm, ss);
        return true;
    } else {
        sprintf(buf, "**:%02d:%02d", mm, ss);
        return false;
    }
}

static int get_idlen(unsigned int id)
{
    int len = 1;
    while (id /= 10) {
        len++;
    }
    return len;
}

void print_jobs(const job_t *jobs, int njobs, WINDOW *win, int selpos,
    unsigned int xshift)
{
    int i;

    wattron(win, COLOR_PAIR(COLOR_PAIR_JHEADER) | A_REVERSE);

    mvwprintw(win, HEADER_NROWS - 1, 0, "%s", "  Job ID ");
    const char *dheader =
        "    User    Queue S    Mem %Mem   VMem  NC %CPU Walltime I/O Name";

    int x, __attribute__ ((unused)) y;
    getyx(win, y, x);
    mvwprintw(win, HEADER_NROWS - 1, x,
        "%-*s", COLS - x, dheader + xshift);

    wattroff(win, COLOR_PAIR(COLOR_PAIR_JHEADER) | A_REVERSE);

    const double gb_scale = pow(2, 20);
    const job_t *job = jobs;
    for (i = HEADER_NROWS; i < LINES && i < njobs + HEADER_NROWS; i++) {
        double mem, vmem;
        long cput, walltime;
        int ncpus;
        double cpuutil = 0, memutil = 0, wallutil = 0;
        char linebuf[1024];

        switch (job->state) {
        case JOB_RUNNING:
        case JOB_EXITING:
        case JOB_FINISHED:
        case JOB_SUSPENDED:
            mem         = job->mem_u;
            vmem        = job->vmem_u;
            cput        = job->cput_u;
            walltime    = job->walltime_u;
            ncpus       = job->ncpus_u;
            if (walltime > 0) {
                cpuutil = (double) cput/(ncpus*walltime);
            }
            if (job->mem_r > 0) {
                memutil = mem/job->mem_r;
            }
            break;
        default:
            mem         = job->mem_r;
            vmem        = job->vmem_r;
            cput        = job->cput_r;
            walltime    = job->walltime_r;
            ncpus       = job->ncpus_r;
            break;
        }

        if (job->walltime_r != 0) {
            wallutil = (double) job->walltime_u/job->walltime_r;
        }

        int cpair = 0;
        switch (job->state) {
        case JOB_RUNNING:
            cpair = COLOR_PAIR_JOB_R;
            break;
        case JOB_QUEUED:
            cpair = COLOR_PAIR_JOB_Q;
            break;
        case JOB_WAITING:
            cpair = COLOR_PAIR_JOB_W;
            break;
        case JOB_HELD:
            cpair = COLOR_PAIR_JOB_H;
            break;
        case JOB_SUSPENDED:
            cpair = COLOR_PAIR_JOB_S;
            break;
        default:
            cpair = COLOR_PAIR_JOB_OTHER;
            break;
        }
        // Test for "badness" only jobs that have run at last 2 min
        if (job->walltime_u > 120) {
            double cpuutil_min, cpuutil_max = 1.25;
            unsigned int nodect = job->nodect_r;
            if (ncpus == 1) {
                cpuutil_min = 0.5;
                if (job->io_r > 1.0) {
                    cpuutil_min = 0;
                }
            } else
            if (ncpus == 2) {
                cpuutil_min = 0.55;
            } else {
                cpuutil_min = 1 - 1.0*nodect/ncpus;
            }
            double mem_unused = (job->mem_r - job->mem_u)/gb_scale;
            int walltime_unused = job->walltime_r - job->walltime_u;
            if (cpuutil < cpuutil_min || cpuutil > cpuutil_max ||
                (memutil > 0 && mem_unused/nodect > 2.0 && memutil < 0.5) ||
                (job->state == JOB_FINISHED && walltime_unused > 7200 &&
                 wallutil > 0 && wallutil < 0.5)) {
                cpair = COLOR_PAIR_JOB_BAD;
            }
        }

        char timebuf[9];
        format_time(walltime, timebuf);

        int cattrs = COLOR_PAIR(cpair);
        if (i == selpos + HEADER_NROWS) {
            cattrs |= A_REVERSE;
        }

        wattron(win, cattrs);
        if (job->is_array) {
            wattron(win, A_BOLD);
        }
        if (job->aid) {
            int idlen = get_idlen(job->id);
            int aidlen = get_idlen(job->aid);
            int treesym = job->is_last_subjob ? ACS_LLCORNER:ACS_LTEE;
            int is;
            mvwaddch(win, i, 8 - idlen, treesym);
            for (is = 0; is < idlen - aidlen; is++) {
                waddch(win, ACS_HLINE);
            }
            mvwprintw(win, i, 8 - aidlen, "%d", job->aid);
        } else {
            mvwprintw(win, i, 0, "%8d", job->id);
        }
        if (job->is_array) {
            wattroff(win, A_BOLD);
        }

        waddch(win, ' ');

        int memprec, vmemprec;
        if (mem/gb_scale >= 1000) {
            memprec = 0;
        } else {
            memprec = 2;
        }
        if (vmem/gb_scale >= 1000) {
            vmemprec = 0;
        } else {
            vmemprec = 2;
        }
        sprintf(linebuf,
            "%8s %8s %c %6.*f  %3.0f %6.*f %3d  %3.0f %8s %3.0f %s",
            job->user, job->queue, job->state,
            memprec, mem/gb_scale, 100*memutil, vmemprec, vmem/gb_scale, ncpus,
            100*cpuutil, timebuf, job->io_r, job->name);

        getyx(win, y, x);

        if (strlen(linebuf) + x > (unsigned int) COLS + xshift) {
            linebuf[COLS + xshift - x - 1] = '>';
            linebuf[COLS + xshift - x] = '\0';
        }
        linebuf[1023] = '\0';

        wprintw(win, "%s", linebuf + xshift);

        getyx(win, y, x);
        if (x > 0 && x < COLS) {
            wprintw(win, "%*.s", COLS - x, "");
        }

        wattroff(win, cattrs);

        job++;
    }

    wrefresh(win);
}

static int state_rank(job_state_t s)
{
    int rank = 0;
    switch (s) {
    case JOB_EXITING:
        rank = 1;
        break;
    case JOB_SUSPENDED:
        rank = 2;
        break;
    case JOB_RUNNING:
        rank = 3;
        break;
    case JOB_BEGUN:
        rank = 4;
        break;
    case JOB_QUEUED:
        rank = 5;
        break;
    case JOB_WAITING:
        rank = 6;
        break;
    case JOB_HELD:
        rank = 7;
        break;
    case JOB_TRANSIT:
        rank = 8;
        break;
    case JOB_FINISHED:
        rank = 9;
        break;
    }

    return rank;
}

static int job_comp(const void *a, const void *b)
{
    const job_t *ja = a, *jb = b;
    int cmp = 0;
    
    cmp = state_rank(ja->state) - state_rank(jb->state);

    if (cmp == 0) {
        if (ja->user && jb->user) {
            cmp = strcmp(ja->user, jb->user);
        } else {
            cmp = 0;
        }
    }
    if (cmp == 0) {
        if (ja->queue && jb->queue) {
            cmp = strcmp(ja->queue, jb->queue);
        } else {
            cmp = 0;
        }
    }
    if (cmp == 0) {
        return jb->id - ja->id;
    }
    if (cmp == 0) {
        return ja->aid - jb->aid;
    }

    return cmp;
}

static void print_job_details(const qtop_t *q, const job_t *job,
    unsigned int xshift, unsigned int yshift)
{
    werase(q->jwin);

    box(q->jwin, 0, 0);

    if (job && job->id) {
        char idbuf[32];
        if (job->is_array) {
            sprintf(idbuf, "%d[]", job->id);
        } else
        if (job->aid) {
            sprintf(idbuf, "%d[%d]", job->id, job->aid);
        } else {
            sprintf(idbuf, "%d", job->id);
        }
        mvwprintw(q->jwin, 0, 1, "Job ID = %s", idbuf);
        struct batch_status *qstatus = pbs_statjob(q->conn, idbuf, NULL, "x");
        if (qstatus) {
            print_attribs(q->jwin, qstatus->attribs, xshift, yshift);
            pbs_statfree(qstatus);
        }
    }

    wrefresh(q->jwin);
}

static job_t *get_job(job_t *jobs, int njobs, int jid)
{
    if (jobs && jid >= 0 && jid < njobs) {
        return jobs + jid;
    } else {
        return NULL;
    }
}

static int refresh_period = DEFAULT_REFRESH;

volatile sig_atomic_t need_update = false;
void catch_alarm(int sig)
{
    if (sig != SIGALRM) {
        return;
    }

    need_update = true;
    if (refresh_period) {
        alarm(refresh_period);
    }
}

static void usage(const char *arg0, FILE *out)
{
    fprintf(out, "usage: %s [options]\n", arg0);
    fprintf(out, "Available options:\n");
    fprintf(out, "  -u <username> show jobs for username\n");
    fprintf(out, "  -q <queue>    only show jobs in specific queue\n");
    fprintf(out, "  -s <state(s)> only show jobs in specific non-terminal state(s)\n");
    fprintf(out, "  -e <host>     only show jobs running on specific host\n");
    fprintf(out, "  -f            show finished jobs\n");
    fprintf(out, "  -F            only show failed jobs (implies -f)\n");
    fprintf(out, "  -H <hours>    history span for finished jobs [%d]\n",
        DEFAULT_HISTORY);
    fprintf(out, "  -S            include array subjobs\n");
    fprintf(out, "  -R <secs>     refresh period [%d]\n", refresh_period);
    fprintf(out, "  -C            start in monochrome mode\n");
    fprintf(out, "  -V            print version info and exit\n");
    fprintf(out, "  -h            print this help\n");
}

static void about(void)
{
    fprintf(stdout, "qtop-%s\n", QTOP_VERSION);
    fprintf(stdout, "Written by Evgeny Stambulchik.\n");
}

int main(int argc, char * const argv[])
{
    char *server_name = NULL;
    char *username = NULL;
    char *queue = NULL;
    char *state = NULL;
    char *exec_host = NULL;
    bool finished = false;
    bool failed = false;
    bool subjobs = false;
    int history_span = DEFAULT_HISTORY;
    bool bw = false;

    int uid = getuid();
    if (uid != 0) {
        struct passwd *p = getpwuid(uid);
        if (p) {
            username = p->pw_name;
        }
    }

    int opt;

    while ((opt = getopt(argc, argv, "u:q:s:e:fFH:R:SCVh")) != -1) {
        switch (opt) {
        case 'u':
            if (strcmp(optarg, "all")) {
                username = optarg;
            } else {
                username = NULL;
            }
            break;
        case 'q':
            queue = optarg;
            break;
        case 's':
            state = optarg;
            break;
        case 'e':
            exec_host = optarg;
            break;
        case 'f':
            finished = true;
            break;
        case 'F':
            finished = true;
            failed = true;
            break;
        case 'H':
            history_span = atoi(optarg);
            break;
        case 'R':
            refresh_period = atoi(optarg);
            break;
        case 'S':
            subjobs = true;
            break;
        case 'C':
            bw = true;
            break;
        case 'V':
            about();
            exit(0);
            break;
        case 'h':
            usage(argv[0], stdout);
            exit(0);
            break;
        default:
            usage(argv[0], stderr);
            exit(1);
            break;
        }
    }

    qtop_t *qtop = qtop_new(server_name);
    if (!qtop) {
        fprintf(stderr, "Failed connecting to server, errno = %d\n", pbs_errno);
        exit(1);
    }
    qtop->username     = username;
    qtop->queue        = queue;
    qtop->state        = state;
    qtop->exec_host    = exec_host;
    qtop->finished     = finished;
    qtop->failed       = failed;
    qtop->history_span = history_span;
    qtop->subjobs      = subjobs;

    server_t *pbs = pbs_server_new();

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    set_escdelay(0);
    curs_set(0);
    timeout(1000);

    if (!bw && has_colors()) {
        start_color();
        use_default_colors();
        if (can_change_color()) {
            init_color(COLOR_WHITE, 1000, 1000, 1000);
            init_color(COLOR_RED,    800,  100,  100);
            init_color(COLOR_BLUE,     0,  400, 1000);
            init_color(COLOR_GREEN,    0,  800,  100);
        }
        init_pair(COLOR_PAIR_HEADER,    COLOR_BLUE,    -1);
        init_pair(COLOR_PAIR_JHEADER,           -1,    -1);
        init_pair(COLOR_PAIR_JOB_R,     COLOR_GREEN,   -1);
        init_pair(COLOR_PAIR_JOB_Q,     COLOR_CYAN,    -1);
        init_pair(COLOR_PAIR_JOB_W,     COLOR_YELLOW,  -1);
        init_pair(COLOR_PAIR_JOB_H,     COLOR_MAGENTA, -1);
        init_pair(COLOR_PAIR_JOB_S,     COLOR_YELLOW,  -1);
        init_pair(COLOR_PAIR_JOB_OTHER, COLOR_BLACK,   -1);
        init_pair(COLOR_PAIR_JOB_BAD,   COLOR_RED,     -1);
    }

    qtop->jwin = newwin(LINES - HEADER_NROWS, COLS, HEADER_NROWS, 0);

    qtop_server_update(qtop, pbs);

    int njobs;
    job_t *jobs = qtop_server_jobs(qtop, &njobs, 0);
    qsort(jobs, njobs, sizeof(job_t), job_comp);

    signal(SIGALRM, catch_alarm);
    if (refresh_period) {
        alarm(refresh_period);
    }

    bool job_details = false;
    bool first_time = true;
    int ch = 0;
    int jid_start = 0;
    int selpos = 0;
    unsigned int xshift = 0, yshift = 0;
    unsigned int joblist_xshift = 0;
    unsigned int ajob_id_expanded = 0;
    do {
        int page_lines = LINES - HEADER_NROWS;
        int ij;
        bool need_joblist_refresh = true;
        job_t *ajob;
        
        switch (ch) {
        case KEY_UP:
            if (job_details) {
                if (yshift > 0) {
                    yshift--;
                }
            } else {
                selpos--;
            }
            break;
        case KEY_DOWN:
            if (job_details) {
                yshift++;
            } else {
                selpos++;
            }
            break;
        case 'j':
            selpos++;
            break;
        case 'k':
            selpos--;
            break;
        case KEY_LEFT:
            if (job_details) {
                if (xshift > 0) {
                    xshift--;
                }
            } else {
                if (joblist_xshift > 0) {
                    joblist_xshift--;
                }
            }
            break;
        case KEY_RIGHT:
            if (job_details) {
                xshift++;
            } else {
                joblist_xshift++;
            }
            break;
        case KEY_PPAGE:
            jid_start -= page_lines;
            break;
        case KEY_NPAGE:
            jid_start += page_lines;
            break;
        case KEY_HOME:
            jid_start = 0;
            selpos = 0;
            break;
        case KEY_END:
            jid_start = njobs - page_lines;
            selpos = page_lines - 1;
            break;
        case 'r':
            need_update = true;
            break;
        case '\n':
        case '\r':
        case KEY_ENTER:
            job_details = !job_details;
            break;
        case ' ':
            ajob = get_job(jobs, njobs, jid_start + selpos);
            if (ajob && ajob->is_array) {
                need_update = true;
                if (ajob_id_expanded == ajob->id) {
                    ajob_id_expanded = 0;
                } else {
                    ajob_id_expanded = ajob->id;
                }
            }
            break;
        case 27:
            job_details = false;
            break;
        case KEY_RESIZE:
            delwin(qtop->jwin);
            qtop->jwin = newwin(LINES - HEADER_NROWS, COLS, HEADER_NROWS, 0);
            break;
        default:
            need_joblist_refresh = false;
            break;
        }

        if (first_time) {
            first_time = false;
            need_joblist_refresh = true;
        }

        if (need_update && !job_details) {
            need_update = false;
            need_joblist_refresh = true;
            
            qtop_server_update(qtop, pbs);

            for (ij = 0; ij < njobs; ij++) {
                job_t *job = jobs + ij;
                job_free_data(job);
            }
            xfree(jobs);

            jobs = qtop_server_jobs(qtop, &njobs, ajob_id_expanded);
            if (!jobs && pbs_errno == PBSE_EXPIRED) {
                qtop_reconnect(qtop);
                qtop_server_update(qtop, pbs);
                jobs = qtop_server_jobs(qtop, &njobs, ajob_id_expanded);
            }
            qsort(jobs, njobs, sizeof(job_t), job_comp);
        }

        if (selpos < 0) {
            selpos++;
            jid_start--;
        } else
        if (selpos >= page_lines) {
            selpos--;
            jid_start++;
        }

        if (jid_start + page_lines > njobs) {
            jid_start = njobs - page_lines;
        }
        if (jid_start < 0) {
            jid_start = 0;
        }
        
        if (selpos >= njobs - jid_start) {
            selpos = njobs - jid_start - 1;
        }
        if (selpos < 0) {
            selpos = 0;
        }

        // If there are no jobs selected, ignore the request to show details
        // of any
        if (!njobs) {
            job_details = false;
        }

        if (!job_details && need_joblist_refresh) {
            werase(stdscr);
        }

        print_server_stats(pbs, stdscr);

        if (job_details) {
            print_job_details(qtop, get_job(jobs, njobs, jid_start + selpos),
                xshift, yshift);
        } else {
            xshift = 0;
            yshift = 0;
            if (need_joblist_refresh) {
                print_jobs(jobs + jid_start, njobs - jid_start, stdscr, selpos,
                    joblist_xshift);
            }
        }
    } while ((ch = getch()) != 'q');

    endwin();

    pbs_server_free(pbs);

    exit(0);
}
