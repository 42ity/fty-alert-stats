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

/*
@header
    fty_alert_stats_server_class
@discuss
@end
*/

#include "fty_alert_stats_classes.h"

AlertStatsActor::AlertStatsActor(zsock_t *pipe, const char *endpoint, int64_t pollerTimeout, int64_t metricTTL)
    : MlmAgent(pipe, endpoint, "fty-alert-stats", pollerTimeout),
      m_alertCounts(),
      m_assetQueries(),
      m_outstandingAssetQueries(),
      m_readyAssets(true),
      m_readyAlerts(true),
      m_lastResync(0),
      m_metricTTL(metricTTL),
      m_pollerTimeout(pollerTimeout)
{
    if (mlm_client_set_consumer(client(), FTY_PROTO_STREAM_ASSETS, ".*") == -1) {
        log_error("mlm_client_set_consumer(stream = '%s', pattern = '%s') failed.", FTY_PROTO_STREAM_ASSETS, ".*");
        throw std::runtime_error("Can't set client consumer");
    }

    if (mlm_client_set_consumer(client(), FTY_PROTO_STREAM_ALERTS, ".*") == -1) {
        log_error("mlm_client_set_consumer(stream = '%s', pattern = '%s') failed.", FTY_PROTO_STREAM_ALERTS, ".*");
        throw std::runtime_error("Can't set client consumer");
    }

    if (mlm_client_set_producer(client(), FTY_PROTO_STREAM_METRICS) == -1) {
        log_error("mlm_client_set_producer(stream = '%s') failed.", FTY_PROTO_STREAM_METRICS);
        throw std::runtime_error("Can't set client producer");
    }
}

