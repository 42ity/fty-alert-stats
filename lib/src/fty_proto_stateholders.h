/*  =========================================================================
    FtyProtoStateHolders - Agent for computing aggregate statistics on alerts

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
#include <fty_proto.h>
#include <functional>
#include <map>
#include <memory>
#include <string>

typedef std::unique_ptr<fty_proto_t, std::function<void(fty_proto_t*)>> FtyProto;
typedef std::map<std::string, FtyProto>                                 FtyProtoCollection;

/// Helper class for tracking fty_proto_t assets.
class FtyAssetStateHolder
{
public:
    virtual ~FtyAssetStateHolder() = default;

protected:
    /// Process the asset and call the callbacks methods.
    ///
    /// This method takes ownership of the fty_proto_t object.
    void processAsset(fty_proto_t* asset);

    /// Callback called before registering (or deleting) an asset. The method must
    /// NOT take ownership of the object.
    /// @param asset fty_proto_t asset object
    /// @return true if the asset shall be registered/deleted, false if it shall
    /// be ignored.
    virtual bool callbackAssetPre(fty_proto_t* /*asset*/)
    {
        return true;
    }

    /// Callback called after registering (or deleting) an asset. The method must
    /// NOT take ownership of the object.
    /// @param asset fty_proto_t asset object
    virtual void callbackAssetPost(fty_proto_t* /*asset*/)
    {
    }

    /// Collection of known assets.
    FtyProtoCollection m_assets;
};

/// Helper class for tracking fty_proto_t alerts.
class FtyAlertStateHolder
{
public:
    virtual ~FtyAlertStateHolder() = default;

protected:
    /// Process the alert and call the callbacks methods.
    ///
    /// This method takes ownership of the fty_proto_t object.
    void processAlert(fty_proto_t* alert);

    /// Call resolve callbacks on expired alerts (i.e. delete them).
    void purgeExpiredAlerts();

    /// Callback called before registering (or deleting) an alert. The method must
    /// NOT take ownership of the object.
    /// @param asset fty_proto_t alert object
    /// @return true if the alert shall be registered/deleted, false if it shall
    /// be ignored.
    virtual bool callbackAlertPre(fty_proto_t* /*alert*/)
    {
        return true;
    }

    /// Callback called after registering (or deleting) an alert. The method must
    /// NOT take ownership of the object.
    /// @param asset fty_proto_t alert object
    virtual void callbackAlertPost(fty_proto_t* /*alert*/)
    {
    }

    /// Collection of known alerts.
    FtyProtoCollection m_alerts;
};
