#pragma once

#include <cstddef>

namespace pando {

/** @brief Settings for a ParDB agent (see pando_pardb's command line)
 *
 * A value of 0 for a size means "use the default".
 */
struct ParDBOptions {
    /** @brief Default flush size for write batches (see ParDB::set_write_batch_bytes) */
    static constexpr size_t DEFAULT_WRITE_BATCH_BYTES = 256 * 1024;

    /** @brief Skip processing of group filters */
    bool skip_group_filters = false;

    /** @brief Filter runs per processing pass before the stage closes (see
     * SeqDB::set_max_filters_per_stage); 0 = SeqDB's default */
    size_t max_filters_per_stage = 0;

    /** @brief Flush size for write batches; 0 turns batching off */
    size_t write_batch_bytes = DEFAULT_WRITE_BATCH_BYTES;

    /** @brief Each process request makes one sweep over the entries and
     * stops (see SeqDB::set_single_sweep) */
    bool single_sweep = false;

    /** @brief Keys the filter scan fetches from the map per request (see
     * SeqDB::set_scan_block_keys); 0 = SeqDB's default */
    size_t scan_block_keys = 0;
};

}
