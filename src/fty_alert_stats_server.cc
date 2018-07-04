/*  =========================================================================
    fty_alert_stats_server - Actor

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
    fty_alert_stats_server - Actor
@discuss
@end
*/

#include "fty_alert_stats_classes.h"

#include "ftyprotostateholders.h"
#include "ftyactor.h"

/**
 * \brief Agent for publishing aggregate metric statitics for alerts by asset.
 *
 * This agent is normally a purely reactive one, where received alerts (or
 * assets) will trigger the publication of updated metrics for each asset. These
 * numbers are propagated topologically, so a datacenter will have an alert
 * count equal to the tally of all the alerts inside it (plus itself if
 * applicable).
 *
 * The agent can also resynchronize itself with the rest of the system. It will
 * enter a state where the agent ceases to publish metrics until the query of
 * all the alerts and all the assets present in the system is complete (or if
 * the operation times out at the next tick). It will then republish all its
 * metrics with fresh data and resume normal operation.
 */
class AlertStatsServer : public FtyActor, private FtyAlertStateHolder, private FtyAssetStateHolder
{
public:
    AlertStatsServer(zsock_t *pipe, const char *endpoint, int64_t pollerTimeout = AlertCount::TTL/4 * 1000);
    virtual ~AlertStatsServer() = default;

    constexpr static const char *WARNING_METRIC = "alerts.warning";
    constexpr static const char *CRITICAL_METRIC = "alerts.critical";

private:
    struct AlertCount
    {
        AlertCount()
          : critical(0), warning(0), lastSent(0)
        {
        }

        int critical;
        int warning;
        int64_t lastSent;

        AlertCount& operator+=(const AlertCount& ac)
        {
            critical += ac.critical;
            warning += ac.warning;
            lastSent = 0; // Invalidate lastSent
            return *this;
        }

        const static int64_t TTL = 12 * 60;
    };

    typedef std::map<std::string, AlertCount> AlertCounts;

    constexpr static const int64_t RESYNC_INTERVAL = AlertCount::TTL * 60;

    virtual bool callbackAssetPre(fty_proto_t *asset) override;
    virtual void callbackAssetPost(fty_proto_t *asset) override;
    virtual bool callbackAlertPre(fty_proto_t *alert) override;

    void recomputeAlerts();
    bool recomputeAlert(fty_proto_t *alert, fty_proto_t *prevAlert);

    void sendMetric(AlertCounts::value_type &metric, bool recursive = true);
    void drainOutstandingAssetQueries();
    void startResynchronization();
    void resynchronizationProgress();

    bool isReady() const { return m_readyAssets && m_readyAlerts; }

    virtual bool tick() override;
    virtual bool handlePipe(zmsg_t *message) override;
    virtual bool handleStream(zmsg_t *message) override;
    virtual bool handleMailbox(zmsg_t *message) override;

    AlertCounts m_alertCounts;
    std::vector<std::string> m_assetQueries;
    int m_outstandingAssetQueries;
    bool m_readyAssets;
    bool m_readyAlerts;
    int64_t m_lastResync;
};

AlertStatsServer::AlertStatsServer(zsock_t *pipe, const char *endpoint, int64_t pollerTimeout)
    : FtyActor(pipe, endpoint, "fty-alert-stats", pollerTimeout),
      m_alertCounts(),
      m_assetQueries(),
      m_outstandingAssetQueries(),
      m_readyAssets(true),
      m_readyAlerts(true),
      m_lastResync(INT64_MAX - RESYNC_INTERVAL)
{
    if (mlm_client_set_consumer(m_client, FTY_PROTO_STREAM_ASSETS, ".*") == -1) {
        log_error("mlm_client_set_consumer(stream = '%s', pattern = '%s') failed.", FTY_PROTO_STREAM_ASSETS, ".*");
        throw std::runtime_error("Can't set client consumer");
    }

    if (mlm_client_set_consumer(m_client, FTY_PROTO_STREAM_ALERTS, ".*") == -1) {
        log_error("mlm_client_set_consumer(stream = '%s', pattern = '%s') failed.", FTY_PROTO_STREAM_ALERTS, ".*");
        throw std::runtime_error("Can't set client consumer");
    }

    if (mlm_client_set_producer(m_client, FTY_PROTO_STREAM_METRICS) == -1) {
        log_error("mlm_client_set_producer(stream = '%s') failed.", FTY_PROTO_STREAM_METRICS);
        throw std::runtime_error("Can't set client producer");
    } 
}

