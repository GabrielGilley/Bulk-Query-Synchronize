#include "test.hpp"
#include "par_db.hpp"
#include "par_db_responder.hpp"
#include "test_helpers.hpp"

#include <chrono>
#include <thread>

using namespace std;
using namespace pando;

localnum_t g_idx = 0;
size_t inc_amount = 3;

TEST(reuse_addr) {
    {   
        elga::ZMQAddress db1_addr { "127.0.0.1", 0 };
        PandoParticipant db1 { db1_addr, true };
        this_thread::sleep_for(chrono::milliseconds(50));
    }
    this_thread::sleep_for(chrono::milliseconds(100));
    {   
        elga::ZMQAddress db1_addr { "127.0.0.1", 0 };
        ParDB db1 { db1_addr };
    }
    
    TEST_PASS
}

TEST(handshake_twice) {
    elga::ZMQAddress db1_addr { "127.0.0.1", g_idx+=inc_amount };
    PandoParticipant db1 { db1_addr, true };

    elga::ZMQAddress db2_addr { "127.0.0.1", g_idx+=inc_amount };
    PandoParticipant db2 { db2_addr, true };

    EQ(db1.num_neighbors(), 1);
    EQ(db2.num_neighbors(), 1);

    EQ(db1.add_neighbor(db2_addr.serialize()), true);
    EQ(db1.add_neighbor(db2_addr.serialize()), false);

    EQ(db1.num_neighbors(), 2);
    EQ(db2.num_neighbors(), 1);

    TEST_PASS
}

TEST(handshake) {
    // Create two parallel databases
    elga::ZMQAddress db1_addr { "127.0.0.1", g_idx+=inc_amount };
    ParDB db1 { db1_addr };

    elga::ZMQAddress db2_addr { "127.0.0.1", g_idx+=inc_amount };
    ParDB db2 { db2_addr };

    db1.add_neighbor(db2_addr.serialize());

    EQ(db1.num_neighbors(), 2);
    EQ(db2.num_neighbors(), 1);

    // Ensure it finishes within 50 ms
    this_thread::sleep_for(chrono::milliseconds(50));

    db2.recv_msg();
    db1.recv_msg();

    EQ(db1.num_neighbors(), 2);
    EQ(db2.num_neighbors(), 2);

    TEST_PASS
}

TEST(serving) {
    elga::ZMQAddress db1_addr { "127.0.0.1", g_idx+=inc_amount };
    ParDBThread db1 { db1_addr };

    elga::ZMQAddress db2_addr { "127.0.0.1", g_idx+=inc_amount };
    ParDBThread db2 { db2_addr };

    ParDBClient c1 { db1_addr };
    ParDBClient c2 { db2_addr };

    EQ(c1.num_neighbors(), 1);
    EQ(c2.num_neighbors(), 1);

    c1.add_neighbor(db2_addr);

    // Ensure it finishes within 50 ms
    this_thread::sleep_for(chrono::milliseconds(50));

    EQ(c1.num_neighbors(), 2);
    EQ(c2.num_neighbors(), 2);

    TEST_PASS
}

TEST(ring_size) {
    elga::ZMQAddress db1_addr { "127.0.0.1", g_idx+=inc_amount };
    ParDBThread db1 { db1_addr };
    ParDBClient c1 { db1_addr };

    EQ(c1.ring_size(), 1*STARTING_VAGENTS);

    elga::ZMQAddress db2_addr { "127.0.0.1", g_idx+=inc_amount };
    ParDBThread db2 { db2_addr };
    ParDBClient c2 { db2_addr };

    EQ(c2.ring_size(), 1*STARTING_VAGENTS);

    c2.add_neighbor(db1_addr);

    this_thread::sleep_for(chrono::milliseconds(50));
    EQ(c1.ring_size(), 2*STARTING_VAGENTS);
    EQ(c2.ring_size(), 2*STARTING_VAGENTS);

    TEST_PASS
}

TEST(add_entry_single) {
    elga::ZMQAddress db1_addr { "127.0.0.1", g_idx+=inc_amount };
    ParDB db1 { db1_addr };
    EQ(db1.db_size(), 0);

    dbkey_t key {1,2,3};
    DBEntry<> e; e.set_key(key); e.value() = "test1"; db1.add_entry(move(e));

    this_thread::sleep_for(chrono::milliseconds(50));

    EQ(db1.db_size(), 0);

    db1.recv_msg();

    EQ(db1.db_size(), 1);

    TEST_PASS
}

