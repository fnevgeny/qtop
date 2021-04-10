# qtop - a top-like job viewer for OpenPBS

qtop aims to help in monitoring and analyzing status/performance of jobs.

By default, it will list all your active (i.e., not finished) jobs. To list
finished ones (by default, during the last 24 hours), use the `-f` switch. With
`-F`, only the failed jobs are listed.

Use `Arrow up/down`, `Page up/down`, `Home`, and `End` to navigate the list.

The list is automatically refreshed every 30 seconds. The job counters are
not continuously monitored by the server, so typically there is no sense to
refresh more frequently. Press `r` to force a refresh.

Press `q` to exit.

For each job, `mem`, `vmem` (in units of GB), `walltime`, `io`, and # of CPU's
are listed (for queued jobs, the respective requested values are shown). In
addition, there are CPU and memory utilization (%) metrics, calculated as a
used/requested ratio.

Press `Enter` to see a detailed report (similar to `qstat -f`) of a currently
highlighted job. `Enter` again or `Escape` to exit this mode.

ID's of array jobs are typeset in bold. Press `space` to expand, showing subjobs.

Misbehaving jobs are marked in red. That means at least one of the following
"badness" criteria was triggered:

* Memory utilization is below 50% with at least 2GB wasted;

* CPU utilization is noticeably less than 100% (unless it's a single-processor,
I/O job with a respective `io` set);

* (For finished jobs) walltime utilization is less than 50% with at least 2
hours unused.
