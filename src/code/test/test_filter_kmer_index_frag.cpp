#include <iostream>
#include <string>

#include "seq_db.hpp"
#include "test.hpp"

using namespace std;
using namespace pando;

// read_kmer_index_frag indexes the canonical 31-mers of each read whose count
// (from read_count_canonical_kmers) is within the cutoffs, looking all of a
// read's counts up in one batched get_entries_by_keys call.

// Two copies of S give its 10 canonical 31-mers a count of 2 (in range); T's
// 10 31-mers (none shared with S) get a count of 1 (below the cutoff).
static const string S = "ACGTTGCATGCTAGCTAGGATCCGATTACAGGCTTAACGG";
static const string T = "TTGACCGTAGGCATCGATCGGCTAAGCTTCAGCTAGCAAT";
#define S_KMERS 10

// Read entries in the format the read filters expect: numeric read id,
// sequence, '+', quality
static DBEntry<> make_read(const string& seq, uint32_t read_id) {
    DBEntry<> e;
    e.add_tag("read");
    e.add_to_value(to_string(read_id) + "\n" + seq + "\n+\n" + string(seq.size(), 'I'));
    return e;
}

TEST(index_built_from_counts) {
    SeqDB db;
    db.add_entry(make_read(S, 1));
    db.add_entry(make_read(S, 2));
    db.add_entry(make_read(T, 3));
    db.add_filter_dir(build_dir + "/filters");

    // Count first, as the pipeline does before its BARRIER
    db.install_filter("read_count_canonical_kmers");
    db.process();

    db.install_filter("read_kmer_index_frag");
    db.process();

    size_t reads_done = 0, index_entries = 0;
    for (auto & [key, entry] : db.entries()) {
        if (entry.has_tag("read")) {
            EQ(entry.has_tag("read_count_canonical_kmers:fail"), false)
            EQ(entry.has_tag("read_kmer_index_frag:fail"), false)
            if (entry.has_tag("read_kmer_index_frag:done")) ++reads_done;
        }
        if (entry.has_tag("index")) {
            ++index_entries;
            // Only S's k-mers are indexed, each with a posting from both copies
            const string& v = entry.value();
            bool from_read_1 = v.rfind("1:", 0) == 0 || v.find("\n1:") != string::npos;
            bool from_read_2 = v.rfind("2:", 0) == 0 || v.find("\n2:") != string::npos;
            bool from_read_3 = v.rfind("3:", 0) == 0 || v.find("\n3:") != string::npos;
            EQ(from_read_1, true)
            EQ(from_read_2, true)
            EQ(from_read_3, false)
        }
    }
    EQ(reads_done, 3)
    EQ(index_entries, S_KMERS)

    TEST_PASS
}

TEST(missing_counts_fail_the_read) {
    SeqDB db;
    db.add_entry(make_read(S, 1));
    db.add_filter_dir(build_dir + "/filters");

    // No count phase: every count lookup comes back missing
    db.install_filter("read_kmer_index_frag");
    db.process();

    size_t index_entries = 0;
    bool failed = false;
    for (auto & [key, entry] : db.entries()) {
        if (entry.has_tag("read")) failed = entry.has_tag("read_kmer_index_frag:fail");
        if (entry.has_tag("index")) ++index_entries;
    }
    EQ(failed, true)
    EQ(index_entries, 0)

    TEST_PASS
}

TESTS_BEGIN
    elga::ZMQChatterbox::Setup();
    RUN_TEST(index_built_from_counts)
    RUN_TEST(missing_counts_fail_the_read)
    elga::ZMQChatterbox::Teardown();
TESTS_END