TEST(add_entry_multiple) {
    g_idx = 0;      // If this test fails, determine if 0 is not free
    elga::ZMQAddress db1_addr { "127.0.0.1", g_idx+=inc_amount };
    ParDB db1 { db1_addr };

    elga::ZMQAddress db2_addr { "127.0.0.1", g_idx+=inc_amount };
    ParDB db2 { db2_addr };

    db1.add_neighbor(db2_addr.serialize());

    db2.recv_msg();

    EQ(db1.num_neighbors(), 2);
    EQ(db2.num_neighbors(), 2);

    dbkey_t key {1,2,4};
    DBEntry<> e; e.set_key(key); e.value() = "test1"; db1.add_entry(move(e));

    this_thread::sleep_for(chrono::milliseconds(50));

    EQ(db1.db_size(), 0);
    EQ(db2.db_size(), 0);

    db1.recv_msg();
    db2.recv_msg();

    EQ(db1.db_size(), 1);
    EQ(db2.db_size(), 0);

    TEST_PASS
}

TEST(mesh) {
    // Create a mesh of several DBs and ensure they all match
    elga::ZMQAddress db1_addr { "127.0.0.1", g_idx+=inc_amount };
    ParDBThread db1 { db1_addr };
    elga::ZMQAddress db2_addr { "127.0.0.1", g_idx+=inc_amount };
    ParDBThread db2 { db2_addr };
    elga::ZMQAddress db3_addr { "127.0.0.1", g_idx+=inc_amount };
    ParDBThread db3 { db3_addr };
    elga::ZMQAddress db4_addr { "127.0.0.1", g_idx+=inc_amount };
    ParDBThread db4 { db4_addr };

    ParDBClient c1 { db1_addr };
    ParDBClient c2 { db2_addr };
    ParDBClient c3 { db3_addr };
    ParDBClient c4 { db4_addr };

    c1.add_neighbor(db2_addr);
    c3.add_neighbor(db4_addr);
    c2.add_neighbor(db4_addr);

    this_thread::sleep_for(chrono::milliseconds(50));

    EQ(c2.num_neighbors(), 3);
    EQ(c1.num_neighbors(), 2);

    this_thread::sleep_for(chrono::milliseconds(2050));

    EQ(c1.num_neighbors(), 4);
    EQ(c3.num_neighbors(), 4);
    EQ(c4.num_neighbors(), 4);

    TEST_PASS
}

TEST(forward_add_entry) {
    g_idx = 0;      // If this test fails, determine if 0 is not free

    elga::ZMQAddress db1_addr { "127.0.0.1", g_idx+=inc_amount };
    ParDBThread db1 { db1_addr };
    ParDBClient c1 { db1_addr };

    elga::ZMQAddress db2_addr { "127.0.0.1", g_idx+=inc_amount };
    ParDBThread db2 { db2_addr };
    ParDBClient c2 { db2_addr };

    c1.add_neighbor(db2_addr.serialize());

    this_thread::sleep_for(chrono::milliseconds(50));

    EQ(c1.num_neighbors(), 2);
    EQ(c2.num_neighbors(), 2);

    this_thread::sleep_for(chrono::milliseconds(50));

    dbkey_t key {1,2,4};
    DBEntry<> e; e.set_key(key); e.value() = "test1";
    EQ(e.value().size(), 5);

    EQ(c1.db_size(), 0);
    EQ(c2.db_size(), 0);

    c1.add_entry(e);

    this_thread::sleep_for(chrono::milliseconds(50));

    EQ(c1.db_size(), 1);
    EQ(c2.db_size(), 0);

    TEST_PASS
}

TEST(heartbeat) {
    g_idx = 1;
    elga::ZMQAddress db1_addr { "127.0.0.1", g_idx+=inc_amount };
    PandoParticipant db1 { db1_addr, true};
    elga::ZMQAddress db2_addr { "127.0.0.1", g_idx+=inc_amount };
    PandoParticipant db2 { db2_addr, true};
    elga::ZMQAddress db3_addr { "127.0.0.1", g_idx+=inc_amount };
    PandoParticipant db3 { db3_addr, true};

    db1.add_neighbor(db2_addr.serialize(), false);
    db2.add_neighbor(db1_addr.serialize(), false);

    db1.add_neighbor(db3_addr.serialize(), false);
    db3.add_neighbor(db1_addr.serialize(), false);

    EQ(db1.num_neighbors(), 3);
    EQ(db2.num_neighbors(), 2);
    EQ(db3.num_neighbors(), 2);
    this_thread::sleep_for(chrono::milliseconds(1000));
    EQ(db1.num_neighbors(), 3);
    EQ(db2.num_neighbors(), 2);
    EQ(db3.num_neighbors(), 2);

    db1.recv_msg();
    this_thread::sleep_for(chrono::milliseconds(50));
    EQ(db1.num_neighbors(), 3);
    EQ(db2.num_neighbors(), 2);
    EQ(db3.num_neighbors(), 2);

    db2.recv_msg();
    db3.recv_msg();

    EQ(db1.num_neighbors(), 3);
    EQ(db2.num_neighbors(), 3);
    EQ(db3.num_neighbors(), 3);

    TEST_PASS
}

