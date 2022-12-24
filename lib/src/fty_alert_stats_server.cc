/*  =========================================================================
    fty_alert_stats_server - Actor

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

#include "fty_alert_stats_server.h"
#include "fty_alert_stats_actor.h"
#include <fty_log.h>
#include <stdexcept>

//  --------------------------------------------------------------------------
//  Actor function

void fty_alert_stats_server(zsock_t* pipe, void* args)
{
    const AlertStatsActorParams* params = reinterpret_cast<const AlertStatsActorParams*>(args);

    try {
        AlertStatsActor alertStatsServer(pipe, params->endpoint, params->address, params->pollerTimeout, params->metricTTL);
        alertStatsServer.mainloop();
    } catch (std::runtime_error& e) {
        log_error("std::runtime_error exception caught, aborting actor (most likely died while initializing).");
    } catch (...) {
        log_error("Unexpected exception caught, aborting actor.");
    }
}
