/*  =========================================================================
    FtyProtoStateHolder

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

#include "fty_alert_stats_classes.h"

static void fty_proto_destroy_wrapper(fty_proto_t *p)
{
    fty_proto_destroy(&p);
}

void FtyAssetStateHolder::processAsset(fty_proto_t *asset)
{
    const char *operation = fty_proto_operation(asset);
    const char *name = fty_proto_name(asset);

    if (operation && name) {
        if (streq(operation, FTY_PROTO_ASSET_OP_DELETE) || streq(operation, FTY_PROTO_ASSET_OP_RETIRE)) {
            if (callbackAssetPre(asset)) {
                m_assets.erase(name);
                callbackAssetPost(asset);
            }
            fty_proto_destroy_wrapper(asset);
        }
        else {
            if (callbackAssetPre(asset))
            {
                m_assets[name] = FtyProto(asset, fty_proto_destroy_wrapper);
                callbackAssetPost(asset);
            }
        }
    }
    else {
        fty_proto_destroy_wrapper(asset);
    }
}

void FtyAlertStateHolder::processAlert(fty_proto_t *alert)
{
    const char *state = fty_proto_state(alert);
    const char *rule = fty_proto_rule(alert);

    if (state && rule) {
        if (streq(state, "RESOLVED")) {
            if (callbackAlertPre(alert))
            {
                m_alerts.erase(rule);
                callbackAlertPost(alert);
            }
            fty_proto_destroy_wrapper(alert);
        }
        else {
            if (callbackAlertPre(alert))
            {
                m_alerts[rule] = FtyProto(alert, fty_proto_destroy_wrapper);
                callbackAlertPost(alert);
            }
        }
    }
    else {
        fty_proto_destroy_wrapper(alert);
    }
}

void FtyAlertStateHolder::purgeExpiredAlerts()
{
    auto it = m_alerts.begin();
    while (it != m_alerts.end())
    {
        fty_proto_t *proto = it->second.get();
        it++;

        if ((fty_proto_time(proto) + fty_proto_ttl(proto)) < (zclock_mono()/1000)) {
            fty_proto_t *dup = fty_proto_dup(proto);
            fty_proto_set_state(dup, "RESOLVED");
            processAlert(dup);
        }
    }
}