TEST(proxy) {
    g_idx = 0;      // If this test fails, determine if 0 is not free

    elga::ZMQAddress db1_addr { "127.0.0.1", g_idx+=inc_amount };
    ParDBThread db1 { db1_addr };
    ParDBClient c1 { db1_addr };

    elga::ZMQAddress db2_addr { "127.0.0.1", g_idx+=inc_amount };
    ParDBThread db2 { db2_addr };
    ParDBClient c2 { db2_addr };

    elga::ZMQAddress db3_addr { "127.0.0.1", g_idx+=inc_amount };
    ParDBThread db3 { db3_addr };
    ParDBClient c3 { db3_addr };

    c1.add_neighbor(db2_addr.serialize());
    c1.add_neighbor(db3_addr.serialize());

    this_thread::sleep_for(chrono::milliseconds(50));

    EQ(c1.num_neighbors(), 3);
    EQ(c2.num_neighbors(), 2);
    EQ(c3.num_neighbors(), 3);

    this_thread::sleep_for(chrono::milliseconds(1050));

    EQ(c1.num_neighbors(), 3);
    EQ(c2.num_neighbors(), 3);
    EQ(c3.num_neighbors(), 3);

    EQ(c1.db_size(), 0);
    EQ(c2.db_size(), 0);
    EQ(c3.db_size(), 0);

    // Now, create and join a proxy
    elga::ZMQAddress p1_addr { "127.0.0.1", g_idx+=inc_amount };
    ParDBThread<PandoProxy> p1 { p1_addr };
    ParDBClient pc1 { p1_addr };

    pc1.add_neighbor(db2_addr.serialize());

    EQ(pc1.num_neighbors(), 1);     // NOTE: slight chance this will be 3, depending on receiving a heartbeat from db1 already
    EQ(c1.num_neighbors(), 3);
    EQ(c2.num_neighbors(), 3);
    EQ(c3.num_neighbors(), 3);

    this_thread::sleep_for(chrono::milliseconds(1050));

    EQ(pc1.num_neighbors(), 3);
    EQ(c1.num_neighbors(), 3);
    EQ(c2.num_neighbors(), 3);
    EQ(c3.num_neighbors(), 3);

    dbkey_t key {1,2,4};
    DBEntry<> e; e.set_key(key); e.value() = "test1";
    pc1.add_entry(e);

    this_thread::sleep_for(chrono::milliseconds(50));

    EQ(c1.db_size(), 1);
    EQ(c2.db_size(), 0);
    EQ(c3.db_size(), 0);

    TEST_PASS
}

TEST(db_file) {
    g_idx = 14;

    elga::ZMQAddress db1_addr { "127.0.0.1", g_idx+=inc_amount };
    ParDBThread db1 { db1_addr };
    ParDBClient c1 { db1_addr };

    elga::ZMQAddress db2_addr { "127.0.0.1", g_idx+=inc_amount };
    ParDBThread db2 { db2_addr };
    ParDBClient c2 { db2_addr };

    c1.add_neighbor(db2_addr);
    this_thread::sleep_for(chrono::milliseconds(50));
    EQ(c1.num_neighbors(), 2);
    EQ(c2.num_neighbors(), 2);

    c1.add_db_file(data_dir + "/simple_bitcoin.txt");
    this_thread::sleep_for(chrono::milliseconds(1000));

    EQ(c1.db_size() + c2.db_size(), 2);

    TEST_PASS
}

TEST(remove_tag_from_entry) {
    //create db
    elga::ZMQAddress db1_addr { "127.0.0.1", g_idx+=inc_amount };
    ParDB db1 { db1_addr };
    EQ(db1.db_size(), 0);

    //create entry for db
    DBEntry<> e;
    dbkey_t key {1,2,3};
    e.clear().add_tag("A", "B", "C");
    e.set_key(key);
    db1.add_entry(move(e));
    db1.recv_msg();
    EQ(db1.db_size(), 1);

    //remove the tag
    db1.remove_tag_from_entry(key, "C");
    EQ(db1.entries().size(), 1);
    db1.recv_msg();

    //validate the DB
    bool correct = false;
    for (auto & [key, entry] : db1.entries()) {
        if (entry.has_tag("A") && entry.has_tag("B")) {
            if (!entry.has_tag("C"))
                correct = true;
        }

    }

    EQ(correct, true);

    TEST_PASS
}