bool AlertStatsServer::callbackAssetPre(fty_proto_t *asset)
{
    const char *name = fty_proto_name(asset);
    const char *operation = fty_proto_operation(asset);

    // Filter things we are interested in
    if (streq(operation, FTY_PROTO_ASSET_OP_INVENTORY)) {
        return false;
    }
    else if (streq(operation, FTY_PROTO_ASSET_OP_UPDATE))
    {
        if (m_assets.count(name))
        {
            fty_proto_t *oldAsset = m_assets[name].get();

            // We only care about topology, ignore update if the asset has not been reparented
            const char *parent = fty_proto_aux_string(asset, FTY_PROTO_ASSET_AUX_PARENT_NAME_1, "_no_parent_for_asset");
            const char *oldParent = fty_proto_aux_string(oldAsset, FTY_PROTO_ASSET_AUX_PARENT_NAME_1, "_no_parent_for_asset");

            if (streq(oldParent, parent)) {
                return false;
            }
        }
    }

    return true;
}

void AlertStatsServer::callbackAssetPost(fty_proto_t *asset)
{
    /**
     * An asset has been modified, trigger recompute.
     *
     * Since the topology itself of the assets has been altered when we get
     * called, we trigger a complete recompute of the alert tallies and
     * republication of the associated metrics. We could do a better job at
     * tracking what happened, but unless there's a huge datacenter that is
     * being thoroughly scrambled around at the speed of sound we shouldn't
     * cause a meltdown.
     */
    recomputeAlerts();
}

bool AlertStatsServer::callbackAlertPre(fty_proto_t *alert)
{
    // Do inline update
    const char *rule = fty_proto_rule(alert);

    fty_proto_t *prevAlert = nullptr;
    if (m_alerts.count(rule)) {
        prevAlert = m_alerts[rule].get();
    }

    if (recomputeAlert(alert, prevAlert)) {
        auto it = m_alertCounts.find(fty_proto_name(alert));
        if (it != m_alertCounts.end()) {
            sendMetric(*it);
        }
    }

    return true;
}

void AlertStatsServer::recomputeAlerts()
{
    if (isReady()) {
        // Don't spam if we're not sending stats
        log_trace("Recomputing all statistics...");
    }

    // Recompute and resend/refresh metrics with our current data
    m_alertCounts.clear();

    for (FtyProtoCollection::value_type &i : m_assets) {
        m_alertCounts.emplace(i.first, AlertCount());
    }
    for (FtyProtoCollection::value_type &i : m_alerts) {
        recomputeAlert(i.second.get(), nullptr);
    }
    if (isReady()) {
        log_trace("Finished recomputing statistics, publishing all metrics...");
    }
    for (auto &i : m_alertCounts) {
        sendMetric(i, false);
    }

    if (isReady()) {
        log_info("All metrics published.");
    }
}

