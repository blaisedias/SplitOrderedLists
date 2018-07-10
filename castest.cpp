/*

Copyright (C) 2018  Blaise Dias

This file is part of sqzbsrv.

sqzbsrv is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

sqzbsrv is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this file.  If not, see <http://www.gnu.org/licenses/>.

Simple noddy tests to check my understandings of memory models with atomic
operations.
Needs further work, checking for now so that it doesn't get lost.
*/
#include "mark_ptr_type.hpp"
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>
#include <chrono>

using   benedias::concurrent::mark_ptr_type;
using namespace std::chrono_literals;

struct  B
{
    mark_ptr_type<B>  next;
    int v=-1;
    std::thread::id  tid;
    bool found = false;
    bool deleted = false;
    explicit B():tid(std::this_thread::get_id())
    {
#if 0
        std::cout << "CTOR B " << this << " " << v << " " << tid <<  std::endl;
#endif
    }
    ~B()
    {
#if 0
#if 0
        // :-( MSAN generates a fault for this, but not the equivalent 
        // sequence of statements below.
        // This is because MSAN requires all libraries linked against,
        // including libc to be built with --fsanitize=memory.
        // For now we will evade that issue, MSAN has been useful.
        indent(); std::cout << "DTOR B " << this << " " << v << std::endl;
#else
        std::cout << "DTOR B ";
        std::cout << this;
        std::cout << " " << v;
        std::cout << " " << tid;
        std::cout << std::endl;
#endif
#endif
    }

    friend std::ostream& operator << (std::ostream& ostream, const B& b)
    {
        ostream << b.v << " (" << &b << ") " << b.tid;
        return ostream;
    }
};

constexpr   std::size_t num_nodes = 50;
constexpr   std::size_t num_threads = 32;
constexpr   std::size_t num_tests = 2;

class Rendezvous
{
    private:
    // at rendezvous each thread calls ready (++num_ready), the waits for go == true
    int     num_ready = 0;
    // each threads increments after go == true, then executes task
    // and decrements when task complete
    int     in_task_count = 0;
    // at the end each thread calls complete (++num_complete)
    int     num_complete = 0;
    public:
    volatile bool       go=false;

    Rendezvous()=default;
    inline void ready() { __atomic_add_fetch(&num_ready, 1, __ATOMIC_SEQ_CST);}
    inline int  ready_count() { return __atomic_load_n(&num_ready, __ATOMIC_SEQ_CST);}
    inline void start_task() { __atomic_add_fetch(&in_task_count, 1, __ATOMIC_SEQ_CST);}
    inline void end_task() { __atomic_sub_fetch(&in_task_count, 1, __ATOMIC_SEQ_CST);}
    inline int  task_count() { return __atomic_load_n(&in_task_count, __ATOMIC_SEQ_CST);}
    inline void complete() { __atomic_add_fetch(&num_complete, 1, __ATOMIC_SEQ_CST);}
    inline int  complete_count() { return __atomic_load_n(&num_complete, __ATOMIC_SEQ_CST);}
};

typedef Rendezvous  * RendezvousPtr;
typedef RendezvousPtr  RendezvousPtrBlock[num_tests];

struct  test_thread_args
{
    typedef void (*testfunc)(B&, test_thread_args&, Rendezvous&);
    std::thread::id tid;
    bool tid_set = false;
    B b[num_nodes];
    std::size_t cas_count=0;
    testfunc        testfuncs[num_tests];
    std::size_t     cas_counts[num_tests]={};
};

void test_0(B& head, test_thread_args& args, Rendezvous& rndvz)
{
    rndvz.ready();
    while(!rndvz.go)
    {
        std::this_thread::yield();
    }
    rndvz.start_task();
    // push so if we want ascending numbers and addresses
    // reverse traverse the array
    // also maximum contention on insert as all threads are inserting at the head.
    for (int x=num_nodes - 1; x >= 0; --x)
    {
        do
        {
            args.b[x].next = head.next();
            // introduce contention
            std::this_thread::sleep_for(1ms);
            ++args.cas_count;
        }while(!head.next.CAS(args.b[x].next(), args.b+x));
    }
    rndvz.end_task();
    while(0 != rndvz.task_count())
    {
        std::this_thread::sleep_for(1ms);
    }

    unsigned x=0;
    int v = -1;
    for(auto b=head.next(); nullptr != b; b=b->next())
    {
        if (b->tid == std::this_thread::get_id())
        {
            b->found = true;
            if (b->v <= v)
            {
                std::cout << b->v << " " << v << std::endl;
            }
            assert(b->v > v);
            v = b->v;
        }
        if (x > num_threads * num_nodes)
        {
            std::cout << "********** Aborting scan *** " << std::endl;
        }
        ++x;
    }

    // Check that all found flags are set and clear the flags,
    for (x=0; x < num_nodes ; ++x)
    {
        if (!args.b[x].found)
        {
            std::cout << "ERROR!!! not found in list" << x << ") " << args.b[x] << std::endl;
            args.b[x].found = false;
        }
    }
    rndvz.complete();
}