TEST(remove_tag_from_entry_multiple) {
    g_idx = 0;      // If this test fails, determine if 0 is not free
    elga::ZMQAddress db1_addr { "127.0.0.1", g_idx+=inc_amount };
    ParDB db1 { db1_addr };

    elga::ZMQAddress db2_addr { "127.0.0.1", g_idx+=inc_amount };
    ParDB db2 { db2_addr };

    db1.add_neighbor(db2_addr.serialize());

    db2.recv_msg();

    EQ(db1.num_neighbors(), 2);
    EQ(db2.num_neighbors(), 2);

    dbkey_t key {1,2,4};
    DBEntry<> e; e.set_key(key); e.value() = "test1"; e.add_tag("A", "B", "C");
    db1.add_entry(move(e));

    this_thread::sleep_for(chrono::milliseconds(50));

    EQ(db1.db_size(), 0);
    EQ(db2.db_size(), 0);

    db1.recv_msg();
    db2.recv_msg();

    EQ(db1.db_size(), 1);
    EQ(db2.db_size(), 0);

    //remove the tag
    db2.remove_tag_from_entry(key, "C");

    db1.recv_msg();
    db2.recv_msg();

    //validate the DB
    bool correct = false;
    for (auto & [key, entry] : db1.entries()) {
        if (entry.has_tag("A") && entry.has_tag("B")) {
            if (!entry.has_tag("C"))
                correct = true;
        }

    }

    EQ(correct, true);

    TEST_PASS
}

TEST(update_entry_val) {
    elga::ZMQAddress db1_addr { "127.0.0.1", g_idx+=inc_amount };
    ParDB db1 { db1_addr };
    EQ(db1.db_size(), 0);

    dbkey_t key {1,2,3};
    DBEntry<> e; e.set_key(key); e.value() = "test1"; db1.add_entry(move(e));

    this_thread::sleep_for(chrono::milliseconds(50));
    EQ(db1.db_size(), 0);
    db1.recv_msg();
    EQ(db1.db_size(), 1);

    string new_val = "test2";
    db1.update_entry_val(key, new_val);
    db1.recv_msg();

    EQ(db1.db_size(), 1);
    bool correct = false;
    for (auto & [key, entry] : db1.entries()) {
        if (string{entry.value()} != new_val) {
            TEST_FAIL
        } else {
            EQ(correct, false);
            correct = true;
        }
    }

    EQ(correct, true);

    TEST_PASS
}

TEST(update_entry_val_multiple) {
    g_idx = 0;      // If this test fails, determine if 0 is not free
    elga::ZMQAddress db1_addr { "127.0.0.1", g_idx+=inc_amount };
    ParDB db1 { db1_addr };

    elga::ZMQAddress db2_addr { "127.0.0.1", g_idx+=inc_amount };
    ParDB db2 { db2_addr };

    db1.add_neighbor(db2_addr.serialize());

    db2.recv_msg();

    EQ(db1.num_neighbors(), 2);
    EQ(db2.num_neighbors(), 2);

    dbkey_t key {1,2,4};
    DBEntry<> e; e.set_key(key); e.value() = "test1"; e.add_tag("A", "B", "C");
    db1.add_entry(move(e));

    this_thread::sleep_for(chrono::milliseconds(50));

    EQ(db1.db_size(), 0);
    EQ(db2.db_size(), 0);

    db1.recv_msg();
    db2.recv_msg();

    EQ(db1.db_size(), 1);
    EQ(db2.db_size(), 0);

    //update the value
    string new_val = "test2";
    db2.update_entry_val(key, new_val);

    db1.recv_msg();
    db2.recv_msg();

    EQ(db1.db_size(), 1);
    EQ(db2.db_size(), 0);

    bool correct = false;
    for (auto & [key, entry] : db1.entries()) {
        if (string{entry.value()} != new_val) {
            TEST_FAIL
        } else {
            EQ(correct, false);
            correct = true;
        }
    }

    EQ(correct, true);

    TEST_PASS
}

TEST(subscribe_to_entry) {
    elga::ZMQAddress db1_addr { "127.0.0.1", g_idx+=inc_amount };
    ParDB db1 { db1_addr };
    EQ(db1.db_size(), 0);

    DBEntry<> my_entry;
    dbkey_t my_key = {0,0,0};
    my_entry.set_key(my_key);
    my_entry.value() = "my entry";
    my_entry.add_tag("foo:inactive");
    db1.add_entry(move(my_entry));
    this_thread::sleep_for(chrono::milliseconds(50));
    db1.recv_msg();
    EQ(db1.db_size(), 1);

    DBEntry<> wait_entry;
    dbkey_t wait_key = {1,1,1};
    wait_entry.set_key(wait_key);
    wait_entry.value() = "wait entry";

    db1.subscribe_to_entry(my_key, wait_key, "foo:inactive");
    this_thread::sleep_for(chrono::milliseconds(50));
    db1.recv_msg();
    EQ(db1.db_size(), 1);


    db1.add_entry(move(wait_entry));
    this_thread::sleep_for(chrono::milliseconds(50));
    db1.recv_msg();
    db1.recv_msg();

    // Need to process to trigger the effects of subscription
    db1.broadcast_process();
    for (size_t i = 0; i < 10; i++)
        db1.recv_msg();

    EQ(db1.db_size(), 2);

    bool entry_found = false;
    for (auto& [key, entry] : db1.entries()) {
        if (key == my_key) {
            EQ(entry_found, false);
            entry_found = true;
            EQ(entry.has_tag("foo:inactive"), false);
        }
    }
    EQ(entry_found, true);

    TEST_PASS
}

