#include "crt_diag.h"

#include <assert.h>

static void test_reset_window_preserves_underruns(void)
{
    crt_diag_snapshot_t snapshot;

    crt_diag_reset();
    crt_diag_set_dma_underrun_count(7);
    crt_diag_update_ready_queue_depth(3);
    crt_diag_update_ready_queue_depth(5);
    crt_diag_update_prep_cycles(100);
    crt_diag_update_prep_cycles(80);

    crt_diag_reset_window();
    crt_diag_get_snapshot(&snapshot);

    assert(snapshot.dma_underrun_count == 7);
    assert(snapshot.ready_queue_min_depth == 0);
    assert(snapshot.prep_cycles_max == 0);

    crt_diag_update_ready_queue_depth(4);
    crt_diag_update_prep_cycles(90);
    crt_diag_get_snapshot(&snapshot);

    assert(snapshot.dma_underrun_count == 7);
    assert(snapshot.ready_queue_min_depth == 4);
    assert(snapshot.prep_cycles_max == 90);
}

int main(void)
{
    test_reset_window_preserves_underruns();
    return 0;
}
