idf_component_register(SRCS "crt_compose.c"
        "crt_compose_layers.c"
        "crt_sprite.c"
                       INCLUDE_DIRS "include"
                       REQUIRES crt_core crt_timing)