void test_1(B& head, test_thread_args& args, Rendezvous& rndvz)
{
    rndvz.ready();
    while(!rndvz.go)
    {
        std::this_thread::yield();
    }
    rndvz.start_task();
    rndvz.end_task();
    rndvz.complete();
}

void test_thread_fn(B& head, test_thread_args& args, RendezvousPtrBlock& rpb)
{
    for (int x=0; x < num_nodes ; ++x)
    {
        args.b[x].v = x;
        args.b[x].tid = std::this_thread::get_id();
    }
    args.tid = std::this_thread::get_id();
    args.tid_set = true;

    for (auto i = 0; i < num_tests; i++)
    {
        args.testfuncs[i](head, args, *rpb[i]);
    }
}

typedef void (*checkfunc)(B&, std::vector<test_thread_args>&, std::vector<std::thread>&);

void check_test_0(B& head, std::vector<test_thread_args>& th_args, std::vector<std::thread>& threads)
{
    unsigned x = 0;
    std::size_t max_list_len = th_args.size() * num_nodes;
    std::size_t interleaves = 0;
    std::thread::id tid = head.next()->tid; 
    for(auto b=head.next(); nullptr != b; b=b->next())
    {
        if (b->tid != tid)
            ++interleaves;
        tid = b->tid;
        // std::cout << *b << ", " << std::endl;
        if (x > max_list_len)
        {
            std::cout << "********** Aborting *** " << std::endl;
        }
    }
    std::cout << "Interleaves " <<  interleaves << " of " << max_list_len << " ";
    std::cout << (interleaves*100)/max_list_len << "%" << std::endl;
    std::size_t cas_count=0;

    for(auto i = 0; i < num_threads; i++)
    {
        cas_count += th_args[i].cas_count;
    }
    std::cout << "CAS counts " << cas_count << ", " << (cas_count*100)/max_list_len << "%" << std::endl;
}

void check_test_1(B& head, std::vector<test_thread_args>& th_args, std::vector<std::thread>& threads)
{
}

int main( int argc, char* argv[] )
{
    std::setlocale(LC_ALL, "en_US.UTF-8");
    std::srand(std::time(nullptr)); // use current time as seed for random generator

    std::vector<test_thread_args> th_args;
    std::vector<std::thread> threads;

    B  head;
    head.v = -2;

    Rendezvous   rendezvous[num_tests];
    RendezvousPtrBlock  rpb;

    for(auto i = 0; i < num_tests; i++)
    {
        rpb[i] = &rendezvous[i];
    }

    test_thread_args::testfunc  testfuncs[num_tests] = {test_0, test_1};
    checkfunc  checkfuncs[num_tests] = {check_test_0, check_test_1};

    for(auto i = 0; i < num_threads; i++)
    {
        th_args.emplace_back(test_thread_args());
        for (auto ii = 0 ; ii < num_tests; ++ii)
        {
            th_args[i].testfuncs[ii] = testfuncs[ii];
        }
    }

    for(auto i = 0; i < th_args.size(); i++)
    {
        threads.emplace_back(std::thread(test_thread_fn, std::ref(head), std::ref(th_args[i]), 
                    std::ref(rpb)));
    }

    std::this_thread::sleep_for(1s);
    
    bool ready = true;
    do
    {
        ready = true;
        for (auto i = 0; i < num_threads; i++)
        {
            ready = ready && th_args[i].tid_set;
        }
        std::this_thread::yield();
    }while(!ready);

    std::cout << "main " << head << std::endl;
    for (auto tn=0; tn < num_tests; ++tn)
    {
        std::cout << "Test " << tn  << ")" << std::endl;
        do
        {
            std::this_thread::yield();
        }while(rendezvous[tn].ready_count() < num_threads);

        std::this_thread::sleep_for(1ms);
        std::cout << "\tAll threads ready, GO!" << std::endl;
        rendezvous[tn].go = true;
        std::this_thread::sleep_for(1ms);

        do
        {
            std::this_thread::yield();
        }while(rendezvous[tn].complete_count() < num_threads);
        std::cout << "\tChecking...!" << std::endl;
        checkfuncs[tn](head, th_args, threads);
        std::cout << "\tDone." << std::endl;
    }

    for(auto &th : threads)
    {
        th.join();
    }
    std::cout << std::endl;

#if 0
    std::thread::id tid = head.next()->tid;
    for(auto b=head.next(); nullptr != b; b=b->next())
    {
        if (tid != b->tid)
        {
            std::cout << std::endl;
            tid = b->tid;
        }
        std::cout << b->v << " ";
    }
    std::cout << std::endl;
#endif
    std::cout << "All Done. " << std::endl;
    return 0;
}
