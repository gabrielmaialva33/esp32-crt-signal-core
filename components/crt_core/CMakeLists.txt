idf_component_register(SRCS "crt_core.c" "crt_waveform.c" "crt_line_policy.c"
                       INCLUDE_DIRS "include"
                       REQUIRES crt_demo crt_diag crt_hal crt_timing)
