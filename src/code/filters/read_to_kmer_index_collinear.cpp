#include "helpers.hpp"
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <stdexcept>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <cctype>

using namespace std;
using namespace pando;
using namespace rapidjson;

/* Inverted k-mer index for collinear (diagonal) overlap search.
 *
 * Based on the collinearity method (c_index_t in ratschlab/collinearity): each
 * k-mer occurrence is stored with its position, quantized into buckets of BW
 * bases, instead of the fragment it falls in (read_kmer_index_frag).
 * overlap_search_collinear then votes on alignment diagonals.
 *
 * Same input, count gating, tags and failure handling as read_kmer_index_frag:
 *   - input: entries tagged "read", value = read_id \n sequence \n + \n quality
 *   - canonical 31-mers; only k-mers whose read count (from
 *     read_count_canonical_kmers) is within KMER_COUNT_MIN..KMER_COUNT_MAX are
 *     indexed; a missing/unreadable count fails the read before anything is written
 *   - index entries keyed with make_kmer_inv_index_key(), tagged "index"
 *
 * Posting format (one line per read, merged per k-mer by BQS with '\n'):
 *   read_id:bucket<+|->[*count],...
 *     bucket : k-mer start position in the read (forward coordinates) / BW
 *     +      : the read contains the canonical k-mer
 *     -      : the read contains its reverse complement
 *     *count : occurrences with the same bucket and strand (omitted if 1)
 *
 * Not compatible with read_kmer_index_frag postings (same key table, different
 * meaning of the number after ':'); run one pipeline or the other. K and BW must
 * match overlap_search_collinear.
 */

#define FILTER_NAME "read_kmer_index_collinear"
#define K 31
#define BW 16

static const char* filter_done_tag = FILTER_NAME ":done";
static const char* filter_fail_tag = FILTER_NAME ":fail";

bool is_uint32_string(const string& s) {
    if (s.empty())
        return false;

    for (unsigned char c : s) {
        if (!isdigit(c))
            return false;
    }

    return true;
}