bool AlertStatsServer::recomputeAlert(fty_proto_t *alert, fty_proto_t *prevAlert)
{
    bool r = false;
    AlertCount delta;
    const char *state = fty_proto_state(alert);
    const char *severity = fty_proto_severity(alert);
    const char *prevSeverity = nullptr;
    const char *prevState = nullptr;

    if (prevAlert) {
        prevSeverity = fty_proto_severity(prevAlert);
        prevState = fty_proto_state(prevAlert);
    }

    // Filter state transitions we're interested in

    // New alert with ACTIVE state
    if ((!prevAlert && streq(state, "ACTIVE")) ||
        (prevAlert && !streq(prevState, "ACTIVE") && streq(state, "ACTIVE"))) {
        r = true;

        if (streq(severity, "CRITICAL")) {
            delta.critical = 1;
        }
        else if (streq(severity, "WARNING")) {
            delta.warning = 1;
        }
    }
    // Known ACTIVE alert switching away from ACTIVE state
    else if (prevAlert && streq(prevState, "ACTIVE") && !streq(state, "ACTIVE")) {
        r = true;

        if (streq(severity, "CRITICAL")) {
            delta.critical = -1;
        }
        else if (streq(severity, "WARNING")) {
            delta.warning = -1;
        }
    }
    // Known ACTIVE alert changing severity
    else if (prevAlert && streq(state, "ACTIVE") && streq(prevState, "ACTIVE") && !streq(severity, prevSeverity)) {
        r = true;

        if (streq(prevSeverity, "CRITICAL")) {
            delta.critical = -1;
        }
        else if (streq(prevSeverity, "WARNING")) {
            delta.warning = -1;
        }

        if (streq(severity, "CRITICAL")) {
            delta.critical += 1;
        }
        else if (streq(severity, "WARNING")) {
            delta.warning += 1;
        }
    }

    if (r) {
        // Update alert count of asset and all parents
        const char *curAsset = fty_proto_name(alert);
        while (curAsset) {
            m_alertCounts[curAsset] += delta;
            auto it = m_assets.find(curAsset);
            curAsset = nullptr;

            if (it != m_assets.end()) {
                curAsset = fty_proto_aux_string(it->second.get(), FTY_PROTO_ASSET_AUX_PARENT_NAME_1, nullptr);
            }
        }
    }

    return r;
}

void AlertStatsServer::sendMetric(AlertCounts::value_type &metric, bool recursive)
{
    if (!isReady()) {
        /**
         * Resynchronizing with the rest of the world. Inhibit sending metrics
         * until we are ready.
         */
        return;
    }

    metric.second.lastSent = zclock_time()/1000;

    zmsg_t *msg;

    msg = fty_proto_encode_metric(
        nullptr,
        metric.second.lastSent,
        AlertCount::TTL,
        WARNING_METRIC,
        metric.first.c_str(),
        std::to_string(metric.second.warning).c_str(),
        "");

    mlm_client_send(m_client, (std::string(WARNING_METRIC)+"@"+metric.first).c_str(), &msg);

    msg = fty_proto_encode_metric(
        nullptr,
        metric.second.lastSent,
        AlertCount::TTL,
        CRITICAL_METRIC,
        metric.first.c_str(),
        std::to_string(metric.second.critical).c_str(),
        "");

    mlm_client_send(m_client, (std::string(CRITICAL_METRIC)+"@"+metric.first).c_str(), &msg);

    if (recursive) {
        // Recursively send metric of parent
        auto it = m_assets.find(metric.first);

        if (it != m_assets.end()) {
            const char *parent = fty_proto_aux_string(it->second.get(), FTY_PROTO_ASSET_AUX_PARENT_NAME_1, nullptr);

            if (parent) {
                auto itParent = m_alertCounts.find(parent);

                if (itParent != m_alertCounts.end()) {
                    sendMetric(*itParent, true);
                }
            }
        }
    }
}

void AlertStatsServer::drainOutstandingAssetQueries()
{
    const int MAX_OUTSTANDING_QUERIES = 32;

    while ((m_outstandingAssetQueries < MAX_OUTSTANDING_QUERIES) && !m_assetQueries.empty()) {
        log_trace("Query details of asset %s...", m_assetQueries.back().c_str());

        zmsg_t *queryMsg = zmsg_new();
        zmsg_addstr(queryMsg, "GET");
        zmsg_addstr(queryMsg, "_ASSET_DETAIL_RESULT");
        zmsg_addstr(queryMsg, m_assetQueries.back().c_str());
        m_assetQueries.pop_back();

        if (mlm_client_sendto(m_client, "asset-agent", "ASSET_DETAIL", nullptr, m_connectionTimeout, &queryMsg) == 0) {
            m_outstandingAssetQueries++;
        }
    }
}

