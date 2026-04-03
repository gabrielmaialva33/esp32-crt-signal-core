#include "crt_diag.h"

#include <string.h>

static crt_diag_snapshot_t s_snapshot;

void crt_diag_reset(void)
{
    memset(&s_snapshot, 0, sizeof(s_snapshot));
}

void crt_diag_get_snapshot(crt_diag_snapshot_t *out_snapshot)
{
    if (out_snapshot == NULL) {
        return;
    }

    *out_snapshot = s_snapshot;
}

void crt_diag_set_dma_underrun_count(uint32_t dma_underrun_count)
{
    s_snapshot.dma_underrun_count = dma_underrun_count;
}

void crt_diag_update_ready_queue_depth(uint32_t queue_depth)
{
    if (s_snapshot.ready_queue_min_depth == 0 || queue_depth < s_snapshot.ready_queue_min_depth) {
        s_snapshot.ready_queue_min_depth = queue_depth;
    }
}

void crt_diag_update_prep_cycles(uint32_t prep_cycles)
{
    if (prep_cycles > s_snapshot.prep_cycles_max) {
        s_snapshot.prep_cycles_max = prep_cycles;
    }
}
