#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "seq_db.hpp"

#include "test.hpp"

using namespace std;
using namespace pando;

// product_engagement_{10,50,90} join table1 rows (USER_ID,PRODUCT_ID,ENGAGEMENT_SCORE)
// to table2 rows (PRODUCT_ID,PRODUCT_SPECS) keyed by product_engagement_set_keys,
// keeping rows whose score is at least X:
//   product_engagement_10: X = 91   (scores 1..100 -> ~10% of rows join)
//   product_engagement_50: X = 51   (~50%)
//   product_engagement_90: X = 11   (~90%)
// A join is a new entry tagged product_engagement_join with value
// "USER_ID,PRODUCT_ID,ENGAGEMENT_SCORE,PRODUCT_SPECS".

static void load_tables(SeqDB& db) {
    // table1: includes scores on both sides of every cutoff (91/90, 51/50, 11/10)
    const vector<string> table1 = {
        "1,1,62", "1,2,49", "1,3,5",
        "2,1,90", "2,2,12", "2,3,50",
        "3,1,7",  "3,2,42", "3,3,75",
        "4,1,91", "4,2,51", "4,3,11",
        "5,1,10",
    };
    // table2
    const vector<string> table2 = {"1,100", "2,43", "3,86"};

    for (const auto& v : table1) {
        DBEntry<> e;
        e.value() = v;
        e.add_tag("product_engagement_table1");
        db.add_entry(move(e));
    }
    for (const auto& v : table2) {
        DBEntry<> e;
        e.value() = v;
        e.add_tag("product_engagement_table2");
        db.add_entry(move(e));
    }
}

static set<string> run_join(const string& build_dir, const string& filter_name) {
    SeqDB db;
    load_tables(db);
    db.add_filter_dir(build_dir + "/filters");
    db.install_filter("product_engagement_set_keys");
    db.install_filter(filter_name);
    db.process();

    set<string> joined;
    for (auto & [key, entry] : db.entries()) {
        if (entry.has_tag("product_engagement_join"))
            joined.insert(entry.value());
        // no row may fail or be left waiting for its product
        if (entry.has_tag(filter_name + ":fail") || entry.has_tag(filter_name + ":inactive"))
            joined.insert("UNEXPECTED_TAG:" + entry.value());
    }
    return joined;
}

TEST(join_10) {
    set<string> joined = run_join(build_dir, "product_engagement_10");
    const set<string> expected = {"4,1,91,100"};

    EQ(joined.size(), expected.size());
    for (const auto& v : expected) EQ(joined.count(v), 1u);
    TEST_PASS
}

TEST(join_50) {
    set<string> joined = run_join(build_dir, "product_engagement_50");
    const set<string> expected = {
        "1,1,62,100", "2,1,90,100", "3,3,75,86", "4,1,91,100", "4,2,51,43",
    };

    EQ(joined.size(), expected.size());
    for (const auto& v : expected) EQ(joined.count(v), 1u);
    TEST_PASS
}

TEST(join_90) {
    set<string> joined = run_join(build_dir, "product_engagement_90");
    const set<string> expected = {
        "1,1,62,100", "1,2,49,43", "2,1,90,100", "2,2,12,43", "2,3,50,86",
        "3,2,42,43",  "3,3,75,86", "4,1,91,100", "4,2,51,43", "4,3,11,86",
    };

    EQ(joined.size(), expected.size());
    for (const auto& v : expected) EQ(joined.count(v), 1u);
    TEST_PASS
}

TESTS_BEGIN
    elga::ZMQChatterbox::Setup();
    RUN_TEST(join_10)
    RUN_TEST(join_50)
    RUN_TEST(join_90)
    elga::ZMQChatterbox::Teardown();
TESTS_END