TEST(subscribe_to_entry_multiple) {
    g_idx = 10;      // If this test fails, determine if 0 is not free
    elga::ZMQAddress db1_addr { "127.0.0.1", g_idx+=inc_amount };
    ParDB db1 { db1_addr };

    elga::ZMQAddress db2_addr { "127.0.0.1", g_idx+=inc_amount };
    ParDB db2 { db2_addr };

    db1.add_neighbor(db2_addr.serialize());

    db2.recv_msg();

    EQ(db1.num_neighbors(), 2);
    EQ(db2.num_neighbors(), 2);

    dbkey_t my_key {1,2,4};
    DBEntry<> e1; e1.set_key(my_key); e1.value() = "test1";
    e1.add_tag("foo:inactive");
    db1.add_entry(move(e1));
    this_thread::sleep_for(chrono::milliseconds(50));

    db1.recv_msg();
    db2.recv_msg();

    EQ(db1.db_size(), 0);
    EQ(db2.db_size(), 1);

    dbkey_t wait_key {2,2,4};
    DBEntry<> e2; e2.set_key(wait_key); e2.value() = "test2";
    
    EQ(db1.db_size(), 0);
    EQ(db2.db_size(), 1);

    db1.subscribe_to_entry(my_key, wait_key, "foo:inactive");
    this_thread::sleep_for(chrono::milliseconds(50));

    db1.recv_msg();
    db2.recv_msg();

    db1.add_entry(move(e2));
    this_thread::sleep_for(chrono::milliseconds(50));

    db1.recv_msg();
    db2.recv_msg();

    // Need to process to trigger the effects of subscription
    db1.broadcast_process();
    for (size_t i = 0; i < 20; i++) {
        db1.recv_msg();
        db2.recv_msg();
    }

    EQ(db2.db_size(), 1);
    EQ(db1.db_size(), 1);

    bool entry_found = false;
    for (auto& [key, entry] : db2.entries()) {
        if (key == my_key) {
            EQ(entry_found, false);
            entry_found = true;
            EQ(entry.has_tag("foo:inactive"), false);
        }
    }
    EQ(entry_found, true);

    TEST_PASS
}

TEST(get_entry_by_key) {
    elga::ZMQAddress db1_addr { "127.0.0.1", g_idx+=inc_amount };
    ParDB db1 { db1_addr };
    EQ(db1.db_size(), 0);


    //Add first entry
    DBEntry<> entry1;
    dbkey_t key1 = {0,0,0};
    entry1.set_key(key1);
    entry1.value() = "test1";
    db1.add_entry(move(entry1));
    this_thread::sleep_for(chrono::milliseconds(50));
    db1.recv_msg();
    EQ(db1.db_size(), 1);


    //Add second entry
    DBEntry<> entry2;
    dbkey_t key2 = {1,1,1};
    entry2.set_key(key2);
    entry2.value() = "test2";
    db1.add_entry(move(entry2));
    this_thread::sleep_for(chrono::milliseconds(50));
    db1.recv_msg();

    EQ(db1.db_size(), 2);

    char* ret = nullptr;
    string ret_s;
    db1.get_entry_by_key(key1, &ret);
    if (ret != nullptr) {
        ret_s.assign(ret);
        free(ret);
    } else {
        throw runtime_error("Entry not found");
    }
    EQ(ret_s, "test1");


    TEST_PASS
}

TEST(get_entry_by_key_multiple) {
    g_idx = 6;

    elga::ZMQAddress db1_addr { "127.0.0.1", g_idx+=inc_amount };
    ParDB db1 { db1_addr };

    elga::ZMQAddress db2_addr { "127.0.0.1", g_idx+=inc_amount };
    ParDB db2 { db2_addr };

    db1.add_neighbor(db2_addr.serialize());
    db2.recv_msg();

    EQ(db1.num_neighbors(), 2);
    EQ(db2.num_neighbors(), 2);


    //Add first entry
    DBEntry<> entry1;
    dbkey_t key1 = {0,0,0};
    entry1.set_key(key1);
    entry1.value() = "test1";
    db1.add_entry(move(entry1));
    this_thread::sleep_for(chrono::milliseconds(50));
    db1.recv_msg();

    EQ(db1.db_size(), 1);
    EQ(db2.db_size(), 0);

    char* ret = nullptr;
    string ret_s;
    db2.get_entry_by_key(key1, &ret);
    if (ret != nullptr) {
        ret_s.assign(ret);
        free(ret);
    } else {
        throw runtime_error("Entry not found");
    }
    EQ(ret_s, "test1");


    TEST_PASS
}

