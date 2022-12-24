/*  ========================================================================
    Copyright (C) 2020 Eaton
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
    ========================================================================
*/

#include "src/fty_alert_stats_actor.h"
#include "src/fty_alert_stats_server.h"
#include <catch2/catch.hpp>
#include <czmq.h>
#include <fty_proto.h>
#include <fty_shm.h>
#include <map>

namespace {

struct TestCase
{
    enum Action
    {
        PURGE_METRICS,
        CHECK_METRICS,
        CHECK_NO_METRICS
    };

    typedef std::vector<zmsg_t*> ProtoVector;

    TestCase(const char* n, const ProtoVector& as, const ProtoVector& al, const ProtoVector& me, Action ac)
        : name(n)
        , assets(as)
        , alerts(al)
        , metrics(me)
        , action(ac)
    {
    }

    const char* name;
    ProtoVector assets;
    ProtoVector alerts;
    ProtoVector metrics;
    Action      action;
};

typedef std::map<std::string, std::string> Properties;

static zmsg_t* buildAssetMsg(const char* name, const char* operation, const Properties& aux = {}, const Properties& ext = {})
{
    zhash_t* auxHash = nullptr;
    if (!aux.empty()) {
        auxHash = zhash_new();
        REQUIRE(auxHash);
        for (const auto& i : aux) {
            CHECK(zhash_insert(auxHash, i.first.c_str(), const_cast<char*>(i.second.c_str())) == 0);
        }
    }

    zhash_t* extHash = nullptr;
    if (!ext.empty()) {
        extHash = zhash_new();
        REQUIRE(extHash);
        for (const auto& i : ext) {
            CHECK(zhash_insert(extHash, i.first.c_str(), const_cast<char*>(i.second.c_str())) == 0);
        }
    }

    zmsg_t* msg = fty_proto_encode_asset(auxHash, name, operation, extHash);
    REQUIRE(msg);

    zhash_destroy(&auxHash);
    zhash_destroy(&extHash);

    return msg;
}

} // namespace

