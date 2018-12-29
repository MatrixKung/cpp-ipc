#include <thread>
#include <vector>
#include <type_traits>
#include <iostream>
#include <shared_mutex>
#include <mutex>
#include <typeinfo>
#include <memory>
#include <string>
#include <cstring>
#include <algorithm>
#include <array>
#include <limits>
#include <utility>

#include "stopwatch.hpp"
#include "spin_lock.hpp"
#include "random.hpp"

#include "ipc.h"
#include "rw_lock.h"

#include "test.h"

namespace {

std::vector<ipc::buff_t> datas__;

constexpr int DataMin   = 2;
constexpr int DataMax   = 256;
constexpr int LoopCount = 100000;

} // internal-linkage

template <typename T>
struct test_verify {
    std::vector<std::vector<ipc::buff_t>> list_;

    test_verify(int M)
        : list_(static_cast<std::size_t>(M))
    {}

    void prepare(void* /*pt*/) {}

    void push_data(int cid, ipc::buff_t const & msg) {
        list_[cid].emplace_back(std::move(msg));
    }

    void verify(int /*N*/, int /*Loops*/) {
        std::cout << "verifying..." << std::endl;
        for (auto& c_dats : list_) {
            QCOMPARE(datas__, c_dats);
        }
    }
};

template <>
struct test_cq<ipc::route> {
    using cn_t = ipc::route;

    std::string conn_name_;

    test_cq(void*)
        : conn_name_("test-ipc-route") {
        ipc::clear_recv(conn_name_.c_str());
    }

    cn_t connect() {
        return cn_t { conn_name_.c_str() };
    }

    void disconnect(cn_t& cn) {
        cn.disconnect();
    }

    void wait_start(int M) {
        auto watcher = ipc::connect(conn_name_.c_str());
        while (ipc::recv_count(watcher) != static_cast<std::size_t>(M)) {
            std::this_thread::yield();
        }
    }

    template <typename F>
    void recv(cn_t& cn, F&& proc) {
        do {
            auto msg = cn.recv();
            if (msg.size() < 2) {
                QCOMPARE(msg, ipc::buff_t { '\0' });
                return;
            }
            proc(msg);
        } while(1);
    }

    cn_t connect_send() {
        return connect();
    }

    void send(cn_t& cn, const std::array<int, 2>& info) {
        int n = info[1];
        if (n < 0) {
            /*QVERIFY*/(cn.send(ipc::buff_t { '\0' }));
        }
        else /*QVERIFY*/(cn.send(datas__[static_cast<decltype(datas__)::size_type>(n)]));
    }
};

template <>
struct test_cq<ipc::channel> {
    using cn_t = ipc::channel;

    std::string conn_name_;
    int m_ = 0;
    std::vector<cn_t*> s_cns_;
    ipc::rw_lock lc_;

    test_cq(void*)
        : conn_name_("test-ipc-channel") {
    }

    ~test_cq() {
        for (auto p : s_cns_) delete p;
    }

    cn_t connect() {
        return cn_t { conn_name_.c_str() };
    }

    void disconnect(cn_t& cn) {
        cn.disconnect();
    }

    void wait_start(int M) { m_ = M; }

    template <typename F>
    void recv(cn_t& cn, F&& proc) {
        do {
            auto msg = cn.recv();
            if (msg.size() < 2) {
                QCOMPARE(msg, ipc::buff_t { '\0' });
                return;
            }
            proc(msg);
        } while(1);
    }

    cn_t* connect_send() {
        auto p = new cn_t { conn_name_.c_str() };
        {
            std::unique_lock<ipc::rw_lock> guard { lc_ };
            s_cns_.push_back(p);
        }
        return p;
    }

    void send(cn_t* cn, const std::array<int, 2>& info) {
        thread_local struct s_dummy {
            s_dummy(cn_t* cn, int m) {
                cn->wait_for_recv(static_cast<std::size_t>(m));
//                std::printf("start to send: %d.\n", m);
            }
        } _(cn, m_);
        int n = info[1];
        if (n < 0) {
            /*QVERIFY*/(cn->send(ipc::buff_t { '\0' }));
        }
        else /*QVERIFY*/(cn->send(datas__[static_cast<decltype(datas__)::size_type>(n)]));
    }
};

namespace {

class Unit : public TestSuite {
    Q_OBJECT

    const char* name() const {
        return "test_ipc";
    }

private slots:
    void initTestCase();
    void cleanupTestCase();

