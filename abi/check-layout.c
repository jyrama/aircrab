/* Compile-time lock of the nqptp shm ABI layout (SMI v11). Built by CI
 * (cc -I abi -fsyntax-only abi/check-layout.c) against the canonical header in
 * this directory; abi/check-sync.sh keeps the copies in both legacy trees
 * byte-identical to it. Any ABI change must bump NQPTP_SHM_STRUCTURES_VERSION,
 * update all copies, and update these asserts. */
#include <stddef.h>

#include "nqptp-shm-structures.h"

_Static_assert(NQPTP_SHM_STRUCTURES_VERSION == 11, "SMI version must be 11");
_Static_assert(NQPTP_CONTROL_PORT == 9000, "control port must be 9000");

_Static_assert(sizeof(shm_structure_set) == 40, "shm_structure_set is 5 x u64");
_Static_assert(offsetof(shm_structure_set, master_clock_id) == 0, "master_clock_id at 0");
_Static_assert(offsetof(shm_structure_set, local_time) == 8, "local_time at 8");
_Static_assert(offsetof(shm_structure_set, local_to_master_time_offset) == 16,
               "local_to_master_time_offset at 16");
_Static_assert(offsetof(shm_structure_set, clock_change_offset) == 24, "clock_change_offset at 24");
_Static_assert(offsetof(shm_structure_set, master_clock_start_time) == 32,
               "master_clock_start_time at 32");

_Static_assert(offsetof(struct shm_structure, version) == 0, "version at 0");
_Static_assert(offsetof(struct shm_structure, main) == 8, "main at 8");
_Static_assert(offsetof(struct shm_structure, secondary) == 48, "secondary at 48");
_Static_assert(sizeof(struct shm_structure) == 88, "shm_structure is 88 bytes");
