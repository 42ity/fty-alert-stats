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

#include "fty_alert_stats_server.h"
#include <fty_log.h>

static zactor_t *g_alert_stats_server = nullptr;

static int s_resync_timer_fn (zloop_t */*loop*/, int /*timer_id*/, void *output)
{
    if (output) {
        zstr_send (output, "RESYNC");
    }
    return 0;
}

static int s_resync_reader_fn (zloop_t */*loop*/, zsock_t */*reader*/, void */*arg*/)
{
    return -1;
}

static void s_resync_actor (zsock_t *pipe, void* resyncPeriod)
{
    // Parse resyncPeriod, falling back to default period on failure.
    size_t loop_delay = 43200; // sec.
    try {
        loop_delay = std::stoul(reinterpret_cast<const char*>(resyncPeriod));
    }
    catch (...) {
        log_error("Invalid resync delay '%s'.", reinterpret_cast<const char*>(resyncPeriod));
    }

    log_info("Resync actor delay: %zu", loop_delay);

    zloop_t *resync_loop = zloop_new();
    zloop_timer(resync_loop, loop_delay * 1000, 0, s_resync_timer_fn, g_alert_stats_server);
    zloop_reader(resync_loop, pipe, s_resync_reader_fn, nullptr);
    zloop_start(resync_loop);
    zloop_destroy(&resync_loop);
}

int main (int argc, char *argv [])
{
    const char* AGENT_NAME = "fty-alert-stats";
    const char* MLM_ENDPOINT = "ipc://@/malamute";

    const char* metricTTL = "720"; // sec.
    const char* pollerTimeout = "180"; // sec.
    const char* resyncPeriod = "43200"; // sec.

    const char* CONFIGFILE = "";
    bool verbose = false;

    ftylog_setInstance(AGENT_NAME, FTY_COMMON_LOGGING_DEFAULT_CFG);

    for (int argn = 1; argn < argc; argn++) {
        std::string arg{argv[argn]};
        const char* next = ((argn + 1) < argc) ? argv[argn + 1] : nullptr;

        if ((arg == "-h") || (arg == "--help")) {
            printf ("%s [options] ...\n", AGENT_NAME);
            printf ("  -c|--config    configuration file\n");
            printf ("  -v|--verbose   verbose output\n");
            printf ("  -h|--help      this information\n");
            return EXIT_SUCCESS;
        }
        else if ((arg == "-v") || (arg == "--verbose")) {
            verbose = true;
        }
        else if ((arg == "-c") || (arg == "--config")) {
            if (!next) {
                log_error ("Missing %s argument", arg.c_str());
                return EXIT_FAILURE;
            }
            CONFIGFILE = next;
            argn++;
        }
        else {
            log_error ("Unknown option: %s", arg.c_str());
            return EXIT_FAILURE;
        }
    }

    zconfig_t *config = NULL;
    if (!streq(CONFIGFILE, "")) {
        log_info ("%s Loading '%s'...", AGENT_NAME, CONFIGFILE);

        config = zconfig_load(CONFIGFILE);
        if (config) {
            metricTTL = zconfig_get(config, "agent/metric_ttl", metricTTL);
            pollerTimeout = zconfig_get(config, "agent/poller_timeout", pollerTimeout);
            resyncPeriod = zconfig_get(config, "agent/resync_period", resyncPeriod);
        }
        else {
            log_error ("%s Failed to load '%s'", AGENT_NAME, CONFIGFILE);
        }
    }

    log_info ("%s starting...", AGENT_NAME);

    if (verbose) {
        ftylog_setVerboseMode(ftylog_getInstance());
        log_trace ("%s Verbose", AGENT_NAME);
    }

    AlertStatsActorParams params;
    params.endpoint = MLM_ENDPOINT;
    params.address = AGENT_NAME;
    params.metricTTL = std::stol(metricTTL);
    params.pollerTimeout = std::stol(pollerTimeout);

    // create stats server
    g_alert_stats_server = zactor_new (fty_alert_stats_server, reinterpret_cast<void*>(&params));
    if (!g_alert_stats_server) {
        log_fatal("%s alert_stats_server creation failed", AGENT_NAME);
        return EXIT_FAILURE;
    }
    // Tell stats server to resync right now
    zstr_send (g_alert_stats_server, "RESYNC");

    // create resync actor (tell stats actor to RESYNC periodically)
    zactor_t *resync_actor = zactor_new (s_resync_actor, const_cast<char*>(resyncPeriod));
    if (!resync_actor) {
        zactor_destroy (&g_alert_stats_server);
        log_fatal("%s resync_actor creation failed", AGENT_NAME);
        return EXIT_FAILURE;
    }

    log_info ("%s started", AGENT_NAME);

    // main loop, accept any message back from server
    // copy from src/malamute.c under MPL license
    while (!zsys_interrupted) {
        char* msg = zstr_recv(g_alert_stats_server);
        if (!msg)
            break;

        log_debug("%s Recv msg '%s'", AGENT_NAME, msg);
        zstr_free(&msg);
    }

    zactor_destroy (&resync_actor);
    zactor_destroy (&g_alert_stats_server);
    zconfig_destroy (&config);

    log_info ("%s ended", AGENT_NAME);
    return EXIT_SUCCESS;
}