TEST(query_entries) {
    elga::ZMQAddress db1_addr { "127.0.0.1", g_idx+=inc_amount };
    ParDBThread<ParDB> db1 { db1_addr };
    ParDBClient c1 { db1_addr };

    { DBEntry<> e; dbkey_t key {0,0,1}; e.set_key(key); e.add_tag("BTC", "A"); e.value() = "test1"; c1.add_entry(e); }
    { DBEntry<> e; dbkey_t key {0,0,2}; e.set_key(key); e.add_tag("BTC", "B"); e.value() = "test2"; c1.add_entry(e); }
    { DBEntry<> e; dbkey_t key {0,0,3}; e.set_key(key); e.add_tag("BTC", "C"); e.value() = "test3"; c1.add_entry(e); }
    { DBEntry<> e; dbkey_t key {0,0,4}; e.set_key(key); e.add_tag("BTC", "A"); e.value() = "test4"; c1.add_entry(e); }

    EQ(c1.db_size(), 4);

    auto a_entries = c1.query("A");
    EQ(a_entries.size(), 2);

    bool test1_found = false;
    bool test4_found = false;
    for (auto & entry : a_entries) {
        if (entry.value() == "test1") {
            EQ(test1_found, false);
            test1_found = true;
        } else if (entry.value() == "test4") {
            EQ(test4_found, false);
            test4_found = true;
        } else {
            TEST_FAIL
        }
    }

    EQ(test1_found, true);
    EQ(test4_found, true);
    EQ(c1.db_size(), 4);

    TEST_PASS

}

TEST(get_neighbors) {
    g_idx = 0;

    elga::ZMQAddress db1_addr { "127.0.0.1", g_idx+=inc_amount };
    ParDBThread<ParDB> db1 { db1_addr };
    ParDBClient c1 { db1_addr };

    elga::ZMQAddress db2_addr { "127.0.0.1", g_idx+=inc_amount };
    ParDBThread<ParDB> db2 { db2_addr };
    ParDBClient c2 { db2_addr };

    elga::ZMQAddress db3_addr { "127.0.0.1", g_idx+=inc_amount };
    ParDBThread<ParDB> db3 { db3_addr };
    ParDBClient c3 { db3_addr };

    c1.add_neighbor(db2_addr);
    c1.add_neighbor(db3_addr);

    this_thread::sleep_for(chrono::milliseconds(50));
    EQ(c1.num_neighbors(), 3);

    bool db1_found = false;
    bool db2_found = false;
    bool db3_found = false;

    for (auto & addr : c1.neighbors()) {
        if (addr == "127.0.0.1,3") {
            EQ(db1_found, false);
            db1_found = true;
        } else if (addr == "127.0.0.1,6") {
            EQ(db2_found, false);
            db2_found = true;
        } else if (addr == "127.0.0.1,9") {
            EQ(db3_found, false);
            db3_found = true;
        } else {
            TEST_FAIL
        }
    }

    EQ(db1_found, true);
    EQ(db2_found, true);
    EQ(db3_found, true);

    TEST_PASS
}

TEST(processing) {
    elga::ZMQAddress db1_addr { "127.0.0.1", g_idx+=inc_amount };
    ParDBThread<ParDB> db1 { db1_addr };
    ParDBClient c1 { db1_addr };

    elga::ZMQAddress db2_addr { "127.0.0.1", g_idx+=inc_amount };
    ParDBThread<ParDB> db2 { db2_addr };
    ParDBClient c2 { db2_addr };

    vector<ZMQAddress> addrs;
    addrs.push_back(db1_addr);
    addrs.push_back(db2_addr);

    c1.add_neighbor(db2_addr);
    this_thread::sleep_for(chrono::milliseconds(50));
    EQ(c1.num_neighbors(), 2);

    size_t num_its = 100;
    for (size_t i = 0; i < num_its; i++) {
        // Let's add this multiple times so processing is a bit slower and we can test the "is processing" functionality
        c1.add_db_file(data_dir + "/simple_bitcoin.txt");
    }
    while (c1.db_size() + c2.db_size() != 200) {
        this_thread::sleep_for(chrono::milliseconds(250));
    }

    c1.add_filter_dir(build_dir+"/filters");
    c1.install_filter("BTC_block_to_tx");

    c1.process();

    wait(addrs);

    TEST_PASS
    
}

