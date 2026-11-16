#include "helpers.hpp"
#include <unordered_set>
#include <algorithm>
#include <cctype>

using namespace std;
using namespace pando;

/* Count canonical 31-mers across reads, for choosing which k-mers to index.
 *
 * Each read contributes 1 to every distinct canonical k-mer it contains, so a
 * k-mer's count is the number of reads containing it (on either strand). That
 * is also the length of its posting list in the inverted index, which is what
 * the lower/upper count cutoffs are meant to bound.
 *
 * Count entries are keyed with make_kmer_count_key() (KMER_COUNT table) using
 * the same canonical encoding as read_kmer_index_frag and overlap_search, and
 * are tagged MERGE_STRATEGY=SUM so BQS adds them on merge. Summed values are
 * written as decimals (e.g. "22.000000"); parse them with stod/stoul.
 *
 * Input: entries tagged "read" whose value is
 *   read_id \n sequence \n + \n quality
 */

#define FILTER_NAME "read_count_canonical_kmers"
#define K 31

static const char* filter_done_tag = FILTER_NAME ":done";
static const char* filter_fail_tag = FILTER_NAME ":fail";

void read_count_canonical_kmers(const DBAccess *access, const char* filter_fail_tag, const char* filter_done_tag)
{
    string val(access->value);
    vector<string> lines;
    split_by_newline(val, &lines);

    if (lines.size() < 2)
    {
        cerr << "Expected at least 2 lines in read entry, got " << lines.size() << endl;
        access->add_tag.run(&access->add_tag, filter_fail_tag);
        return;
    }

    string seq = lines[1];
    seq.erase(remove_if(seq.begin(), seq.end(), [](unsigned char c) { return isspace(c); }), seq.end());

    if (seq.length() < K)
    {
        access->add_tag.run(&access->add_tag, filter_done_tag);
        return;
    }

    vector<uint64_t> codes;
    vector<uint8_t> strands;
    size_t bad_pos = 0;

    if (!encode_canonical_kmers_rolling(seq, K, codes, strands, bad_pos))
    {
        cerr << "Invalid nucleotide '" << seq[bad_pos] << "' at position " << bad_pos
             << " in read: '" << lines[0] << "'" << endl;
        access->add_tag.run(&access->add_tag, filter_fail_tag);
        return;
    }

    // One count per distinct canonical k-mer in this read
    sort(codes.begin(), codes.end());
    codes.erase(unique(codes.begin(), codes.end()), codes.end());

    const char* count_tags[] = {"count", "MERGE_STRATEGY=SUM", ""};

    for (uint64_t code : codes)
    {
        dbkey_t count_key = make_kmer_count_key(code);
        access->make_new_entry.run(&access->make_new_entry, count_tags, "1", count_key);
    }

    access->add_tag.run(&access->add_tag, filter_done_tag);
}

bool should_run_read_count_canonical_kmers(const DBAccess* access, const char* filter_fail_tag, const char* filter_done_tag)
{
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
    return read_count_canonical_kmers(access, filter_fail_tag, filter_done_tag);
}

// Fit the Pando API
extern "C" {
    /** @brief Main filter entry point */
    extern void run(void* access) {
        run_((DBAccess*)access);
    }

    extern bool should_run(const DBAccess* access) {
        return should_run_read_count_canonical_kmers(access, filter_fail_tag, filter_done_tag);
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
