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

static void print_attribs(WINDOW *win, const struct attrl *attribs)
{
    int y = 1, x, maxx, maxy;
    getmaxyx(win, maxy, maxx);
    const struct attrl *qattr = attribs;
    while (qattr && y < maxy - 1) {
        // Skip over lengthy/rarely important stuff...
        if (strcmp(qattr->name, ATTR_v) &&
            strcmp(qattr->name, ATTR_submit_arguments)) {
            if (qattr->resource != NULL) {
                mvwprintw(win, y, 1, "%s.%s = ", qattr->name, qattr->resource);
            } else {
                mvwprintw(win, y, 1, "%s = ", qattr->name);
            }
            getyx(win, y, x);
            if (x + (int)strlen(qattr->value) >= maxx - 1) {
                qattr->value[maxx - x - 1] = '\0';
            }
            mvwprintw(win, y, x, "%s", qattr->value);
            y++;
        }

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

job_t *qtop_server_jobs(qtop_t *q, int *njobs)
{
    struct batch_status *qstatus, *qtmp;
    struct attrl *qattribs = NULL;
    struct attropl *criteria_list = NULL;
    char extend[3] = "";

    if (q->subjobs) {
        strcat(extend, "t");
    }
    if (q->finished) {
        strcat(extend, "x");
    }

    qattribs = calloc(6, sizeof(struct attrl));
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
    qattribs[5].name = ATTR_used;
    qattribs[5].value = "";
    qattribs[5].next = NULL;

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

    job_t *jobs = calloc(*njobs, sizeof(job_t));
    if (!jobs) {
        *njobs = 0;
        pbs_statfree(qstatus);
        return NULL;
    }

    qtmp = qstatus;
    jid = 0;
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
                sscanf(qtmp->name, "%ud[]", &job->id);
            } else {
                sscanf(qtmp->name, "%ud[%ud]", &job->id, &job->aid);
            }
        } else {
            job->id = atoi(qtmp->name);
        }

        parse_job_attribs(job, qtmp->attribs);
        qtmp = qtmp->next;
        jid++;
    }

    /* free allocated data */
    xfree(qattribs);
    attropl_free(criteria_list);
    pbs_statfree(qstatus);

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

    mvwprintw(win, 1, 0, "Mem: %.1f GB, VMem: %.1f GB, Cores: %d (SP:%d + MP:%d)  %s",
        pbs->mem/gb_scale, pbs->vmem/gb_scale, pbs->ncpus,
        pbs->ncpus - pbs->mpiprocs, pbs->mpiprocs, datebuf);

    wattroff(win, COLOR_PAIR(COLOR_PAIR_HEADER));
    wrefresh(win);
}

static bool format_time(int secs, char buf[9])
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