TEST(import_export) {
    dbkey_t set_key {1,2,3};
    set<dbkey_t> orig_keys;
    { 
        elga::ZMQAddress db1_addr { "127.0.0.1", g_idx+=inc_amount };
        ParDBThread<ParDB> db1 { db1_addr };
        ParDBClient c1 { db1_addr };

        elga::ZMQAddress db2_addr { "127.0.0.1", g_idx+=inc_amount };
        ParDBThread<ParDB> db2 { db2_addr };
        ParDBClient c2 { db2_addr };

        c1.add_neighbor(db2_addr);
        this_thread::sleep_for(chrono::milliseconds(50));
        EQ(c1.num_neighbors(), 2);

        c1.add_db_file(data_dir + "/simple_bitcoin.txt");
        this_thread::sleep_for(chrono::milliseconds(50));

        EQ(c1.db_size() + c2.db_size(), 2);

        DBEntry<> e; e.add_tag("a", "b").value() = "res "; e.set_key(set_key); c1.add_entry(e);
        c1.add_entry(e);
        this_thread::sleep_for(chrono::milliseconds(50));

        EQ(c1.db_size() + c2.db_size(), 3);

        for (auto & entry : c1.query("BTC"))
            orig_keys.insert(entry.get_key());
        for (auto & entry : c2.query("BTC"))
            orig_keys.insert(entry.get_key());
        orig_keys.insert(set_key);

        mkdir("export", 0777);
        c1.export_db("export");
    }

    // now, create a new set of ParDBs (with a different mesh size) and test the import
    elga::ZMQAddress db1_addr { "127.0.0.1", g_idx+=inc_amount };
    ParDBThread<ParDB> db1 { db1_addr };
    ParDBClient c1 { db1_addr };
    elga::ZMQAddress db2_addr { "127.0.0.1", g_idx+=inc_amount };
    ParDBThread<ParDB> db2 { db2_addr };
    ParDBClient c2 { db2_addr };
    elga::ZMQAddress db3_addr { "127.0.0.1", g_idx+=inc_amount };
    ParDBThread<ParDB> db3 { db3_addr };
    ParDBClient c3 { db3_addr };

    c1.add_neighbor(db2_addr);
    c1.add_neighbor(db3_addr);
    this_thread::sleep_for(chrono::milliseconds(50));
    EQ(c1.num_neighbors(), 3);

    EQ(c1.db_size() + c2.db_size() + c3.db_size(), 0);

    c1.import_db("export");
    this_thread::sleep_for(chrono::milliseconds(50));

    EQ(c1.db_size() + c2.db_size() + c3.db_size(), 3);

    vector<DBEntry<>> entries;
    auto c1_entries = c1.get_entries();
    auto c2_entries = c2.get_entries();
    auto c3_entries = c3.get_entries();
    move(c1_entries.begin(), c1_entries.end(), std::back_inserter(entries));
    move(c2_entries.begin(), c2_entries.end(), std::back_inserter(entries));
    move(c3_entries.begin(), c3_entries.end(), std::back_inserter(entries));

    size_t found_count = 0;
    bool keyed_entry_found = false;
    for (auto & entry : entries) {
        auto key = entry.get_key();
        if (key == set_key) {
            EQ(keyed_entry_found, false);
            EQ(entry.value(), "res ");
            EQ(entry.has_tag("a"), true);
            EQ(entry.has_tag("b"), true);
            EQ(entry.tags().size(), 2);
            keyed_entry_found = true;
        } else if (orig_keys.count(key) == 0) {
            TEST_FAIL
        } else {
            ++found_count;
        }
    }

    EQ(found_count, 2);
    EQ(keyed_entry_found, true);

    std::filesystem::remove_all("export");

    TEST_PASS
}

TEST(query_entries_all_neighbors) {
    g_idx = 0;

    elga::ZMQAddress db1_addr { "127.0.0.1", g_idx+=inc_amount };
    ParDBThread<ParDB> db1 { db1_addr };
    ParDBClient c1 { db1_addr };

    elga::ZMQAddress db2_addr { "127.0.0.1", g_idx+=inc_amount };
    ParDBThread<ParDB> db2 { db2_addr };
    ParDBClient c2 { db2_addr };

    elga::ZMQAddress db3_addr { "127.0.0.1", g_idx+=inc_amount };
    ParDBThread<ParDB> db3 { db3_addr };
    ParDBClient c3 { db3_addr };

    c1.add_neighbor(db2_addr);
    c1.add_neighbor(db3_addr);

    this_thread::sleep_for(chrono::milliseconds(50));
    EQ(c1.num_neighbors(), 3);

    { DBEntry<> e; dbkey_t key {0,5,0}; e.set_key(key); e.add_tag("BTC", "A"); e.value() = "test1"; c1.add_entry(e); }
    { DBEntry<> e; dbkey_t key {0,2,0}; e.set_key(key); e.add_tag("BTC", "B"); e.value() = "test2"; c1.add_entry(e); }
    { DBEntry<> e; dbkey_t key {0,3,0}; e.set_key(key); e.add_tag("BTC", "C"); e.value() = "test3"; c1.add_entry(e); }
    { DBEntry<> e; dbkey_t key {0,4,0}; e.set_key(key); e.add_tag("BTC", "A"); e.value() = "test4"; c1.add_entry(e); }

    EQ(c1.db_size(), 1);
    EQ(c2.db_size(), 1);
    EQ(c3.db_size(), 2);

    //Make sure both "A" entries are not on the same db
    EQ(c1.query("A").size(), 1);
    EQ(c2.query("A").size(), 1);

    auto c1_entries = c1.query("A");
    auto c2_entries = c2.query("A");

    EQ(c1_entries[0].value(), "test1");
    EQ(c2_entries[0].value(), "test4");

    TEST_PASS
}

