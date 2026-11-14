#include <algorithm>
#include <iostream>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "seq_db.hpp"

#include "test.hpp"

using namespace std;
using namespace pando;

// BigSpace reuse under real SeqDB/PandoMap use.
//
// Each SeqDB is given a deliberately small BigSpace: enough when freed blocks
// are reused, too small if they are not (the old bump allocator ignored
// deallocate()). Sizes were calibrated against both allocators on these exact
// workloads:
//   posting_list_merges:  peak 1.55x live with reuse, 5.9-6.3x without -> space = 3x live
//   large_entry_retagging: peak 1.03x live with reuse, 4.0x without     -> space = 2x live
// Running out of BigSpace throws std::bad_alloc on the PandoMap thread, which
// aborts the test binary, so a regression shows up as a crashed test.
//
// Both tests also check that every value and tag survives intact, since
// reusing freed memory turns any use-after-free into corrupted data.

static const uint64_t TEST_TABLE = 10;

// Like index postings: many keys whose values grow by concatenation as lines
// arrive in random order, merged by stage_close() as during process().
TEST(posting_list_merges) {
    const size_t NUM_KEYS = 5000;
    const size_t STAGE_SIZE = 250;

    // Plan: Poisson(11) lines per key (clipped to 2..24), arriving shuffled
    mt19937 rng(7);
    poisson_distribution<int> pois(11.0);
    vector<int> events;
    for (size_t k = 0; k < NUM_KEYS; ++k) {
        int n = min(24, max(2, pois(rng)));
        for (int i = 0; i < n; ++i) events.push_back(static_cast<int>(k));
    }
    shuffle(events.begin(), events.end(), rng);

    // Expected values: lines joined by '\n' in arrival order (each line is
    // unique, so no merge is skipped as a duplicate)
    vector<string> lines(events.size());
    vector<int> seq(NUM_KEYS, 0);
    vector<string> expected(NUM_KEYS);
    for (size_t i = 0; i < events.size(); ++i) {
        int k = events[i];
        lines[i] = to_string(k) + ":" + to_string(rng() % 1000) + "+" + to_string(seq[k]++);
        expected[k] += (expected[k].empty() ? "" : "\n") + lines[i];
    }

    size_t live = 0;
    for (const auto& v : expected) live += v.size() + 1;

    SeqDB db { ZMQAddress {"127.0.0.1", 0}, 3 * live };

    const char* tags[] = {"posting", ""};
    for (size_t i = 0; i < events.size(); ++i) {
        dbkey_t key {TEST_TABLE, events[i], 0};
        db.stage_add_entry(DBEntry<> {tags, lines[i].c_str(), key});
        if ((i + 1) % STAGE_SIZE == 0)
            db.stage_close();
    }
    db.stage_close();

    EQ(db.size(), NUM_KEYS);

    size_t checked = 0;
    for (auto & [key, entry] : db.entries()) {
        EQ(key.a, TEST_TABLE);
        EQ(entry.has_tag("posting"), true);
        EQ(string(entry.value()), expected.at(key.b));
        ++checked;
    }
    EQ(checked, NUM_KEYS);

    TEST_PASS
}

// Like reads: large values (~20 KB) re-inserted whole every time a tag is
// added, as add_tags_to_entries() does after each filter marks a read done.
TEST(large_entry_retagging) {
    const size_t NUM_ENTRIES = 200;
    const size_t VALUE_LEN = 20000;
    const vector<string> phase_tags = {"count:done", "index:done", "search:done"};

    vector<string> values(NUM_ENTRIES);
    for (size_t i = 0; i < NUM_ENTRIES; ++i) {
        values[i].assign(VALUE_LEN, 'A' + (i % 26));
        values[i].replace(0, to_string(i).size(), to_string(i));   // make each value distinct
    }

    SeqDB db { ZMQAddress {"127.0.0.1", 0}, 2 * NUM_ENTRIES * (VALUE_LEN + 1) };

    const char* tags[] = {"read", ""};
    for (size_t i = 0; i < NUM_ENTRIES; ++i)
        db.add_entry(DBEntry<> {tags, values[i].c_str(), dbkey_t {TEST_TABLE, (vtx_t)i, 0}});

    EQ(db.size(), NUM_ENTRIES);

    for (const auto& tag : phase_tags) {
        for (size_t i = 0; i < NUM_ENTRIES; ++i)
            db.stage_add_tag(dbkey_t {TEST_TABLE, (vtx_t)i, 0}, tag);
        db.stage_close();
    }

    EQ(db.size(), NUM_ENTRIES);

    size_t checked = 0;
    for (auto & [key, entry] : db.entries()) {
        EQ(string(entry.value()), values.at(key.b));
        EQ(entry.has_tag("read"), true);
        for (const auto& tag : phase_tags)
            EQ(entry.has_tag(tag), true);
        EQ(entry.tags().size(), phase_tags.size() + 1);
        ++checked;
    }
    EQ(checked, NUM_ENTRIES);

    TEST_PASS
}

TESTS_BEGIN
    elga::ZMQChatterbox::Setup();
    RUN_TEST(posting_list_merges)
    RUN_TEST(large_entry_retagging)
    elga::ZMQChatterbox::Teardown();
TESTS_END
