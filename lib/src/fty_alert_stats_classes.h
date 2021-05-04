/*  =========================================================================
    fty_alert_stats_classes - private header file

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

#ifndef FTY_ALERT_STATS_CLASSES_H_INCLUDED
#define FTY_ALERT_STATS_CLASSES_H_INCLUDED

//  External dependencies
#include <czmq.h>
#include <malamute.h>
#include <cxxtools/allocator.h>
#include <fty_log.h>
#include <fty_proto.h>
#include <fty_shm.h>
#include <fty_common_mlm.h>

#if defined (__WINDOWS__)
#   if defined FTY_ALERT_STATS_STATIC
#       define FTY_ALERT_STATS_EXPORT
#   elif defined FTY_ALERT_STATS_INTERNAL_BUILD
#       if defined DLL_EXPORT
#           define FTY_ALERT_STATS_EXPORT __declspec(dllexport)
#       else
#           define FTY_ALERT_STATS_EXPORT
#       endif
#   elif defined FTY_ALERT_STATS_EXPORTS
#       define FTY_ALERT_STATS_EXPORT __declspec(dllexport)
#   else
#       define FTY_ALERT_STATS_EXPORT __declspec(dllimport)
#   endif
#   define FTY_ALERT_STATS_PRIVATE
#elif defined (__CYGWIN__)
#   define FTY_ALERT_STATS_EXPORT
#   define FTY_ALERT_STATS_PRIVATE
#else
#   if (defined __GNUC__ && __GNUC__ >= 4) || defined __INTEL_COMPILER
#       define FTY_ALERT_STATS_PRIVATE __attribute__ ((visibility ("hidden")))
#       define FTY_ALERT_STATS_EXPORT __attribute__ ((visibility ("default")))
#   else
#       define FTY_ALERT_STATS_PRIVATE
#       define FTY_ALERT_STATS_EXPORT
#   endif
#endif

//  Internal API
#include "fty_proto_stateholders.h"
#include "fty_alert_stats_actor.h"
#include "fty_alert_stats_server.h"

#endif