TEST(query_entries_all_neighbors_after_filter) {
    g_idx = 2;

    elga::ZMQAddress db1_addr { "127.0.0.1", g_idx+=inc_amount };
    ParDBThread<ParDB> db1 { db1_addr };
    ParDBClient c1 { db1_addr };

    elga::ZMQAddress db2_addr { "127.0.0.1", g_idx+=inc_amount };
    ParDBThread<ParDB> db2 { db2_addr };
    ParDBClient c2 { db2_addr };

    c1.add_db_file(data_dir + "/simple_bitcoin.txt");
    c1.add_filter_dir(build_dir+"/filters");
    c1.install_filter("BTC_block_to_tx");

    c1.process();
    this_thread::sleep_for(chrono::milliseconds(2000));

    EQ(c1.query("BTC_block_to_tx:done").size() + c2.query("BTC_block_to_tx:done").size(), 2)


    TEST_PASS
}

TEST(query_multiple_tags) {
    elga::ZMQAddress db1_addr { "127.0.0.1", g_idx+=inc_amount };
    ParDBThread<ParDB> db1 { db1_addr };
    ParDBClient c1 { db1_addr };

    { DBEntry<> e; dbkey_t key {0,0,1}; e.set_key(key); e.add_tag("BTC", "A"); e.value() = "test1"; c1.add_entry(e); }
    { DBEntry<> e; dbkey_t key {0,0,2}; e.set_key(key); e.add_tag("BTC", "B"); e.value() = "test2"; c1.add_entry(e); }
    { DBEntry<> e; dbkey_t key {0,0,3}; e.set_key(key); e.add_tag("ETH", "A"); e.value() = "test3"; c1.add_entry(e); }
    { DBEntry<> e; dbkey_t key {0,0,4}; e.set_key(key); e.add_tag("BTC", "A"); e.value() = "test4"; c1.add_entry(e); }

    EQ(c1.db_size(), 4);

    vector<string> tags {"A", "BTC"};
    auto a_entries = c1.query(tags);
    EQ(a_entries.size(), 2);

    bool test1_found = false;
    bool test4_found = false;
    for (auto & entry : a_entries) {
        if (entry.value() == "test1") {
            EQ(test1_found, false);
            test1_found = true;
        } else if (entry.value() == "test4") {
            EQ(test4_found, false);
            test4_found = true;
        } else {
            TEST_FAIL
        }
    }

    EQ(test1_found, true);
    EQ(test4_found, true);
    EQ(c1.db_size(), 4);

    TEST_PASS

}

TEST(import_txt_db) {
    elga::ZMQAddress db1_addr { "127.0.0.1", g_idx+=inc_amount };
    ParDBThread<ParDB> db1 { db1_addr };
    ParDBClient c1 { db1_addr };

    elga::ZMQAddress db2_addr { "127.0.0.1", g_idx+=inc_amount };
    ParDBThread<ParDB> db2 { db2_addr };
    ParDBClient c2 { db2_addr };

    c1.add_neighbor(db2_addr);
    this_thread::sleep_for(chrono::milliseconds(50));
    EQ(c1.num_neighbors(), 2);
    
    c1.import_db(data_dir);
    this_thread::sleep_for(chrono::milliseconds(100));

    // Every entry in the top-level files of test/data:
    // simple_bitcoin (2) + simple_bnb (2) + simple_ethereum (2) + simple_omni (5)
    // + simple_fastq (1, BQS-only). Update this when files are added there.
    EQ(c1.db_size() + c2.db_size(), 12);


    TEST_PASS
}

TESTS_BEGIN
    elga::ZMQChatterbox::Setup();

    RUN_TEST(reuse_addr)
    RUN_TEST(handshake_twice)
    RUN_TEST(handshake)
    RUN_TEST(serving)
    RUN_TEST(ring_size)
    RUN_TEST(mesh)
    RUN_TEST(forward_add_entry)
    RUN_TEST(heartbeat)
    RUN_TEST(proxy)
    RUN_TEST(db_file)
    RUN_TEST(query_entries)
    RUN_TEST(get_neighbors)
    RUN_TEST(processing)
    RUN_TEST(import_export)
    RUN_TEST(query_entries_all_neighbors)
    RUN_TEST(query_entries_all_neighbors_after_filter)
    RUN_TEST(query_multiple_tags)

    RUN_TEST(add_entry_single)
    RUN_TEST(add_entry_multiple)

    RUN_TEST(remove_tag_from_entry)
    RUN_TEST(remove_tag_from_entry_multiple)

    RUN_TEST(update_entry_val)
    RUN_TEST(update_entry_val_multiple)

    RUN_TEST(subscribe_to_entry)
    RUN_TEST(subscribe_to_entry_multiple)
    RUN_TEST(get_entry_by_key)
    RUN_TEST(get_entry_by_key_multiple)
    RUN_TEST(import_txt_db)


    elga::ZMQChatterbox::Teardown();
TESTS_END
