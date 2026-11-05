/* SPDX-License-Identifier: Apache-2.0
 * Entry of elfbench.elf. argv[0] points at an elfbench_arg_t.
 */
#include "bench_kernel.h"

__attribute__((visibility("default"))) int app_main(int argc, char *argv[])
{
    if (argc < 1) return -1;
    elfbench_arg_t *a = (elfbench_arg_t *)argv[0];
    a->result = a->mode == 1 ? bench_small(a->iters) : bench_big(a->iters);
    return 0;
}
