cmake_minimum_required(VERSION 3.13)
cmake_policy(VERSION 3.13)

########################################################################################################################

etn_target(static ${PROJECT_NAME}-lib
    SOURCES
        src/fty_alert_stats_actor.cc
        src/fty_alert_stats_actor.h
        src/fty_alert_stats_server.cc
        src/fty_alert_stats_server.h
        src/fty_proto_stateholders.cc
        src/fty_proto_stateholders.h
    USES_PRIVATE
        czmq
        mlm
        fty_shm
        fty_proto
        fty_common_logging
        fty_common_mlm
    PRIVATE
)

########################################################################################################################

etn_test_target(${PROJECT_NAME}-lib
    SOURCES
        tests/main.cpp
        tests/alert_stats.cpp
    PREPROCESSOR
        -DCATCH_CONFIG_FAST_COMPILE
    SUBDIR
        tests
)

########################################################################################################################
