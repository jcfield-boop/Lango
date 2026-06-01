idf_component_register(
    SRCS
        "test_main.c"
        "test_downsample.c"
        "test_cron.c"
        "test_ws_messages.c"
    INCLUDE_DIRS
        "."
        "../../main"            # for audio/downsample.h
    REQUIRES
        unity
        cjson
)
