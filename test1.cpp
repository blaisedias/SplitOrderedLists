/*

Copyright (C) 2014,2015  Blaise Dias

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
*/
#include "solist.hpp"
#include "solist_dbg.hpp"
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <memory>

using   benedias::concurrent::solist;
using   benedias::concurrent::solist_accessor;
using   benedias::concurrent::hash_t;

void test0_1(hash_t h[3])
{
    solist_accessor<uint32_t> sol(4);
//    benedias::concurrent::dump_solist_buckets(sol);
//    benedias::concurrent::dump_solist(sol);
    sol.initialise_bucket(h[0]);
//    benedias::concurrent::dump_solist_buckets(sol);
//    benedias::concurrent::dump_solist(sol);
    sol.initialise_bucket(h[1]);
//    benedias::concurrent::dump_solist_buckets(sol);
    sol.initialise_bucket(h[2]);
//    benedias::concurrent::dump_solist_buckets(sol);
    benedias::concurrent::check_solist(sol);
    benedias::concurrent::dump_solist_keys(sol);
//    benedias::concurrent::dump_solist_key_order(sol);
//    benedias::concurrent::dump_solist(sol);
}


// simple test of bucket initialisation
void test0()
{
    {
        hash_t t1[3]={1,2,3};
        test0_1(t1);
        std::cout << "----------" << std::endl;
    }
    {
        hash_t t2[3]={3,2,1};
        test0_1(t2);
        std::cout << "----------" << std::endl;
    }
    {
        hash_t t3[3]={2,3,1};
        test0_1(t3);
        std::cout << "----------" << std::endl;
    }
    {
        hash_t t4[3]={1,3,2};
        test0_1(t4);
        std::cout << "----------" << std::endl;
    }
    std::cout << "==========================" << std::endl;
}

//-------------------------

void test1_1(solist_accessor<uint32_t>& sol, hash_t h[4])
{
    for(uint32_t x=0; x < 4; ++x)
    {
        sol.initialise_bucket(h[x]);
    }
}

// simple test of bucket initialisation and node insertion
void test1()
{
    solist_accessor<uint32_t> sol(4);
    {
        hash_t t3[4]={2,3,1,0};
        test1_1(sol, t3);
        benedias::concurrent::dump_solist_buckets(sol);
        benedias::concurrent::dump_solist_key_order(sol);
        std::cout << "----------" << std::endl;
    }
    sol.insert_node(2, 2);
    sol.insert_node(1, 1);
    benedias::concurrent::dump_solist_buckets(sol);
    benedias::concurrent::dump_solist_key_order(sol);
    benedias::concurrent::dump_solist(sol);
}

// simple test of bucket initialisation, node insertion and node deletion
void test3()
{
    solist_accessor<uint32_t> sol(2);
    bool marked[32];
    uint32_t n_gen = 0;
    uint32_t iters = 0;

    uint32_t count = sizeof(marked)/sizeof(marked[0]);

    for (unsigned x=0; x < count; ++x)
    {
        marked[x] = false;
    }

    while(n_gen <= count && (++iters < (count * 4)))
    {
        uint32_t v = static_cast<uint32_t>(rand()) % sizeof(marked);
        if (!marked[v])
        {
            std::cerr << v << " ";
            sol.insert_node(v, v);
            marked[v] = true;
            ++n_gen;
        }
    }

    for (unsigned v=0; v < count; ++v)
    {
        if (!marked[v])
        {
            std::cerr << v << " ";
            sol.insert_node(v, v);
            marked[v] = true;
            ++n_gen;
        }
    }
    std::cerr << std::endl;
    benedias::concurrent::dump_solist_items(sol);
    benedias::concurrent::dump_solist(sol);
    benedias::concurrent::check_solist(sol);

    std::cout << std::endl << "-- Deleting 30, 0 and 31" << std::endl << std::endl;
    sol.delete_node(30);
    sol.delete_node(0);
    sol.delete_node(31);

    benedias::concurrent::dump_solist_items(sol);
    benedias::concurrent::dump_solist(sol);
    benedias::concurrent::check_solist(sol);
}


int main( int argc, char* argv[] )
{
    std::setlocale(LC_ALL, "en_US.UTF-8");
    std::srand(std::time(nullptr)); // use current time as seed for random generator
#if 0
    test0();
    test1();
#endif
    test3();
    std::cout << "All Done. " << std::endl;
    return 0;
}