bool AlertStatsActor::callbackAssetPre(fty_proto_t *asset)
{
    const char *name = fty_proto_name(asset);
    const char *operation = fty_proto_operation(asset);

    // Filter things we are interested in
    if (streq(operation, FTY_PROTO_ASSET_OP_INVENTORY)) {
        return false;
    }
    else if (streq(operation, FTY_PROTO_ASSET_OP_UPDATE)) {
        if (m_assets.count(name)) {
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

void AlertStatsActor::callbackAssetPost(fty_proto_t *asset)
{
    /**
     * An asset has been modified, trigger recompute.
     */
    const char *name = fty_proto_name(asset);
    const char *operation = fty_proto_operation(asset);

    if (streq(operation, FTY_PROTO_ASSET_OP_CREATE)) {
        m_alertCounts[name] = AlertCount();
        bool mustRecurse = false;

        // Just update alerts attached to the asset.
        for (FtyProtoCollection::value_type &i : m_alerts) {
            if (streq(fty_proto_name(i.second.get()), name)) {
                recomputeAlert(i.second.get(), nullptr);
                mustRecurse = true;
            }
        }

        sendMetric(*(m_alertCounts.find(name)), mustRecurse);
    }
    else {
        /**
         * Since the topology itself of the assets has been altered when we get
         * here, we trigger a complete recompute of the alert tallies and
         * republication of the associated metrics. We could do a better job at
         * tracking what happened, but unless there's a huge datacenter that is
         * being thoroughly scrambled around at the speed of sound we shouldn't
         * cause a meltdown.
         */
        recomputeAlerts();
    }
}

bool AlertStatsActor::callbackAlertPre(fty_proto_t *alert)
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

void AlertStatsActor::recomputeAlerts()
{
    if (isReady()) {
        // Don't spam if we're not sending stats
        log_debug("Recomputing all statistics...");
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
        log_debug("Finished recomputing statistics, publishing all metrics...");
    }
    for (auto &i : m_alertCounts) {
        sendMetric(i, false);
    }

    if (isReady()) {
        log_info("All metrics published.");
    }
}

bool AlertStatsActor::recomputeAlert(fty_proto_t *alert, fty_proto_t *prevAlert)
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

        if (streq(prevSeverity, "CRITICAL")) {
            delta.critical = -1;
        }
        else if (streq(prevSeverity, "WARNING")) {
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
        if (delta.warning == 0 && delta.critical == 0) {
            log_error("Interesting alert but computed null delta!");
        }

        // Update alert count of asset and all parents
        const char *curAsset = fty_proto_name(alert);

        log_trace("alert=%s state=%s severity=%s prev_state=%s prev_severity=%s interesting.",
            fty_proto_rule(alert),
            state,
            severity,
            prevState ? prevState : "(null)",
            prevSeverity ? prevSeverity : "(null)"
        );

        while (curAsset) {
            log_trace("asset=%s update count (W %d; C %d) + (W %d; C %d) = (W %d; C %d).",
                curAsset,
                m_alertCounts[curAsset].warning,
                m_alertCounts[curAsset].critical,
                delta.warning,
                delta.critical,
                m_alertCounts[curAsset].warning+delta.warning,
                m_alertCounts[curAsset].critical+delta.critical
            );

            m_alertCounts[curAsset] += delta;
            auto it = m_assets.find(curAsset);
            curAsset = nullptr;

            if (it != m_assets.end()) {
                curAsset = fty_proto_aux_string(it->second.get(), FTY_PROTO_ASSET_AUX_PARENT_NAME_1, nullptr);
            }
        }
    }
    else {
        log_trace("alert=%s state=%s severity=%s prev_state=%s prev_severity=%s not interesting.",
            fty_proto_rule(alert),
            state,
            severity,
            prevState ? prevState : "(null)",
            prevSeverity ? prevSeverity : "(null)"
        );
    }

    return r;
}

void AlertStatsActor::sendMetric(AlertCounts::value_type &metric, bool recursive)
{
    if (!isReady()) {
        /**
         * Resynchronizing with the rest of the world. Inhibit sending metrics
         * until we are ready.
         */
        return;
    }

    // Inhibit metrics for simple devices or fty-outage malfunctions
    const auto& assetId = metric.first;

    if (assetId.find("datacenter-") == 0 || assetId.find("room-") == 0 || assetId.find("row-") == 0 || assetId.find("rack-") == 0) {
        metric.second.lastSent = zclock_time()/1000;

        fty::shm::write_metric(assetId,WARNING_METRIC,std::to_string(metric.second.warning),"", m_metricTTL);

        fty::shm::write_metric(assetId, CRITICAL_METRIC, std::to_string(metric.second.critical), "", m_metricTTL);
    }
    else {
        metric.second.lastSent = INT64_MAX/2;
    }

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

void AlertStatsActor::drainOutstandingAssetQueries()
{
    const int MAX_OUTSTANDING_QUERIES = 32;

    while ((m_outstandingAssetQueries < MAX_OUTSTANDING_QUERIES) && !m_assetQueries.empty()) {
        log_debug("Query details of asset %s...", m_assetQueries.back().c_str());

        zmsg_t *queryMsg = zmsg_new();
        zmsg_addstr(queryMsg, "GET");
        zmsg_addstr(queryMsg, "_ASSET_DETAIL_RESULT");
        zmsg_addstr(queryMsg, m_assetQueries.back().c_str());
        m_assetQueries.pop_back();

        if (mlm_client_sendto(client(), "asset-agent", "ASSET_DETAIL", nullptr, 5000, &queryMsg) == 0) {
            m_outstandingAssetQueries++;
        }
    }
}

void AlertStatsActor::startResynchronization()
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
     * answers, we force the flags back to true if the agent ticks while in this
     * state for too long.
     */

    log_info("Querying list of assets...");
    m_assets.clear();
    zmsg_t *msg = zmsg_new();
    zmsg_addstr(msg, "GET");
    zmsg_addstr(msg, "");
    mlm_client_sendto(client(), "asset-agent", "ASSETS_IN_CONTAINER", nullptr, 5000, &msg);

    log_info("Querying details of all alerts...");
    m_alerts.clear();
    msg = zmsg_new();
    zmsg_addstr(msg, "LIST");
    zmsg_addstr(msg, "ALL");
    mlm_client_sendto(client(), "fty-alert-list", "rfc-alerts-list", nullptr, 5000, &msg);

    // Disarm agent until we have our data
    m_readyAssets = false;
    m_readyAlerts = false;

    m_lastResync = zclock_mono()/1000;
}
void AlertStatsActor::resynchronizationProgress()
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

bool AlertStatsActor::tick()
{
    purgeExpiredAlerts();

    log_info("Agent is ticking.");

    /**
     * As a safety precaution, unwedge the agent if it's stuck resynchronizing
     * for at least one complete poller timespan.
     */
    if (!isReady() && (zclock_mono()/1000 > m_lastResync + m_pollerTimeout*2)) {
        log_info("Agent was stuck resynchronizing data when entering tick, unwedging it...");
        m_readyAssets = true;
        m_readyAlerts = true;

        resynchronizationProgress();
    }

    // Refresh all alerts
    int64_t curClock = zclock_time()/1000;
    for (auto &i : m_alertCounts) {
        if ((i.second.lastSent + m_metricTTL/2) <= curClock) {
            sendMetric(i, false);
        }
    }

    return true;
}

bool AlertStatsActor::handlePipe(zmsg_t *message)
{
    bool r = true;
    char *actor_command = zmsg_popstr(message);

    // $TERM actor command implementation is required by zactor_t interface
    if (streq(actor_command, "$TERM")) {
        r = false;
    }
    // Resynchronize ourselves with the rest of the world
    else if (streq(actor_command, "RESYNC")) {
        log_info("Agent is resynchronizing data...");
        startResynchronization();
    }
    else {
        log_error("Unexpected pipe message '%s'.", actor_command);
    }

    zstr_free(&actor_command);
    return r;
}

bool AlertStatsActor::handleMailbox(zmsg_t *message)
{
    const char *sender = mlm_client_sender(client());
    const char *subject = mlm_client_subject(client());
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

        mlm_client_sendto(client(), sender, "REPUBLISH", NULL, 5000, &reply);
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
                    log_debug("Injecting alert '%s' state %s severity %s.", fty_proto_rule(alertProto), fty_proto_state(alertProto), fty_proto_severity(alertProto));
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
                log_debug("Injecting asset '%s'.", fty_proto_name(assetProto));
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
    return true;
}

bool AlertStatsActor::handleStream(zmsg_t *message)
{
    // On malamute streams we should receive only fty_proto messages
    if (!is_fty_proto(message)) {
        log_error("Received message is not a fty_proto message.");
        return true;
    }

    zmsg_t *message_dup = zmsg_dup(message);
    fty_proto_t *protocol_message = fty_proto_decode(&message_dup);
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
