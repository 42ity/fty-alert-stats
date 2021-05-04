/*  =========================================================================
    fty_alert_stats - Binary

    Copyright (C) 2014 - 2020 Eaton

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
    =========================================================================
*/

/*
@header
    fty_alert_stats - Binary
@discuss
@end
*/

#include <fty_log.h>
#include "fty_alert_stats_library.h"

static zactor_t *alert_stats_server;

static int s_resync_timer (zloop_t *loop, int timer_id, void *output)
{
    zstr_send (output, "RESYNC");
    return 0;
}

static int s_resync_actor_pipe (zloop_t *loop, zsock_t *reader, void *arg)
{
    return -1;
}

static void s_resync_actor (zsock_t *pipe, void* resyncPeriod)
{
    // Parse resyncPeriod, falling back to default period on failure.
    size_t loop_delay = 43200; // sec.
    try {
        loop_delay = std::stoul((const char*)resyncPeriod);
    }
    catch (...) {
        log_error("Invalid resync period '%s'.", (const char*)resyncPeriod);
    }

    loop_delay *= 1000; // msec.

    zloop_t *resync_loop = zloop_new();
    zloop_timer(resync_loop, loop_delay, 0, s_resync_timer, alert_stats_server);
    zloop_reader(resync_loop, pipe, s_resync_actor_pipe, nullptr);
    zloop_start(resync_loop);
    zloop_destroy(&resync_loop);
}

int main (int argc, char *argv [])
{
    const char * CONFIGFILE = "";
    const char * LOGCONFIGFILE = FTY_COMMON_LOGGING_DEFAULT_CFG;
    const char * metricTTL = "720"; // sec.
    const char * tickPeriod = "180"; // sec.
    const char * resyncPeriod = "43200"; // sec.

    ftylog_setInstance("fty-alert-stats", FTY_COMMON_LOGGING_DEFAULT_CFG);

    bool verbose = false;
    int argn;
    for (argn = 1; argn < argc; argn++) {
        if (streq (argv [argn], "--help")
        ||  streq (argv [argn], "-h")) {
            puts ("fty-alert-stats [options] ...");
            puts ("  --config / -c          configuration file");
            puts ("  --verbose / -v         verbose test output");
            puts ("  --help / -h            this information");
            return 0;
        }
        else
        if (streq (argv [argn], "--verbose")
        ||  streq (argv [argn], "-v"))
            verbose = true;
        else
        if (streq (argv [argn], "--config")
        ||  streq (argv [argn], "-c")) {
            if ((argn + 1) >= argc)  {
                log_error ("Missing -c argument");
                return EXIT_FAILURE;
            }
            CONFIGFILE = argv [argn + 1];
            ++argn;
        }
        else {
            log_error ("Unknown option: %s\n", argv [argn]);
            return EXIT_FAILURE;
        }
    }

    zconfig_t *config = NULL;
    if (!streq(CONFIGFILE,"")) {
        log_info ("Loading config file '%s'...", CONFIGFILE);

        config = zconfig_load(CONFIGFILE);
        if (config) {
            LOGCONFIGFILE = zconfig_get(config, "log/config", "");
            metricTTL = zconfig_get(config, "agent/metric_ttl", metricTTL);
            tickPeriod = zconfig_get(config, "agent/tick_period", tickPeriod);
            resyncPeriod = zconfig_get(config, "agent/resync_period", resyncPeriod);
            //log_info ("Config file loaded (%s)", CONFIGFILE);
        }
        else {
            log_error ("Couldn't load config file (%s)", CONFIGFILE);
        }
    }

    if (!streq(LOGCONFIGFILE,"")) {
        ftylog_setConfigFile(ftylog_getInstance(), LOGCONFIGFILE);
    }

    log_info ("fty-alert-stats starting...");

    if (verbose) {
        ftylog_setVerboseMode(ftylog_getInstance());
        log_trace ("Verbose mode OK");
    }

    AlertStatsActorParams params;
    params.endpoint = "ipc://@/malamute";
    params.metricTTL = std::stol(metricTTL);
    params.pollerTimeout = std::stol(tickPeriod) * 1000;
    alert_stats_server = zactor_new (fty_alert_stats_server, (void *) &params);
    if (!alert_stats_server) {
        log_fatal("alert_stats_server creation failed");
        return EXIT_FAILURE;
    }

    // Tell actor to fetch data right away
    zstr_send (alert_stats_server, "RESYNC");

    // Periodically resync actor
    zactor_t *resync_actor = zactor_new (s_resync_actor, const_cast<char*>(resyncPeriod));
    if (!resync_actor) {
        zactor_destroy (&alert_stats_server);
        log_fatal("resync_actor creation failed");
        return EXIT_FAILURE;
    }

    while (!zsys_interrupted) {
        zmsg_t *msg = zmsg_recv (alert_stats_server);
        if (msg) {
            char *cmd = zmsg_popstr (msg);
            zsys_debug ("main: %s received", cmd ? cmd : "(null)");
            zstr_free (&cmd);
            zmsg_destroy (&msg);
        }
    }

    zactor_destroy (&resync_actor);
    zactor_destroy (&alert_stats_server);
    zconfig_destroy (&config);

    log_info ("fty-alert-stats ended");
    return EXIT_SUCCESS;
}
