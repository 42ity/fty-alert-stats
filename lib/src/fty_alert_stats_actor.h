/*  =========================================================================
    fty-alert-stats - Agent for computing aggregate statistics on alerts

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
#include "fty_proto_stateholders.h"
#include <fty_common_mlm_agent.h>

/// Agent for publishing aggregate metric statitics for alerts by asset.
///
/// This agent is normally a purely reactive one, where received alerts (or
/// assets) will trigger the publication of updated metrics for each asset. These
/// numbers are propagated topologically, so a datacenter will have an alert
/// count equal to the tally of all the alerts inside it (plus itself if
/// applicable).
///
/// The agent can also resynchronize itself with the rest of the system. It will
/// enter a state where the agent ceases to publish metrics until the query of
/// all the alerts and all the assets present in the system is complete (or if
/// the operation times out at the next tick). It will then republish all its
/// metrics with fresh data and resume normal operation.
class AlertStatsActor : public mlm::MlmAgent, private FtyAlertStateHolder, private FtyAssetStateHolder
{
public:
    AlertStatsActor(zsock_t* pipe, const std::string& endpoint, const std::string& address, int64_t pollerTimeout, int64_t metricTTL);
    virtual ~AlertStatsActor() = default;

private:
    struct AlertCount
    {
        AlertCount()
            : critical(0)
            , warning(0)
            , lastSent(0)
        {
        }

        int     critical;
        int     warning;
        int64_t lastSent;

        AlertCount& operator+=(const AlertCount& ac)
        {
            critical += ac.critical;
            warning += ac.warning;
            lastSent = 0; // Invalidate lastSent
            return *this;
        }

        AlertCount& operator=(const AlertCount& ac)
        {
            critical = ac.critical;
            warning  = ac.warning;
            lastSent = ac.lastSent;
            return *this;
        }
    };

    typedef std::map<std::string, AlertCount> AlertCounts;

    virtual bool callbackAssetPre(fty_proto_t* asset) override;
    virtual void callbackAssetPost(fty_proto_t* asset) override;
    virtual bool callbackAlertPre(fty_proto_t* alert) override;

    void recomputeAlerts();
    bool recomputeAlert(fty_proto_t* alert, fty_proto_t* prevAlert);

    void sendMetric(AlertCounts::value_type& metric, bool recursive = true);
    void drainOutstandingAssetQueries();
    void startResynchronization();
    void resynchronizationProgress();

    bool isReady() const
    {
        return m_readyAssets && m_readyAlerts;
    }

    virtual bool tick() override;
    virtual bool handlePipe(zmsg_t* message) override;
    virtual bool handleStream(zmsg_t* message) override;
    virtual bool handleMailbox(zmsg_t* message) override;

    AlertCounts              m_alertCounts;
    std::vector<std::string> m_assetQueries;
    int                      m_outstandingAssetQueries{0};
    bool                     m_readyAssets{true};
    bool                     m_readyAlerts{true};
    int64_t                  m_lastResync{0}; // sec
    int64_t                  m_pollerTimeout{0}; // ms
    int64_t                  m_metricTTL{0}; // sec

public:
    constexpr static const char* WARNING_METRIC  = "alerts.active.warning";
    constexpr static const char* CRITICAL_METRIC = "alerts.active.critical";
};