    void test_rw_lock();
    void test_send_recv();
    void test_route();
    void test_route_rtt();
    void test_route_performance();
    void test_channel();
    void test_channel_rtt();
    void test_channel_performance();
} unit__;

#include "test_ipc.moc"

void Unit::initTestCase() {
    TestSuite::initTestCase();

    capo::random<> rdm { DataMin, DataMax };
    capo::random<> bit { 0, (std::numeric_limits<ipc::buff_t::value_type>::max)() };

    for (int i = 0; i < LoopCount; ++i) {
        auto n = rdm();
        ipc::buff_t buff(static_cast<ipc::buff_t::size_type>(n));
        for (std::size_t k = 0; k < buff.size(); ++k) {
            buff[k] = static_cast<ipc::buff_t::value_type>(bit());
        }
        datas__.emplace_back(std::move(buff));
    }
}

void Unit::cleanupTestCase() {
    datas__.clear();
}

template <typename T>
constexpr T acc(T b, T e) {
    return (e + b) * (e - b + 1) / 2;
}

template <typename Mutex>
struct lc_wrapper : Mutex {
    void lock_shared  () { Mutex::lock  (); }
    void unlock_shared() { Mutex::unlock(); }
};

template <typename Lc, int W, int R, int Loops = LoopCount>
void benchmark_lc() {
    std::thread w_trd[W];
    std::thread r_trd[R];
    std::atomic_int fini { 0 };
//    std::atomic_bool wf { false };

    std::vector<int> datas;
    Lc lc;

    test_stopwatch sw;
    std::cout << std::endl << type_name<Lc>() << std::endl;

    for (auto& t : r_trd) {
        t = std::thread([&] {
            std::vector<int> seq;
            std::size_t cnt = 0;
            while (1) {
                int x = -1;
                {
                    std::shared_lock<Lc> guard { lc };
//                    QVERIFY(!wf);
                    if (cnt < datas.size()) {
                        x = datas[cnt];
                    }
//                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    if (x ==  0) break; // quit
                    if (x != -1) {
                        seq.push_back(x);
                        ++cnt;
                    }
                }
                std::this_thread::yield();
            }
            if (++fini == std::extent<decltype(r_trd)>::value) {
                sw.print_elapsed(W, R, Loops);
            }
            std::uint64_t sum = 0;
            for (int i : seq) sum += static_cast<std::uint64_t>(i);
            QCOMPARE(sum, acc<std::uint64_t>(1, Loops) * std::extent<decltype(w_trd)>::value);
        });
    }

    for (auto& t : w_trd) {
        t = std::thread([&] {
            sw.start();
            for (int i = 1; i <= Loops; ++i) {
                {
                    std::unique_lock<Lc> guard { lc };
//                    wf = true;
                    datas.push_back(i);
//                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
//                    wf = false;
                }
                std::this_thread::yield();
            }
        });
    }

    for (auto& t : w_trd) t.join();
    lc.lock();
    datas.push_back(0);
    lc.unlock();
    for (auto& t : r_trd) t.join();
}

template <int W, int R>
void test_lock_performance() {

    std::cout << std::endl
              << "test_lock_performance: [" << W << ":" << R << "]"
              << std::endl;

    benchmark_lc<ipc::rw_lock               , W, R>();
    benchmark_lc<lc_wrapper<capo::spin_lock>, W, R>();
    benchmark_lc<lc_wrapper<std::mutex>     , W, R>();
    benchmark_lc<std::shared_timed_mutex    , W, R>();
}

void Unit::test_rw_lock() {
//    test_lock_performance<1, 1>();
//    test_lock_performance<4, 4>();
//    test_lock_performance<1, 8>();
//    test_lock_performance<8, 1>();
}

void Unit::test_send_recv() {
    auto h = ipc::connect("my-ipc");
    QVERIFY(h != nullptr);
    ipc::clear_recv(h);
    char data[] = "hello ipc!";
    std::thread xx([h, data] {
        auto got = ipc::recv(h);
        QCOMPARE((char*)got.data(), data);
    });
    while (!ipc::send(h, data, sizeof(data))) {
        std::this_thread::yield();
    }
    xx.join();
    ipc::disconnect(h);
}

template <typename T, int N, int M, bool V = true, int Loops = LoopCount>
void test_prod_cons() {
    benchmark_prod_cons<N, M, Loops, std::conditional_t<V, T, void>>((T*)nullptr);
}

void Unit::test_route() {
    ipc::clear_recv("my-ipc-route");

    std::vector<char const *> const datas = {
        "hello!",
        "foo",
        "bar",
        "ISO/IEC",
        "14882:2011",
        "ISO/IEC 14882:2017 Information technology - Programming languages - C++",
        "ISO/IEC 14882:2020",
        "Modern C++ Design: Generic Programming and Design Patterns Applied"
    };

    std::thread t1 {[&] {
        ipc::route cc { "my-ipc-route" };
        for (std::size_t i = 0; i < datas.size(); ++i) {
            ipc::buff_t dd = cc.recv();
            QCOMPARE(dd.size(), std::strlen(datas[i]) + 1);
            QVERIFY(std::memcmp(dd.data(), datas[i], dd.size()) == 0);
        }
    }};

    std::thread t2 {[&] {
        ipc::route cc { "my-ipc-route" };
        while (cc.recv_count() == 0) {
            std::this_thread::yield();
        }
        for (std::size_t i = 0; i < datas.size(); ++i) {
            std::cout << "sending: " << datas[i] << std::endl;
            QVERIFY(cc.send(datas[i]));
        }
    }};

    t1.join();
    t2.join();

    test_prod_cons<ipc::route, 1, 1>();
}

void Unit::test_route_rtt() {
    test_stopwatch sw;

    std::thread t1 {[&] {
        ipc::route cc { "my-ipc-route-1" };
        ipc::route cr { "my-ipc-route-2" };
        for (std::size_t i = 0;; ++i) {
            auto dd = cc.recv();
            if (dd.size() < 2) return;
//            std::cout << "recving: " << i << "-[" << dd.size() << "]" << std::endl;
            while (!cr.send(ipc::buff_t { 'a' })) {
                std::this_thread::yield();
            }
        }
    }};

    std::thread t2 {[&] {
        ipc::route cc { "my-ipc-route-1" };
        ipc::route cr { "my-ipc-route-2" };
        while (cc.recv_count() == 0) {
            std::this_thread::yield();
        }
        sw.start();
        for (std::size_t i = 0; i < LoopCount; ++i) {
//            std::cout << "sending: " << i << "-[" << datas__[i].size() << "]" << std::endl;
            cc.send(datas__[i]);
            /*auto dd = */cr.recv();
//            if (dd.size() != 1 || dd[0] != 'a') {
//                QVERIFY(false);
//            }
        }
        cc.send(ipc::buff_t { '\0' });
        t1.join();
        sw.print_elapsed(1, 1, LoopCount);
    }};

    t2.join();
}

template <int P, int C, bool V = false>
struct test_performance {
    template <typename T = ipc::route>
    static void start() {
        test_performance<P - 1, C - 1, V>::template start<T>();
        test_prod_cons<T, P, C, V>();
    }
};

template <int C, bool V>
struct test_performance<1, C, V> {
    template <typename T = ipc::route>
    static void start() {
        test_performance<1, C - 1, V>::template start<T>();
        test_prod_cons<T, 1, C, V>();
    }
};

template <int P, bool V>
struct test_performance<P, 1, V> {
    template <typename T = ipc::route>
    static void start() {
        test_performance<P - 1, 1, V>::template start<T>();
        test_prod_cons<T, P, 1, V>();
    }
};

template <bool V>
struct test_performance<1, 1, V> {
    template <typename T = ipc::route>
    static void start() {
        test_prod_cons<T, 1, 1, V>();
    }
};

void Unit::test_route_performance() {
    test_performance<1, 10, true>::start();
}

void Unit::test_channel() {
    std::thread t1 {[&] {
        ipc::channel cc { "my-ipc-channel" };
        for (std::size_t i = 0;; ++i) {
            ipc::buff_t dd = cc.recv();
            if (dd.size() < 2) return;
            QCOMPARE(dd, datas__[i]);
        }
    }};

    std::thread t2 {[&] {
        ipc::channel cc { "my-ipc-channel" };
        cc.wait_for_recv(1);
        for (std::size_t i = 0; i < (std::min)(100, LoopCount); ++i) {
            std::cout << "sending: " << i << "-[" << datas__[i].size() << "]" << std::endl;
            cc.send(datas__[i]);
        }
        cc.send(ipc::buff_t { '\0' });
        t1.join();
    }};

    t2.join();
}

void Unit::test_channel_rtt() {
    test_stopwatch sw;

    std::thread t1 {[&] {
        ipc::channel cc { "my-ipc-channel" };
        for (std::size_t i = 0;; ++i) {
            auto dd = cc.recv();
            if (dd.size() < 2) return;
//            std::cout << "recving: " << i << "-[" << dd.size() << "]" << std::endl;
            while (!cc.send(ipc::buff_t { 'a' })) {
                cc.wait_for_recv(1);
            }
        }
    }};

    std::thread t2 {[&] {
        ipc::channel cc { "my-ipc-channel" };
        cc.wait_for_recv(1);
        sw.start();
        for (std::size_t i = 0; i < LoopCount; ++i) {
//            std::cout << "sending: " << i << "-[" << datas__[i].size() << "]" << std::endl;
            cc.send(datas__[i]);
            /*auto dd = */cc.recv();
//            if (dd.size() != 1 || dd[0] != 'a') {
//                QVERIFY(false);
//            }
        }
        cc.send(ipc::buff_t { '\0' });
        t1.join();
        sw.print_elapsed(1, 1, LoopCount);
    }};

    t2.join();
}

void Unit::test_channel_performance() {
    test_performance<1 , 10>::start<ipc::channel>();
    test_performance<10, 1 >::start<ipc::channel>();
    test_performance<10, 10>::start<ipc::channel>();
}

} // internal-linkage