TEST_CASE("alert stats server test")
{
    std::vector<TestCase> testCases = {
        {
            "Set up assets",
            {buildAssetMsg("datacenter-3", FTY_PROTO_ASSET_OP_CREATE, {{"status", "active"}}),
                buildAssetMsg("rackcontroller-0", FTY_PROTO_ASSET_OP_CREATE,
                    {{"status", "active"}, {FTY_PROTO_ASSET_AUX_PARENT_NAME_1, "datacenter-3"}}),
                buildAssetMsg("room-4", FTY_PROTO_ASSET_OP_CREATE,
                    {{"status", "active"}, {FTY_PROTO_ASSET_AUX_PARENT_NAME_1, "datacenter-3"}}),
                buildAssetMsg("row-5", FTY_PROTO_ASSET_OP_CREATE,
                    {{"status", "active"}, {FTY_PROTO_ASSET_AUX_PARENT_NAME_1, "room-4"}}),
                buildAssetMsg("rack-6", FTY_PROTO_ASSET_OP_CREATE,
                    {{"status", "active"}, {FTY_PROTO_ASSET_AUX_PARENT_NAME_1, "row-5"}})},
            {},
            {},
            TestCase::Action::PURGE_METRICS // Not checking metrics now since we'll have a storm
        },
        {
            "Publish WARNING alert1@rackcontroller-0",
            {},
            {fty_proto_encode_alert(nullptr, uint64_t(zclock_time() / 1000), 60, "alert1@rackcontroller-0",
                "rackcontroller-0", "ACTIVE", "WARNING", "", nullptr)},
            {// fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::WARNING_METRIC, "rackcontroller-0", "1", ""),
             // fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::CRITICAL_METRIC, "rackcontroller-0", "0", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::WARNING_METRIC, "datacenter-3", "1", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::CRITICAL_METRIC, "datacenter-3", "0", "")},
            TestCase::Action::CHECK_METRICS
        },
        {
            "Publish WARNING alert2@row-5",
            {},
            {fty_proto_encode_alert(nullptr, uint64_t(zclock_time() / 1000), 60, "alert2@row-5", "row-5", "ACTIVE",
                "WARNING", "", nullptr)},
            {fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::WARNING_METRIC, "row-5", "1", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::CRITICAL_METRIC, "row-5", "0", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::WARNING_METRIC, "room-4", "1", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::CRITICAL_METRIC, "room-4", "0", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::WARNING_METRIC, "datacenter-3", "2", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::CRITICAL_METRIC, "datacenter-3", "0", "")},
            TestCase::Action::CHECK_METRICS
        },
        {
            "Publish CRITICAL alert3@room4",
            {},
            {fty_proto_encode_alert(nullptr, uint64_t(zclock_time() / 1000), 60, "alert3@room-4", "room-4", "ACTIVE",
                "CRITICAL", "", nullptr)},
            {fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::WARNING_METRIC, "room-4", "1", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::CRITICAL_METRIC, "room-4", "1", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::WARNING_METRIC, "datacenter-3", "2", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::CRITICAL_METRIC, "datacenter-3", "1", "")},
            TestCase::Action::CHECK_METRICS
        },
        {
            "Clear WARNING alert1@rackcontroller-0",
            {},
            {fty_proto_encode_alert(nullptr, uint64_t(zclock_time() / 1000), 60, "alert1@rackcontroller-0",
                "rackcontroller-0", "RESOLVED", "OK", "", nullptr)},
            {// fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::WARNING_METRIC, "rackcontroller-0", "0", ""),
             // fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::CRITICAL_METRIC, "rackcontroller-0", "0", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::WARNING_METRIC, "datacenter-3", "1", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::CRITICAL_METRIC, "datacenter-3", "1", "")},
            TestCase::Action::CHECK_METRICS
        },
        {
            "Publish ACK-SILENCE alert1@rackcontroller-0",
            {},
            {fty_proto_encode_alert(nullptr, uint64_t(zclock_time() / 1000), 60, "alert1@rackcontroller-0",
                "rackcontroller-0", "ACK-SILENCE", "WARNING", "", nullptr)},
            {},
            TestCase::Action::CHECK_NO_METRICS
        },
        {
            "Move room-4 to datacenter-6",
            {
                buildAssetMsg("room-4", FTY_PROTO_ASSET_OP_UPDATE,
                    {{"status", "active"}, {FTY_PROTO_ASSET_AUX_PARENT_NAME_1, "datacenter-6"}}),
            },
            {},
            {},
            TestCase::Action::PURGE_METRICS // Not checking metrics now since we'll have a storm
        },
        {
            "Publish WARNING alert4@row-5",
            {},
            {fty_proto_encode_alert(nullptr, uint64_t(zclock_time() / 1000), 60, "alert4@row-5", "row-5", "ACTIVE",
                "WARNING", "", nullptr)},
            {fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::WARNING_METRIC, "row-5", "2", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::CRITICAL_METRIC, "row-5", "0", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::WARNING_METRIC, "room-4", "2", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::CRITICAL_METRIC, "room-4", "1", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::WARNING_METRIC, "datacenter-6", "2", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::CRITICAL_METRIC, "datacenter-6", "1", "")},
            TestCase::Action::CHECK_METRICS
        },
        {
            "Demote alert3@room4 from CRITICAL to WARNING",
            {},
            {fty_proto_encode_alert(nullptr, uint64_t(zclock_time() / 1000), 60, "alert3@room-4", "room-4", "ACTIVE",
                "WARNING", "", nullptr)},
            {fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::WARNING_METRIC, "room-4", "3", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::CRITICAL_METRIC, "room-4", "0", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::WARNING_METRIC, "datacenter-6", "3", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::CRITICAL_METRIC, "datacenter-6", "0", "")},
            TestCase::Action::CHECK_METRICS
        },
        {
            "Update rackcontroller-0 without touching topology",
            {
                buildAssetMsg("rackcontroller-0", FTY_PROTO_ASSET_OP_UPDATE,
                    {{"status", "active"}, {FTY_PROTO_ASSET_AUX_PARENT_NAME_1, "datacenter-3"}, {"_bwaa", "bwaa"}}),
            },
            {},
            {},
            TestCase::Action::CHECK_NO_METRICS
        },
        {
            "Publish WARNING alert1@rackcontroller-0",
            {},
            {fty_proto_encode_alert(nullptr, uint64_t(zclock_time() / 1000), 60, "alert1@rackcontroller-0",
                "rackcontroller-0", "ACTIVE", "WARNING", "", nullptr)},
            {// fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::WARNING_METRIC, "rackcontroller-0", "1", ""),
             // fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::CRITICAL_METRIC, "rackcontroller-0", "0", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::WARNING_METRIC, "datacenter-3", "1", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::CRITICAL_METRIC, "datacenter-3", "0", "")},
            TestCase::Action::CHECK_METRICS
        },
        {
            "Create rackcontroller-1 (with no known alerts)",
            {
                buildAssetMsg("rackcontroller-1", FTY_PROTO_ASSET_OP_CREATE,
                    {{"status", "active"}, {FTY_PROTO_ASSET_AUX_PARENT_NAME_1, "datacenter-3"}}),
            },
            {},
            {
                // fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::WARNING_METRIC, "rackcontroller-1", "0", ""),
                // fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::CRITICAL_METRIC, "rackcontroller-1", "0", "")
            },
            TestCase::Action::CHECK_NO_METRICS
        },
        {
            "Publish WARNING alert1@rackcontroller-2 (unknown asset)",
            {},
            {fty_proto_encode_alert(nullptr, uint64_t(zclock_time() / 1000), 60, "alert1@rackcontroller-2",
                "rackcontroller-2", "ACTIVE", "WARNING", "", nullptr)},
            {
                // fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::WARNING_METRIC, "rackcontroller-2", "1", ""),
                // fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::CRITICAL_METRIC, "rackcontroller-2", "0",
                // ""),
            },
            TestCase::Action::CHECK_NO_METRICS
        },
        {
            "Create rackcontroller-2 (with known alerts)",
            {
                buildAssetMsg("rackcontroller-2", FTY_PROTO_ASSET_OP_CREATE,
                    {{"status", "active"}, {FTY_PROTO_ASSET_AUX_PARENT_NAME_1, "datacenter-3"}}),
            },
            {},
            {// fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::WARNING_METRIC, "rackcontroller-2", "1", ""),
             // fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::CRITICAL_METRIC, "rackcontroller-2", "0", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::WARNING_METRIC, "datacenter-3", "2", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::CRITICAL_METRIC, "datacenter-3", "0", "")},
            TestCase::Action::CHECK_METRICS
        },
    }; //testCases

    const char* endpoint = "inproc://fty-alert-stats-server-test";
    const char* actorAddress = "alert-stats-server-test";

    fty_shm_set_test_dir(".");

    //  Set up broker
    zactor_t* server = zactor_new(mlm_server, const_cast<char*>("Malamute"));
    REQUIRE(server);
    zstr_sendx(server, "BIND", endpoint, NULL);

    AlertStatsActorParams params;
    params.endpoint              = endpoint;
    params.address               = actorAddress;
    params.metricTTL             = 180;
    params.pollerTimeout         = 720 * 1000;
    zactor_t* alert_stats_server = zactor_new(fty_alert_stats_server, reinterpret_cast<void*>(&params));
    REQUIRE(alert_stats_server);

    //  Producer on FTY_PROTO_STREAM_ASSETS stream
    mlm_client_t* assets_producer = mlm_client_new();
    REQUIRE(assets_producer);
    REQUIRE(mlm_client_connect(assets_producer, endpoint, 1000, "assets_producer") == 0);
    REQUIRE(mlm_client_set_producer(assets_producer, FTY_PROTO_STREAM_ASSETS) == 0);

    //  Producer on FTY_PROTO_STREAM_ALERTS stream
    mlm_client_t* alerts_producer = mlm_client_new();
    REQUIRE(alerts_producer);
    REQUIRE(mlm_client_connect(alerts_producer, endpoint, 1000, "alerts_producer") == 0);
    REQUIRE(mlm_client_set_producer(alerts_producer, FTY_PROTO_STREAM_ALERTS) == 0);

    printf("== REPUBLISH request\n");
    {
        mlm_client_t* client = assets_producer;
        zpoller_t* poller = zpoller_new(mlm_client_msgpipe(client), NULL);
        REQUIRE(poller);

        zmsg_t* msg = zmsg_new();
        int r = mlm_client_sendto(client, actorAddress, "REPUBLISH", NULL, 1000, &msg);
        REQUIRE(r == 0);
        zmsg_destroy(&msg);

        if (zpoller_wait(poller, 5000))
            { msg = mlm_client_recv(client); }
        REQUIRE(msg);
        zmsg_print(msg);
        char* s = zmsg_popstr(msg);
        CHECK((s && (streq(s, "OK") || streq(s, "RESYNC"))));
        zstr_free(&s);
        zmsg_destroy(&msg);

        zpoller_destroy(&poller);
    }

    printf("== ALIVE request\n");
    {
        mlm_client_t* client = assets_producer;
        zpoller_t* poller = zpoller_new(mlm_client_msgpipe(client), NULL);
        REQUIRE(poller);

        zmsg_t* msg = zmsg_new();
        int r = mlm_client_sendto(client, actorAddress, "ALIVE", NULL, 1000, &msg);
        REQUIRE(r == 0);
        zmsg_destroy(&msg);

        if (zpoller_wait(poller, 5000))
            { msg = mlm_client_recv(client); }
        REQUIRE(msg);
        zmsg_print(msg);
        char* s = zmsg_popstr(msg);
        CHECK((s && streq(s, "OK")));
        zstr_free(&s);
        zmsg_destroy(&msg);

        zpoller_destroy(&poller);
    }

    //  Run test cases
    printf("== testCases\n");
    for (auto& testCase : testCases) {
        // Inject assets (destroyed)
        for (auto asset : testCase.assets) {
            REQUIRE(mlm_client_send(assets_producer, "asset", &asset) == 0);
        }
        // Inject alerts (destroyed)
        for (auto alert : testCase.alerts) {
            REQUIRE(mlm_client_send(alerts_producer, "alert", &alert) == 0);
        }

        if (testCase.action == TestCase::Action::CHECK_METRICS) {
            // Check metrics
            sleep(2); // sync
            for (auto refMsg : testCase.metrics) {
                fty_proto_t* refMetric = fty_proto_decode(&refMsg);
                REQUIRE(refMetric);

                fty_proto_t* recvMetric = nullptr;
                int r = fty::shm::read_metric(fty_proto_name(refMetric), fty_proto_type(refMetric), &recvMetric);
                REQUIRE(r == 0);
                REQUIRE(recvMetric);

                CHECK(fty_proto_id(recvMetric) == FTY_PROTO_METRIC);
                CHECK(streq(fty_proto_type(refMetric), fty_proto_type(recvMetric)));
                CHECK(streq(fty_proto_name(refMetric), fty_proto_name(recvMetric)));
                CHECK(streq(fty_proto_value(refMetric), fty_proto_value(recvMetric)));

                fty_proto_destroy(&refMetric);
                fty_proto_destroy(&recvMetric);
            }
            // Purge metrics
            fty_shm_delete_test_dir();
            fty_shm_set_test_dir(".");
        }
        else if (testCase.action == TestCase::Action::CHECK_NO_METRICS) {
            // Check we don't receive metrics
            fty::shm::shmMetrics metrics;
            fty::shm::read_metrics(".*", ".*", metrics);
            CHECK(metrics.size() == 0);
        }
        else if (testCase.action == TestCase::Action::PURGE_METRICS) {
            // Purge metrics
            fty_shm_delete_test_dir();
            fty_shm_set_test_dir(".");
        }
    }

    mlm_client_destroy(&alerts_producer);
    mlm_client_destroy(&assets_producer);
    zactor_destroy(&alert_stats_server);
    zactor_destroy(&server);

    fty_shm_delete_test_dir();
}