void read_kmer_index_collinear(const DBAccess *access, const char* filter_fail_tag, const char* filter_done_tag)
{
    string val = string(access->value);
    vector<string> lines;
    split_by_newline(val, &lines);

    // Expected VALUE layout:
    // lines[0] = read_id
    // lines[1] = sequence
    // lines[2] = +
    // lines[3] = quality
    if (lines.size() < 4)
    {
        cerr << "Expected at least 4 lines in read entry, got " << lines.size() << endl;
        access->add_tag.run(&access->add_tag, filter_fail_tag);
        return;
    }

    string read_id = lines[0];
    read_id.erase(std::remove_if(read_id.begin(), read_id.end(), [](unsigned char c) {
        return std::isspace(c);
    }), read_id.end());

    string seq = lines[1];
    seq.erase(std::remove_if(seq.begin(), seq.end(), [](unsigned char c) {
        return std::isspace(c);
    }), seq.end());

    if (seq.empty())
    {
        cerr << "Empty sequence for read id: '" << read_id << "'" << endl;
        access->add_tag.run(&access->add_tag, filter_fail_tag);
        return;
    }

    if (!is_uint32_string(read_id))
    {
        cerr << "Invalid read id in lines[0]: '" << read_id << "'" << endl;
        access->add_tag.run(&access->add_tag, filter_fail_tag);
        return;
    }

    try
    {
        unsigned long long parsed_id = stoull(read_id);

        if (parsed_id > numeric_limits<uint32_t>::max())
        {
            cerr << "Read id exceeds 32 bits: '" << read_id << "'" << endl;
            access->add_tag.run(&access->add_tag, filter_fail_tag);
            return;
        }
    }
    catch (const exception& e)
    {
        cerr << "Unable to parse read id from lines[0]: '" << read_id << "'" << endl;
        cerr << "Error: " << e.what() << endl;
        access->add_tag.run(&access->add_tag, filter_fail_tag);
        return;
    }

    if (seq.length() < K)
    {
        access->add_tag.run(&access->add_tag, filter_done_tag);
        return;
    }

    // Canonical encoding (the smaller of the k-mer and its reverse complement)
    // so reads from opposite strands share keys.
    vector<uint64_t> encoded;
    vector<uint8_t> strands;
    size_t bad_pos = 0;
    if (!encode_canonical_kmers_rolling(seq, K, encoded, strands, bad_pos))
    {
        cerr << "Invalid nucleotide '" << seq[bad_pos] << "' at position " << bad_pos
             << " in read id: '" << read_id << "'" << endl;
        access->add_tag.run(&access->add_tag, filter_fail_tag);
        return;
    }

    if ((encoded.size() - 1) / BW > (numeric_limits<uint32_t>::max() >> 1))
    {
        cerr << "Read too long to bucket positions for read id: '" << read_id << "'" << endl;
        access->add_tag.run(&access->add_tag, filter_fail_tag);
        return;
    }

    // Per canonical k-mer: one (bucket << 1 | strand) value per occurrence.
    unordered_map<uint64_t, vector<uint32_t>> kmer_buckets;

    for (size_t i = 0; i < encoded.size(); ++i)
        kmer_buckets[encoded[i]].push_back((static_cast<uint32_t>(i / BW) << 1) | strands[i]);

    // Only index k-mers whose read count is within the cutoffs: count-1 k-mers
    // (mostly sequencing errors) cannot link reads, and very frequent ones are
    // repeats whose posting lists would be costly to merge.
    //
    // All counts are checked before anything is written, so a read whose counts
    // are missing or unreadable (the count phase did not run correctly) fails
    // without leaving a partial set of postings in the index.
    unordered_set<uint64_t> kmers_to_index;
    kmers_to_index.reserve(kmer_buckets.size());

    for (const auto& pair : kmer_buckets)
    {
        KmerCountStatus status = check_kmer_count(access, pair.first);

        if (status == KmerCountStatus::MISSING || status == KmerCountStatus::INVALID)
        {
            cerr << "[ERROR] unable to run filter: " << FILTER_NAME << " read id '" << read_id << "': "
                 << (status == KmerCountStatus::MISSING
                         ? "no count entry for a k-mer (was read_count_canonical_kmers run first, with a BARRIER?)"
                         : "unreadable count entry for a k-mer (counts not summed?)")
                 << endl;
            access->add_tag.run(&access->add_tag, filter_fail_tag);
            return;
        }

        if (status == KmerCountStatus::IN_RANGE)
            kmers_to_index.insert(pair.first);
    }

    const char* kmer_index_tags[] = {"index", ""};

    for (auto& pair : kmer_buckets)
    {
        uint64_t encoded_kmer = pair.first;
        vector<uint32_t>& occurrences = pair.second;

        if (kmers_to_index.count(encoded_kmer) == 0)
            continue;

        // Group by bucket, then strand
        sort(occurrences.begin(), occurrences.end());

        dbkey_t new_key = make_kmer_inv_index_key(encoded_kmer);

        string new_value = read_id + ":";

        bool first_bucket = true;

        for (size_t j = 0; j < occurrences.size();)
        {
            uint32_t occurrence = occurrences[j];
            uint64_t count = 1;
            ++j;

            while (j < occurrences.size() && occurrences[j] == occurrence)
            {
                ++count;
                ++j;
            }

            if (!first_bucket)
                new_value += ",";

            first_bucket = false;

            new_value += to_string(occurrence >> 1);
            new_value += (occurrence & 1) ? '-' : '+';

            if (count > 1)
            {
                new_value += "*";
                new_value += to_string(count);
            }
        }

        access->make_new_entry.run(&access->make_new_entry, kmer_index_tags, new_value.c_str(), new_key);
    }

    access->add_tag.run(&access->add_tag, filter_done_tag);
}

bool should_run_read_kmer_index_collinear(const DBAccess* access, const char* filter_fail_tag, const char* filter_done_tag) {
    auto tags = access->tags;
    bool found_read = false;

    while ((*tags)[0] != '\0') {
        if (string(*tags) == "read")
            found_read = true;
        if (string(*tags) == filter_done_tag)
            return false;
        if (string(*tags) == filter_fail_tag)
            return false;
        ++tags;
    }

    return found_read;
}

void run_(const DBAccess *access) {
    return read_kmer_index_collinear(access, filter_fail_tag, filter_done_tag);
}

// Fit the Pando API
extern "C" {
    /** @brief Main filter entry point */
    extern void run(void* access) {
        run_((DBAccess*)access);
    }

    extern bool should_run(const DBAccess* access) {
        return should_run_read_kmer_index_collinear(access, filter_fail_tag, filter_done_tag);
    }

    /** @brief Contains the entry point and tags for the filter */
    extern const FilterInterface filter {
        filter_name: FILTER_NAME,
        filter_type: SINGLE_ENTRY,
        should_run: &should_run,
        init: nullptr,
        destroy: nullptr,
        run: &run
    };
}
