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

#pragma once
#include <czmq.h>
#include <string>

struct AlertStatsActorParams
{
    std::string endpoint;
    std::string address;
    int64_t     pollerTimeout{0}; // sec
    int64_t     metricTTL{0}; // sec
};

//  This is the actor constructor as zactor_fn
void fty_alert_stats_server(zsock_t* pipe, void* args);
