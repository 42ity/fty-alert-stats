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

//  --------------------------------------------------------------------------
//  Actor function

void
fty_alert_stats_server(zsock_t *pipe, void* args)
{
    const char *endpoint = (const char *) args;

    try {
        AlertStatsActor alertStatsServer(pipe, endpoint);
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
                //fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::WARNING_METRIC, "rackcontroller-0", "1", ""),
                //fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::CRITICAL_METRIC, "rackcontroller-0", "0", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::WARNING_METRIC, "datacenter-3", "1", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::CRITICAL_METRIC, "datacenter-3", "0", "")
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
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::WARNING_METRIC, "row-5", "1", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::CRITICAL_METRIC, "row-5", "0", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::WARNING_METRIC, "room-4", "1", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::CRITICAL_METRIC, "room-4", "0", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::WARNING_METRIC, "datacenter-3", "2", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::CRITICAL_METRIC, "datacenter-3", "0", "")
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
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::WARNING_METRIC, "room-4", "1", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::CRITICAL_METRIC, "room-4", "1", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::WARNING_METRIC, "datacenter-3", "2", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::CRITICAL_METRIC, "datacenter-3", "1", "")
            },
            TestCase::Action::CHECK_METRICS
        },
        {
            "Clear WARNING alert1@rackcontroller-0",
            {},
            {
                fty_proto_encode_alert(nullptr, zclock_time()/1000, 60, "alert1@rackcontroller-0", "rackcontroller-0", "RESOLVED", "OK", "", nullptr)
            },
            {
                //fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::WARNING_METRIC, "rackcontroller-0", "0", ""),
                //fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::CRITICAL_METRIC, "rackcontroller-0", "0", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::WARNING_METRIC, "datacenter-3", "1", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::CRITICAL_METRIC, "datacenter-3", "1", "")
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
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::WARNING_METRIC, "row-5", "2", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::CRITICAL_METRIC, "row-5", "0", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::WARNING_METRIC, "room-4", "2", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::CRITICAL_METRIC, "room-4", "1", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::WARNING_METRIC, "datacenter-6", "2", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::CRITICAL_METRIC, "datacenter-6", "1", "")
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
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::WARNING_METRIC, "room-4", "3", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::CRITICAL_METRIC, "room-4", "0", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::WARNING_METRIC, "datacenter-6", "3", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::CRITICAL_METRIC, "datacenter-6", "0", "")
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
                //fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::WARNING_METRIC, "rackcontroller-0", "1", ""),
                //fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::CRITICAL_METRIC, "rackcontroller-0", "0", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::WARNING_METRIC, "datacenter-3", "1", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::CRITICAL_METRIC, "datacenter-3", "0", "")
            },
            TestCase::Action::CHECK_METRICS
        },
        {
            "Create rackcontroller-1 (with no known alerts)",
            {
                buildAssetMsg("rackcontroller-1", FTY_PROTO_ASSET_OP_CREATE, {{"status", "active"}, {FTY_PROTO_ASSET_AUX_PARENT_NAME_1, "datacenter-3"}}),
            },
            {},
            {
                //fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::WARNING_METRIC, "rackcontroller-1", "0", ""),
                //fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::CRITICAL_METRIC, "rackcontroller-1", "0", "")
            },
            TestCase::Action::CHECK_NO_METRICS
        },
        {
            "Publish WARNING alert1@rackcontroller-2 (unknown asset)",
            {},
            {
                fty_proto_encode_alert(nullptr, zclock_time()/1000, 60, "alert1@rackcontroller-2", "rackcontroller-2", "ACTIVE", "WARNING", "", nullptr)
            },
            {
                //fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::WARNING_METRIC, "rackcontroller-2", "1", ""),
                //fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::CRITICAL_METRIC, "rackcontroller-2", "0", ""),
            },
            TestCase::Action::CHECK_NO_METRICS
        },
        {
            "Create rackcontroller-2 (with known alerts)",
            {
                buildAssetMsg("rackcontroller-2", FTY_PROTO_ASSET_OP_CREATE, {{"status", "active"}, {FTY_PROTO_ASSET_AUX_PARENT_NAME_1, "datacenter-3"}}),
            },
            {},
            {
                //fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::WARNING_METRIC, "rackcontroller-2", "1", ""),
                //fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::CRITICAL_METRIC, "rackcontroller-2", "0", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::WARNING_METRIC, "datacenter-3", "2", ""),
                fty_proto_encode_metric(nullptr, 0, 0, AlertStatsActor::CRITICAL_METRIC, "datacenter-3", "0", "")
            },
            TestCase::Action::CHECK_METRICS
        },
    };

    //  @selftest
    const char* endpoint = "inproc://fty-alert-stats-server-test";

    const char *SELFTEST_DIR_RO = "src/selftest-ro";
    const char *SELFTEST_DIR_RW = "src/selftest-rw";
    assert (SELFTEST_DIR_RO);
    assert (SELFTEST_DIR_RW);
    
    fty_shm_set_test_dir(SELFTEST_DIR_RW);

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
            //currently only verify the number in shm. Must be improved.
            {
              fty::shm::shmMetrics testresult;
              fty::shm::read_metrics(FTY_SHM_METRIC_TYPE, ".*", ".*", testresult);
              assert(testCase.metrics.size() == testresult.size());
              fty_shm_delete_test_dir();
              fty_shm_set_test_dir(SELFTEST_DIR_RW);
            }
        }
        else if (testCase.action == TestCase::Action::CHECK_NO_METRICS) {
            // Check we don't receive metrics
            assert(zpoller_wait(metrics_poller, 1000) == nullptr);
            log_info(" * (No metrics received)");
            fty::shm::shmMetrics testresult;
            fty::shm::read_metrics(FTY_SHM_METRIC_TYPE, ".*", ".*", testresult);
            assert(testresult.size() == 0);
        }
        else if (testCase.action == TestCase::Action::PURGE_METRICS) {
            // Purge away metrics
            log_info(" * (Not checking metrics)");
            while (zpoller_wait(metrics_poller, 1000)) {
                zmsg_t *msg = mlm_client_recv(metrics_consumer);
                zmsg_destroy(&msg);
            }
            fty_shm_delete_test_dir();
            fty_shm_set_test_dir(SELFTEST_DIR_RW);
        }
    }

    zpoller_destroy(&metrics_poller);
    mlm_client_destroy(&metrics_consumer);
    mlm_client_destroy(&alerts_producer);
    mlm_client_destroy(&assets_producer);
    zactor_destroy(&alert_stats_server);
    zactor_destroy(&server);
    fty_shm_delete_test_dir();
    //  @end
    printf ("OK\n");
}
