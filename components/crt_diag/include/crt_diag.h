#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t dma_underrun_count;
    uint32_t ready_queue_min_depth;
    uint32_t prep_cycles_max;
} crt_diag_snapshot_t;

void crt_diag_reset(void);
void crt_diag_get_snapshot(crt_diag_snapshot_t *out_snapshot);
void crt_diag_set_dma_underrun_count(uint32_t dma_underrun_count);
void crt_diag_update_ready_queue_depth(uint32_t queue_depth);
void crt_diag_update_prep_cycles(uint32_t prep_cycles);

#ifdef __cplusplus
}
#endif