void print_jobs(const job_t *jobs, int njobs, WINDOW *win, int selpos)
{
    int i;

    wattron(win, COLOR_PAIR(COLOR_PAIR_JHEADER) | A_BOLD);
    const char *header =
        "  Job ID     User    Queue S    Mem %Mem   VMem  NC %CPU Walltime I/O Name";
    mvwprintw(win, HEADER_NROWS - 1, 0, "%-*s", COLS, header);
    wattroff(win, COLOR_PAIR(COLOR_PAIR_JHEADER) | A_BOLD);

    const double gb_scale = pow(2, 20);
    const job_t *job = jobs;
    for (i = HEADER_NROWS; i < LINES && i < njobs + HEADER_NROWS; i++) {
        double mem, vmem;
        long cput, walltime;
        int ncpus;
        double cpuutil = 0, memutil = 0, wallutil = 0;

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
            if (ncpus == 1) {
                cpuutil_min = 0.5;
                if (job->io_r > 1.0) {
                    cpuutil_min = 0;
                }
            } else
            if (ncpus == 2) {
                cpuutil_min = 0.55;
            } else {
                cpuutil_min = 1 - 1.0/ncpus;
            }
            double mem_unused = (job->mem_r - job->mem_u)/gb_scale;
            int walltime_unused = job->walltime_r - job->walltime_u;
            if (cpuutil < cpuutil_min || cpuutil > cpuutil_max ||
                (memutil > 0 && mem_unused > 2.0 && memutil < 0.5) ||
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
        mvwprintw(win, i, 0, "%8d", job->id);
        if (job->is_array) {
            wattroff(win, A_BOLD);
        }
        wprintw(win,
            " %8s %8s %c %6.2f  %3.0f %6.2f %3d  %3.0f %8s %3.0f %-*s",
            job->user, job->queue, job->state,
            mem/gb_scale, 100*memutil, vmem/gb_scale, ncpus, 100*cpuutil, timebuf,
            job->io_r, COLS - 70, job->name);
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

static void print_job_details(const qtop_t *q, const job_t *job)
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
            print_attribs(q->jwin, qstatus->attribs);
            pbs_statfree(qstatus);
        }
    }

    wrefresh(q->jwin);
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
    fprintf(out, "  -f            show finished jobs\n");
    fprintf(out, "  -F            only show failed jobs (implies -f)\n");
    fprintf(out, "  -H <hours>    history span for finished jobs [%d]\n",
        DEFAULT_HISTORY);
    fprintf(out, "  -S            include array subjobs\n");
    fprintf(out, "  -R <secs>     refresh period [%d]\n", refresh_period);
    fprintf(out, "  -v            print version info and exit\n");
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
    bool finished = false;
    bool failed = false;
    bool subjobs = false;
    int history_span = DEFAULT_HISTORY;

    int uid = getuid();
    if (uid != 0) {
        struct passwd *p = getpwuid(uid);
        if (p) {
            username = p->pw_name;
        }
    }

    int opt;

    while ((opt = getopt(argc, argv, "u:q:s:fFH:R:Svh")) != -1) {
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
        case 'v':
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

    start_color();
    use_default_colors();
    init_color(COLOR_WHITE, 1000, 1000, 1000);
    init_pair(COLOR_PAIR_HEADER,    COLOR_BLUE,    COLOR_WHITE);
    init_pair(COLOR_PAIR_JHEADER,   COLOR_WHITE,   COLOR_BLACK);
    init_pair(COLOR_PAIR_JOB_R,     COLOR_GREEN,   COLOR_WHITE);
    init_pair(COLOR_PAIR_JOB_Q,     COLOR_CYAN,    COLOR_WHITE);
    init_pair(COLOR_PAIR_JOB_W,     COLOR_YELLOW,  COLOR_WHITE);
    init_pair(COLOR_PAIR_JOB_H,     COLOR_MAGENTA, COLOR_WHITE);
    init_pair(COLOR_PAIR_JOB_S,     COLOR_YELLOW,  COLOR_WHITE);
    init_pair(COLOR_PAIR_JOB_OTHER, COLOR_BLACK,   COLOR_WHITE);
    init_pair(COLOR_PAIR_JOB_BAD,   COLOR_RED,     COLOR_WHITE);

    qtop->jwin = newwin(LINES - HEADER_NROWS, COLS, HEADER_NROWS, 0);

    qtop_server_update(qtop, pbs);

    int njobs;
    job_t *jobs = qtop_server_jobs(qtop, &njobs);
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
    do {
        int page_lines = LINES - HEADER_NROWS;
        int ij;
        bool need_joblist_refresh = true;
        
        switch (ch) {
        case KEY_UP:
            selpos--;
            break;
        case KEY_DOWN:
            selpos++;
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

        if (need_update) {
            need_update = false;
            need_joblist_refresh = true;
            
            qtop_server_update(qtop, pbs);

            for (ij = 0; ij < njobs; ij++) {
                job_t *job = jobs + ij;
                job_free_data(job);
            }
            xfree(jobs);

            jobs = qtop_server_jobs(qtop, &njobs);
            if (!jobs && pbs_errno == PBSE_EXPIRED) {
                qtop_reconnect(qtop);
                qtop_server_update(qtop, pbs);
                jobs = qtop_server_jobs(qtop, &njobs);
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

        if (!job_details && need_joblist_refresh) {
            werase(stdscr);
        }

        print_server_stats(pbs, stdscr);

        if (job_details) {
            print_job_details(qtop, jobs + jid_start + selpos);
        } else
        if (need_joblist_refresh) {
            print_jobs(jobs + jid_start, njobs - jid_start, stdscr, selpos);
        }
    } while ((ch = getch()) != 'q');

    endwin();

    pbs_server_free(pbs);

    exit(0);
}
