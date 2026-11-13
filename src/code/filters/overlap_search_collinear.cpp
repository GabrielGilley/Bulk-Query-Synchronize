#include "helpers.hpp"
#include <sstream>
#include <unordered_map>
#include <stdexcept>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <cctype>

using namespace std;
using namespace pando;
using namespace rapidjson;

/* Collinear (diagonal) overlap search over read_kmer_index_collinear.
 *
 * Based on the collinearity method (c_index_t::search in ratschlab/collinearity):
 * for each query k-mer at position q and each indexed occurrence of it at
 * position t in another read, vote for that pair's alignment diagonal. Shared
 * k-mers from a true overlap all fall on one diagonal; unrelated shared k-mers
 * (repeats, chance matches) scatter across many.
 *
 * Differences from collinearity, which maps a read to its single best place in
 * a reference:
 *   - every target read whose best diagonal passes THRESHOLD is reported, not
 *     just the top one, since a read overlaps many others;
 *   - strands come from canonical k-mers (strand bit per occurrence) instead of
 *     indexing both strands. Same orientation: hits line up on t - q (diagonal).
 *     Opposite orientation: on t + q (anti-diagonal);
 *   - a diagonal's score is its bucket plus the next one. Target positions are
 *     stored as buckets of BW bases, so a true diagonal's votes split across two
 *     adjacent diagonal buckets; summing them avoids the one-bucket bias
 *     collinearity documents and corrects for (bench/NOTES.md there);
 *   - each vote counts once per (query occurrence, target occurrence) pair.
 *
 * Input: entries tagged "read" (read_id \n sequence \n + \n quality), after
 * read_kmer_index_collinear has finished (BARRIER). K and BW must match it.
 *
 * Output: one entry per (query, target) tagged "candidate", value
 *   query,target,offset,presence,strand
 *     strand   : '+' same orientation, '-' target is the reverse complement
 *     offset   : '+' -> target_pos - query_pos (target coordinate where query
 *                       position 0 lands; may be negative)
 *                '-' -> target_pos + query_pos (the query k-mer at q lies at
 *                       target position offset - q)
 *                Approximate to within about BW bases.
 *     presence : diagonal score / number of k-mers in the query
 * Each overlap is reported from both reads (A->B and B->A).
 */

#define FILTER_NAME "overlap_search_collinear"
#define K 31
#define BW 16
#define THRESHOLD 0.1

static const char* filter_done_tag = FILTER_NAME ":done";
static const char* filter_fail_tag = FILTER_NAME ":fail";

// Diagonal buckets are biased so they pack as unsigned 31-bit values
static const int64_t DIAG_BIAS = (1ll << 30);

static inline int64_t floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

// (target read, relative strand, diagonal bucket) -> one 64-bit accumulator key
static inline uint64_t diag_key(uint32_t target, uint32_t rel_strand, int64_t diag_bucket)
{
    return (static_cast<uint64_t>(target) << 32) |
           (static_cast<uint64_t>(rel_strand) << 31) |
           static_cast<uint64_t>(diag_bucket + DIAG_BIAS);
}

static void print_invalid(const char* s, size_t n, const char* why)
{
    cerr << "Invalid index value (" << why << "): '";
    cerr.write(s, n);
    cerr << "'" << endl;
}

