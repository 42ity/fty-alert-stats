/*  =========================================================================
    fty_alert_stats - Binary

    Copyright (C) 2014 - 2018 Eaton

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

#include "fty_alert_stats_classes.h"

static int s_resync_timer (zloop_t *loop, int timer_id, void *output)
{
    zstr_send (output, "RESYNC");
    return 0;
}

int main (int argc, char *argv [])
{
    const char * CONFIGFILE = "";
    const char * LOGCONFIGFILE = "";
    
    ftylog_setInstance("fty-alert-stats","");
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
            CONFIGFILE = argv [argn + 1];
            ++argn;
        }
        else {
            log_error ("Unknown option: %s\n", argv [argn]);
            return 1;
        }
    }
    //  Insert main code here
    if (!streq(CONFIGFILE,"")) {
        zconfig_t *cfg = zconfig_load(CONFIGFILE);
        if (cfg) {
            LOGCONFIGFILE = zconfig_get(cfg, "log/config", "");
        }
    }
        
    if (!streq(LOGCONFIGFILE,"")) {
        ftylog_setConfigFile(ftylog_getInstance(),LOGCONFIGFILE);
    }
    
    if (verbose)
    {
        ftylog_setVeboseMode(ftylog_getInstance());
        log_trace ("Verbose mode OK");
    }
    log_info ("fty-alert-stats starting");
    const char *endpoint = "ipc://@/malamute";
    zactor_t *alert_stats_server = zactor_new (fty_alert_stats_server, (void *) endpoint);

    // Tell actor to fetch data right away
    zstr_send (alert_stats_server, "RESYNC");

    zloop_t *resync_stream = zloop_new();
    zloop_timer(resync_stream, AlertStatsActor::RESYNC_INTERVAL * 1000, 0, s_resync_timer, alert_stats_server);
    zloop_start(resync_stream);

    //  Accept and print any message back from server
    //  copy from src/malamute.c under MPL license
    while (true) {
        char *message = zstr_recv (alert_stats_server);
        if (message) {
            puts (message);
            free (message);
        }
        else {
            log_info ("interrupted");
            break;
        }
    }

    zloop_destroy (&resync_stream);
    zactor_destroy (&alert_stats_server);

    return EXIT_SUCCESS;
}