void AlertStatsServer::startResynchronization()
{
    /**
     * Stop sending metrics, resync all our data, recompute statistics and
     * resume sending metrics.
     *
     * This message tells us to resynchronize ourselves with the rest of the
     * world. We set both m_readyAssets and m_readyAlerts to false and query
     * all:
     *  - outstanding alarms,
     *  - known assets.
     *
     * Data will be collected through the mailbox and the flags will be
     * reset on completion of subtasks. To prevent deadlocking on lost
     * answers, we force the flags back to true if the agent ticks.
     *
     * Note that this will enable periodic data resynchronization too. In self-
     * test mode this method will not ever get called (so as to test only tally
     * keeping), but in real-life we trigger this method on start-up.
     */
    log_info("Agent is resynchronizing data...");

    log_info("Querying list of assets...");
    m_assets.clear();
    zmsg_t *msg = zmsg_new();
    zmsg_addstr(msg, "GET");
    zmsg_addstr(msg, "");
    mlm_client_sendto(m_client, "asset-agent", "ASSETS_IN_CONTAINER", nullptr, m_connectionTimeout, &msg);

    log_info("Querying details of all alerts...");
    m_alerts.clear();
    msg = zmsg_new();
    zmsg_addstr(msg, "LIST");
    zmsg_addstr(msg, "ALL");
    mlm_client_sendto(m_client, "fty-alert-list", "rfc-alerts-list", nullptr, m_connectionTimeout, &msg);

    // Disarm agent until we have our data
    m_readyAssets = false;
    m_readyAlerts = false;

    m_lastResync = zclock_mono()/1000;
}
void AlertStatsServer::resynchronizationProgress()
{
    /**
     * We enter this method everytime we've made progress on resynchronizing
     * with the rest of the world. If we're done, publish all our metrics.
     */
    if (isReady()) {
        log_info("Agent is done resynchronizing data.");
        recomputeAlerts();
    }
}

bool AlertStatsServer::tick()
{
    purgeExpiredAlerts();

    log_info("Agent is ticking.");

    /**
     * As a safety precaution, unwedge the agent if it's stuck resynchronizing
     * when we enter here.
     */
    if (!isReady()) {
        log_info("Agent was stuck resynchronizing data when entering tick, unwedging it.");
        m_readyAssets = true;
        m_readyAlerts = true;

        resynchronizationProgress();
    }

    // Refresh all alerts
    int64_t curClock = zclock_time()/1000;
    for (auto &i : m_alertCounts) {
        if ((i.second.lastSent + AlertCount::TTL/2) <= curClock) {
            sendMetric(i, false);
        }
    }

    // Resynchronize data periodically
    if (m_lastResync + RESYNC_INTERVAL <= zclock_mono()/1000) {
        startResynchronization();
    }

    return true;
}

bool AlertStatsServer::handlePipe(zmsg_t *message)
{
    bool r = true;
    char *actor_command = zmsg_popstr(message);

    // $TERM actor command implementation is required by zactor_t interface
    if (streq(actor_command, "$TERM")) {
        r = false;
    }
    // Requery state (and enable periodic data resynchronization)
    else if (streq(actor_command, "_QUERY_STUFF")) {
        startResynchronization();
    }
    else {
        log_error("Unexpected pipe message '%s'.", actor_command);
    }

    zstr_free(&actor_command);
    zmsg_destroy(&message);
    return r;
}