void overlap_search_collinear(const DBAccess *access, const char* filter_fail_tag, const char* filter_done_tag)
{
    string val(access->value);
    vector<string> lines;
    split_by_newline(val, &lines);

    if (lines.size() < 4)
    {
        cerr << "Expected at least 4 lines in read entry, got " << lines.size() << endl;
        access->add_tag.run(&access->add_tag, filter_fail_tag);
        return;
    }

    string query_read_id_str = lines[0];
    query_read_id_str.erase(remove_if(query_read_id_str.begin(), query_read_id_str.end(), [](unsigned char c) {
        return isspace(c);
    }), query_read_id_str.end());

    string seq = lines[1];
    seq.erase(remove_if(seq.begin(), seq.end(), [](unsigned char c) {
        return isspace(c);
    }), seq.end());

    if (query_read_id_str.empty() || seq.length() < K)
    {
        access->add_tag.run(&access->add_tag, filter_done_tag);
        return;
    }

    uint32_t query_read_id = 0;

    {
        uint64_t parsed = 0;

        for (unsigned char c : query_read_id_str)
        {
            if (!isdigit(c))
            {
                cerr << "Invalid query read id: '" << query_read_id_str << "'" << endl;
                access->add_tag.run(&access->add_tag, filter_fail_tag);
                return;
            }

            parsed = parsed * 10 + static_cast<uint64_t>(c - '0');

            if (parsed > numeric_limits<uint32_t>::max())
            {
                cerr << "Query read id exceeds uint32: '" << query_read_id_str << "'" << endl;
                access->add_tag.run(&access->add_tag, filter_fail_tag);
                return;
            }
        }

        query_read_id = static_cast<uint32_t>(parsed);
    }

    // Canonical k-mers, so the index finds reads from either strand
    vector<uint64_t> codes;
    vector<uint8_t> strands;
    size_t bad_pos = 0;

    if (!encode_canonical_kmers_rolling(seq, K, codes, strands, bad_pos))
    {
        cerr << "Invalid nucleotide '" << seq[bad_pos] << "' at position " << bad_pos
             << " in query read id: '" << query_read_id_str << "'" << endl;
        access->add_tag.run(&access->add_tag, filter_fail_tag);
        return;
    }

    const uint64_t total_query_kmers = codes.size();

    // Distinct canonical k-mers with every query occurrence (position, strand):
    // one lookup per distinct k-mer, but each occurrence votes on its own diagonal.
    struct QueryOccurrence
    {
        int64_t pos;
        uint32_t strand;
    };

    unordered_map<uint64_t, vector<QueryOccurrence>> query_kmers;
    query_kmers.reserve(codes.size());

    for (size_t i = 0; i < codes.size(); ++i)
        query_kmers[codes[i]].push_back({static_cast<int64_t>(i), strands[i]});

    unordered_map<uint64_t, uint64_t> diagonal_votes;
    diagonal_votes.reserve(query_kmers.size() * 4);

    // Parse one posting line "read_id:bucket<+|->[*count],..." and vote
    auto process_index_value = [&](const char* s, size_t n, const vector<QueryOccurrence>& occurrences) -> bool
    {
        size_t p = 0;
        uint64_t parsed_read_id = 0;
        bool saw_read_digit = false;

        while (p < n && s[p] != ':')
        {
            unsigned char c = static_cast<unsigned char>(s[p]);

            if (isspace(c)) { ++p; continue; }

            if (!isdigit(c)) { print_invalid(s, n, "bad read id"); return false; }

            saw_read_digit = true;
            parsed_read_id = parsed_read_id * 10 + static_cast<uint64_t>(c - '0');

            if (parsed_read_id > numeric_limits<uint32_t>::max()) { print_invalid(s, n, "read id too large"); return false; }

            ++p;
        }

        if (!saw_read_digit || p >= n || s[p] != ':') { print_invalid(s, n, "missing read id"); return false; }

        uint32_t hit_read_id = static_cast<uint32_t>(parsed_read_id);

        if (hit_read_id == query_read_id)
            return true;

        ++p;

        while (p < n)
        {
            while (p < n && isspace(static_cast<unsigned char>(s[p])))
                ++p;

            if (p >= n)
                break;

            // bucket
            uint64_t bucket = 0;
            bool saw_bucket_digit = false;

            while (p < n && isdigit(static_cast<unsigned char>(s[p])))
            {
                saw_bucket_digit = true;
                bucket = bucket * 10 + static_cast<uint64_t>(s[p] - '0');

                if (bucket > (numeric_limits<uint32_t>::max() >> 1)) { print_invalid(s, n, "bucket too large"); return false; }

                ++p;
            }

            if (!saw_bucket_digit) { print_invalid(s, n, "missing bucket"); return false; }

            // strand of the k-mer in the target read
            if (p >= n || (s[p] != '+' && s[p] != '-')) { print_invalid(s, n, "missing strand after bucket"); return false; }

            uint32_t target_strand = (s[p] == '-') ? 1 : 0;
            ++p;

            // optional *count
            uint64_t index_count = 1;

            if (p < n && s[p] == '*')
            {
                ++p;
                index_count = 0;
                bool saw_count_digit = false;

                while (p < n && isdigit(static_cast<unsigned char>(s[p])))
                {
                    saw_count_digit = true;
                    index_count = index_count * 10 + static_cast<uint64_t>(s[p] - '0');
                    ++p;
                }

                if (!saw_count_digit || index_count == 0) { print_invalid(s, n, "bad count"); return false; }
            }

            if (p < n && s[p] != ',') { print_invalid(s, n, "unexpected character"); return false; }

            if (p < n)
                ++p;

            const int64_t target_pos = static_cast<int64_t>(bucket) * BW;

            for (const QueryOccurrence& q : occurrences)
            {
                uint32_t rel_strand = q.strand ^ target_strand;
                int64_t diagonal = rel_strand ? (target_pos + q.pos) : (target_pos - q.pos);
                int64_t diag_bucket = floor_div(diagonal, BW);

                if (diag_bucket <= -DIAG_BIAS || diag_bucket >= DIAG_BIAS - 1) { print_invalid(s, n, "diagonal out of range"); return false; }

                diagonal_votes[diag_key(hit_read_id, rel_strand, diag_bucket)] += index_count;
            }
        }

        return true;
    };

    for (const auto& [encoded_kmer, occurrences] : query_kmers)
    {
        dbkey_t kmer_key = make_kmer_inv_index_key(encoded_kmer);

        char* kmer_ret = nullptr;
        access->get_entry_by_key.run(&access->get_entry_by_key, kmer_key, &kmer_ret);

        if (kmer_ret == nullptr)
            continue;

        const char* line_start = kmer_ret;

        for (const char* p = kmer_ret; ; ++p)
        {
            if (*p == '\n' || *p == '\0')
            {
                size_t line_len = static_cast<size_t>(p - line_start);

                if (line_len > 0 && line_start[line_len - 1] == '\r')
                    --line_len;

                if (line_len > 0 && !process_index_value(line_start, line_len, occurrences))
                {
                    free(kmer_ret);
                    access->add_tag.run(&access->add_tag, filter_fail_tag);
                    return;
                }

                if (*p == '\0')
                    break;

                line_start = p + 1;
            }
        }

        free(kmer_ret);
    }

    // Best diagonal per target read: score = votes in a bucket + the next bucket
    struct Best
    {
        uint64_t score = 0;
        int64_t diag_bucket = 0;
        uint32_t rel_strand = 0;
    };

    unordered_map<uint32_t, Best> best_by_target;

    for (const auto& [key, votes] : diagonal_votes)
    {
        uint32_t target = static_cast<uint32_t>(key >> 32);
        uint32_t rel_strand = static_cast<uint32_t>((key >> 31) & 1);
        int64_t diag_bucket = static_cast<int64_t>(key & 0x7FFFFFFFull) - DIAG_BIAS;

        uint64_t score = votes;
        auto next = diagonal_votes.find(key + 1);
        if (next != diagonal_votes.end())
            score += next->second;

        Best& best = best_by_target[target];
        if (score > best.score)
            best = {score, diag_bucket, rel_strand};
    }

    const char* candidate_tags[] = {"candidate", ""};

    for (const auto& [target, best] : best_by_target)
    {
        double presence = static_cast<double>(best.score) / static_cast<double>(total_query_kmers);

        if (presence < THRESHOLD)
            continue;

        // The two summed buckets cover diagonals [b*BW, (b+2)*BW); report the middle
        int64_t offset = (best.diag_bucket + 1) * BW;

        string candidate_value =
            query_read_id_str + "," +
            to_string(target) + "," +
            to_string(offset) + "," +
            to_string(presence) + "," +
            (best.rel_strand ? "-" : "+");

        access->make_new_entry.run(&access->make_new_entry, candidate_tags, candidate_value.c_str(), INITIAL_KEY);
    }

    access->add_tag.run(&access->add_tag, filter_done_tag);
}

bool should_run_overlap_search_collinear(const DBAccess* access, const char* filter_fail_tag, const char* filter_done_tag) {
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
    return overlap_search_collinear(access, filter_fail_tag, filter_done_tag);
}

// Fit the Pando API
extern "C" {
    /** @brief Main filter entry point */
    extern void run(void* access) {
        run_((DBAccess*)access);
    }

    extern bool should_run(const DBAccess* access) {
        return should_run_overlap_search_collinear(access, filter_fail_tag, filter_done_tag);
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