bool AlertStatsServer::handleMailbox(zmsg_t *message)
{
    const char *sender = mlm_client_sender(m_client);
    const char *subject = mlm_client_subject(m_client);
    char *actor_command = nullptr;

    // Resend all metrics
    if (streq(subject, "REPUBLISH")) {
        log_info("Republish query from '%s'.", sender);
        zmsg_t *reply = zmsg_new();

        if (isReady()) {
            recomputeAlerts();
            zmsg_addstr (reply, "OK");
        }
        else {
            zmsg_addstr (reply, "RESYNC");
        }

        mlm_client_sendto(m_client, sender, "REPUBLISH", NULL, 5000, &reply);
        zmsg_destroy(&reply);
    }
    // Result of rfc-alerts-list query to fty-alert-list
    else if (streq(sender, "fty-alert-list") && streq(subject, "rfc-alerts-list")) {
        // Pop return code
        actor_command = zmsg_popstr(message);

        if (actor_command && streq(actor_command, "LIST")) {
            // Pop message frame 'state'
            zstr_free(&actor_command);
            actor_command = zmsg_popstr(message);

            while (zmsg_size(message)) {
                // Inject each alarm into ourselves
                zmsg_t *alertMsg = zmsg_popmsg(message);
                fty_proto_t *alertProto = fty_proto_decode(&alertMsg);
                if (alertProto) {
                    log_trace("Injecting alert '%s'.", fty_proto_rule(alertProto));
                    processAlert(alertProto);
                }
                else {
                    log_error("Couldn't decode alert fty_proto_t message.");
                }
            }

            log_info("Finished resync of all alerts.");
            m_readyAlerts = true;

            resynchronizationProgress();
        }
    }
    // Result of ASSETS_IN_CONTAINER query to asset-agent
    else if (streq(sender, "asset-agent") && streq(subject, "ASSETS_IN_CONTAINER")) {
        // Pop UUID
        actor_command = zmsg_popstr(message);

        if (actor_command && streq(actor_command, "OK")) {
            /**
             * We have a list of asset names, but we need to query each asset
             * details in order to get the topology. Queue the queries to perform.
             */
            m_outstandingAssetQueries = 0;
            while (zmsg_size(message)) {
                m_assetQueries.emplace_back(zmsg_popstr(message));
            }

            log_info("Received list of %d asset names, querying asset details...", m_assetQueries.size());
            drainOutstandingAssetQueries();
        }
    }
    // Result of ASSET_DETAIL query to asset-agent
    else if (streq(sender, "asset-agent")) {
        // Pop UUID
        actor_command = zmsg_popstr(message);

        if (actor_command && streq(actor_command, "_ASSET_DETAIL_RESULT")) {
            // Inject asset into ourselves
            zmsg_t *assetMsg = zmsg_dup(message);
            fty_proto_t *assetProto = fty_proto_decode(&assetMsg);
            if (assetProto) {
                log_trace("Injecting asset '%s'.", fty_proto_name(assetProto));
                processAsset(assetProto);
            }
            else {
                log_error("Couldn't decode asset fty_proto_t message.");
            }

            --m_outstandingAssetQueries;
            drainOutstandingAssetQueries();

            if (m_outstandingAssetQueries == 0) {
                log_info("Finished resync of all assets.");
                m_readyAssets = true;
            }

            resynchronizationProgress();
        }
        else {
            log_error("Unexpected mailbox message '%s' from '%s'.", subject, sender);
        }
    }
    else {
        log_error("Unexpected mailbox message '%s' from '%s'.", subject, sender);
    }

    zstr_free(&actor_command);
    zmsg_destroy(&message);
    return true;
}

bool AlertStatsServer::handleStream(zmsg_t *message)
{
    // On malamute streams we should receive only fty_proto messages
    if (!is_fty_proto(message)) {
        log_error("Received message is not a fty_proto message.");
        zmsg_destroy(&message);
        return true;
    }

    fty_proto_t *protocol_message = fty_proto_decode(&message);
    if (protocol_message == NULL) {
        log_error("fty_proto_decode() failed, received message could not be parsed.");
        return true;
    }

    if (fty_proto_id(protocol_message) == FTY_PROTO_ASSET) {
        processAsset(protocol_message);
    }
    else if (fty_proto_id(protocol_message) == FTY_PROTO_ALERT) {
        processAlert(protocol_message);
    }
    else {
        log_error("Unexpected fty_proto message.");
        fty_proto_destroy(&protocol_message);
    }

    return true;
}

//  --------------------------------------------------------------------------
//  Actor function

void
fty_alert_stats_server(zsock_t *pipe, void* args)
{
    const char *endpoint = (const char *) args;

    try {
        AlertStatsServer alertStatsServer(pipe, endpoint);
        alertStatsServer.mainloop();
    }
    catch (std::runtime_error &e) {
        log_error("std::runtime_error exception caught, aborting actor (most likely died while initializing).");
    }
    catch (...) {
        log_error("Unexpected exception caught, aborting actor.");
    }
}

//  --------------------------------------------------------------------------
//  Self test of this class

namespace
{

struct TestCase
{
    enum Action {
        PURGE_METRICS,
        CHECK_METRICS,
        CHECK_NO_METRICS
    };

    typedef std::vector<zmsg_t*> ProtoVector;

    TestCase(const char *n, const ProtoVector &as, const ProtoVector &al, const ProtoVector &me, Action ac)
      : name(n), assets(as), alerts(al), metrics(me), action(ac)
    {
    }

    const char *name;
    ProtoVector assets;
    ProtoVector alerts;
    ProtoVector metrics;
    Action action;
};

typedef std::map<std::string, std::string> Properties;

zmsg_t* buildAssetMsg(const char *name, const char *operation, const Properties &aux = {}, const Properties &ext = {})
{
    zhash_t *auxHash = zhash_new();
    zhash_t *extHash = zhash_new();
    assert(auxHash);
    assert(extHash);

    for (const auto &i : aux) {
        assert(zhash_insert(auxHash, i.first.c_str(), (void*)i.second.c_str()) == 0);
    }
    for (const auto &i : ext) {
        assert(zhash_insert(extHash, i.first.c_str(), (void*)i.second.c_str()) == 0);
    }

    zmsg_t *msg = fty_proto_encode_asset(auxHash, name, operation, extHash);
    assert(msg);

    zhash_destroy (&auxHash);
    zhash_destroy (&extHash);

    return msg;
}

}

void
fty_alert_stats_server_test (bool verbose)
{
    ftylog_setInstance("fty-alert-stats","./src/selftest-ro/fty-alert-stats-log.conf");

    std::vector<TestCase> testCases =
    {
        {
            "Set up assets",
            {
                buildAssetMsg("datacenter-3", FTY_PROTO_ASSET_OP_CREATE, {{"status", "active"}}),
                buildAssetMsg("rackcontroller-0", FTY_PROTO_ASSET_OP_CREATE, {{"status", "active"}, {FTY_PROTO_ASSET_AUX_PARENT_NAME_1, "datacenter-3"}}),
                buildAssetMsg("room-4", FTY_PROTO_ASSET_OP_CREATE, {{"status", "active"}, {FTY_PROTO_ASSET_AUX_PARENT_NAME_1, "datacenter-3"}}),
                buildAssetMsg("row-5", FTY_PROTO_ASSET_OP_CREATE, {{"status", "active"}, {FTY_PROTO_ASSET_AUX_PARENT_NAME_1, "room-4"}}),
                buildAssetMsg("rack-6", FTY_PROTO_ASSET_OP_CREATE, {{"status", "active"}, {FTY_PROTO_ASSET_AUX_PARENT_NAME_1, "row-5"}})
            },
            {},
            {},
            TestCase::Action::PURGE_METRICS  // Not checking metrics now since we'll have a storm
        },
        {
            "Publish WARNING alert1@rackcontroller-0",
            {},
            {
                fty_proto_encode_alert(nullptr, zclock_time()/1000, 60, "alert1@rackcontroller-0", "rackcontroller-0", "ACTIVE", "WARNING", "", nullptr)
            },
            {
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsServer::WARNING_METRIC, "rackcontroller-0", "1", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsServer::CRITICAL_METRIC, "rackcontroller-0", "0", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsServer::WARNING_METRIC, "datacenter-3", "1", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsServer::CRITICAL_METRIC, "datacenter-3", "0", "")
            },
            TestCase::Action::CHECK_METRICS
        },
        {
            "Publish WARNING alert2@row-5",
            {},
            {
                fty_proto_encode_alert(nullptr, zclock_time()/1000, 60, "alert2@row-5", "row-5", "ACTIVE", "WARNING", "", nullptr)
            },
            {
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsServer::WARNING_METRIC, "row-5", "1", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsServer::CRITICAL_METRIC, "row-5", "0", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsServer::WARNING_METRIC, "room-4", "1", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsServer::CRITICAL_METRIC, "room-4", "0", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsServer::WARNING_METRIC, "datacenter-3", "2", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsServer::CRITICAL_METRIC, "datacenter-3", "0", "")
            },
            TestCase::Action::CHECK_METRICS
        },
        {
            "Publish CRITICAL alert3@room4",
            {},
            {
                fty_proto_encode_alert(nullptr, zclock_time()/1000, 60, "alert3@room-4", "room-4", "ACTIVE", "CRITICAL", "", nullptr)
            },
            {
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsServer::WARNING_METRIC, "room-4", "1", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsServer::CRITICAL_METRIC, "room-4", "1", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsServer::WARNING_METRIC, "datacenter-3", "2", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsServer::CRITICAL_METRIC, "datacenter-3", "1", "")
            },
            TestCase::Action::CHECK_METRICS
        },
        {
            "Clear WARNING alert1@rackcontroller-0",
            {},
            {
                fty_proto_encode_alert(nullptr, zclock_time()/1000, 60, "alert1@rackcontroller-0", "rackcontroller-0", "RESOLVED", "WARNING", "", nullptr)
            },
            {
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsServer::WARNING_METRIC, "rackcontroller-0", "0", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsServer::CRITICAL_METRIC, "rackcontroller-0", "0", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsServer::WARNING_METRIC, "datacenter-3", "1", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsServer::CRITICAL_METRIC, "datacenter-3", "1", "")
            },
            TestCase::Action::CHECK_METRICS
        },
        {
            "Publish ACK-SILENCE alert1@rackcontroller-0",
            {},
            {
                fty_proto_encode_alert(nullptr, zclock_time()/1000, 60, "alert1@rackcontroller-0", "rackcontroller-0", "ACK-SILENCE", "WARNING", "", nullptr)
            },
            {},
            TestCase::Action::CHECK_NO_METRICS
        },
        {
            "Move room-4 to datacenter-6",
            {
                buildAssetMsg("room-4", FTY_PROTO_ASSET_OP_UPDATE, {{"status", "active"}, {FTY_PROTO_ASSET_AUX_PARENT_NAME_1, "datacenter-6"}}),
            },
            {},
            {},
            TestCase::Action::PURGE_METRICS  // Not checking metrics now since we'll have a storm
        },
        {
            "Publish WARNING alert4@row-5",
            {},
            {
                fty_proto_encode_alert(nullptr, zclock_time()/1000, 60, "alert4@row-5", "row-5", "ACTIVE", "WARNING", "", nullptr)
            },
            {
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsServer::WARNING_METRIC, "row-5", "2", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsServer::CRITICAL_METRIC, "row-5", "0", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsServer::WARNING_METRIC, "room-4", "2", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsServer::CRITICAL_METRIC, "room-4", "1", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsServer::WARNING_METRIC, "datacenter-6", "2", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsServer::CRITICAL_METRIC, "datacenter-6", "1", "")
            },
            TestCase::Action::CHECK_METRICS
        },
        {
            "Demote alert3@room4 from CRITICAL to WARNING",
            {},
            {
                fty_proto_encode_alert(nullptr, zclock_time()/1000, 60, "alert3@room-4", "room-4", "ACTIVE", "WARNING", "", nullptr)
            },
            {
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsServer::WARNING_METRIC, "room-4", "3", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsServer::CRITICAL_METRIC, "room-4", "0", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsServer::WARNING_METRIC, "datacenter-6", "3", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsServer::CRITICAL_METRIC, "datacenter-6", "0", "")
            },
            TestCase::Action::CHECK_METRICS
        },
        {
            "Update rackcontroller-0 without touching topology",
            {
                buildAssetMsg("rackcontroller-0", FTY_PROTO_ASSET_OP_UPDATE, {{"status", "active"}, {FTY_PROTO_ASSET_AUX_PARENT_NAME_1, "datacenter-3"}, {"_bwaa", "bwaa"}}),
            },
            {},
            {},
            TestCase::Action::CHECK_NO_METRICS
        },
        {
            "Publish WARNING alert1@rackcontroller-0",
            {},
            {
                fty_proto_encode_alert(nullptr, zclock_time()/1000, 60, "alert1@rackcontroller-0", "rackcontroller-0", "ACTIVE", "WARNING", "", nullptr)
            },
            {
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsServer::WARNING_METRIC, "rackcontroller-0", "1", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsServer::CRITICAL_METRIC, "rackcontroller-0", "0", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsServer::WARNING_METRIC, "datacenter-3", "1", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsServer::CRITICAL_METRIC, "datacenter-3", "0", "")
            },
            TestCase::Action::CHECK_METRICS
        }
    };

    //  @selftest
    const char* endpoint = "inproc://fty-alert-stats-server-test";

    const char *SELFTEST_DIR_RO = "src/selftest-ro";
    const char *SELFTEST_DIR_RW = "src/selftest-rw";
    assert (SELFTEST_DIR_RO);
    assert (SELFTEST_DIR_RW);

    //  Set up broker
    zactor_t *server = zactor_new (mlm_server, (void*)"Malamute");
    zstr_sendx (server, "BIND", endpoint, NULL);
    if (verbose)
    {
        ftylog_setVeboseMode(ftylog_getInstance());
        zstr_send (server, "VERBOSE");
    }
    printf(" * fty_alert_stats_server: ");
    zactor_t *alert_stats_server = zactor_new(fty_alert_stats_server, (void *) endpoint);
    log_info("After launch fty_alert_stats_server");

    //  Producer on FTY_PROTO_STREAM_ASSETS stream
    mlm_client_t *assets_producer = mlm_client_new();
    assert(mlm_client_connect(assets_producer, endpoint, 1000, "assets_producer") == 0);
    assert(mlm_client_set_producer(assets_producer, FTY_PROTO_STREAM_ASSETS) == 0);

    //  Producer on FTY_PROTO_STREAM_ALERTS stream
    mlm_client_t *alerts_producer = mlm_client_new();
    assert(mlm_client_connect(alerts_producer, endpoint, 1000, "alerts_producer") == 0);
    assert(mlm_client_set_producer(alerts_producer, FTY_PROTO_STREAM_ALERTS) == 0);

    //  Consumer on FTY_PROTO_STREAM_METRICS stream
    mlm_client_t *metrics_consumer = mlm_client_new();
    assert(mlm_client_connect(metrics_consumer, endpoint, 1000, "metrics_consumer") == 0);
    assert(mlm_client_set_consumer(metrics_consumer, FTY_PROTO_STREAM_METRICS, ".*") == 0);
    zpoller_t *metrics_poller = zpoller_new(mlm_client_msgpipe(metrics_consumer), nullptr);

    //  Run test cases
    for (auto &testCase : testCases) {
        log_info("%s", testCase.name);

        // Inject assets
        for (auto asset : testCase.assets) {
            assert(mlm_client_send(assets_producer, "asset", &asset) == 0);
        }
        // Inject alerts
        for (auto alert : testCase.alerts) {
            assert(mlm_client_send(alerts_producer, "alert", &alert) == 0);
        }

        if (testCase.action == TestCase::Action::CHECK_METRICS) {
            // Check metrics
            for (auto refMsg : testCase.metrics) {
                fty_proto_t *refMetric = fty_proto_decode(&refMsg);

                assert(zpoller_wait(metrics_poller, 1000));
                zmsg_t *recvMsg = mlm_client_recv(metrics_consumer);
                fty_proto_t *recvMetric = fty_proto_decode(&recvMsg);
                assert(recvMetric);

                assert(fty_proto_id(recvMetric) == FTY_PROTO_METRIC);
                log_info(" * Received metric type@name=%s@%s, value=%s", fty_proto_type(recvMetric), fty_proto_name(recvMetric), fty_proto_value(recvMetric));

                assert(streq(fty_proto_type(refMetric), fty_proto_type(recvMetric)) &&
                       streq(fty_proto_name(refMetric), fty_proto_name(recvMetric)) &&
                       streq(fty_proto_value(refMetric), fty_proto_value(recvMetric)));

                fty_proto_destroy(&refMetric);
                fty_proto_destroy(&recvMetric);
            }
        }
        else if (testCase.action == TestCase::Action::CHECK_NO_METRICS) {
            // Check we don't receive metrics
            assert(zpoller_wait(metrics_poller, 1000) == nullptr);
            log_info(" * (No metrics received)");
        }
        else if (testCase.action == TestCase::Action::PURGE_METRICS) {
            // Purge away metrics
            log_info(" * (Not checking metrics)");
            while (zpoller_wait(metrics_poller, 1000)) {
                zmsg_t *msg = mlm_client_recv(metrics_consumer);
                zmsg_destroy(&msg);
            }
        }
    }

    zpoller_destroy(&metrics_poller);
    mlm_client_destroy(&metrics_consumer);
    mlm_client_destroy(&alerts_producer);
    mlm_client_destroy(&assets_producer);
    zactor_destroy(&alert_stats_server);
    zactor_destroy(&server);
    //  @end
    printf ("OK\n");
}
