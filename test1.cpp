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
    sol.initialise_bucket(h[0]);
    sol.initialise_bucket(h[1]);
    sol.initialise_bucket(h[2]);
    benedias::concurrent::dump_solist(sol);
    benedias::concurrent::check_solist(sol);
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
        std::cout << "----------" << std::endl;
    }
    sol.insert_node(2, 2);
    sol.insert_node(1, 1);
//    benedias::concurrent::dump_solist_buckets(sol);
//    benedias::concurrent::dump_solist_key_order(sol);
    benedias::concurrent::dump_solist(sol);
}

// simple test of bucket initialisation, node insertion and node deletion
// hash values are optimally geberated, you should end up with 8 buckets
// of 4 entries each, before the deletes.
void test2()
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

    std::cout << std::endl << "-- Checking find" << std::endl << std::endl;
    for(hash_t x = 0; x < 32; ++x)
    {
        if (nullptr == sol.find_item_node(x))
        {
            std::cout << "Failed! could not find item with hash " << x << std::endl;
        }
    }
    std::cout << "-- Deleting 30, 0 and 31" << std::endl << std::endl;
    sol.delete_node(30);
    sol.delete_node(0);
    sol.delete_node(31);

    benedias::concurrent::dump_solist_items(sol);
    benedias::concurrent::dump_solist(sol);
    benedias::concurrent::check_solist(sol);
}


// test adds 32 randomly generated nodes.
void test3()
{
    solist_accessor<uint32_t> sol(2);
    uint32_t n_gen = 0;

    while(n_gen <= 32)
    {
        uint32_t v = static_cast<uint32_t>(rand());
        std::cerr << v << " ";
        if (sol.find_item_node(v) == nullptr)
        {
            sol.insert_node(v, v);
            ++n_gen;
        }
    }

    std::cerr << std::endl;
    benedias::concurrent::dump_solist_items(sol);
    benedias::concurrent::dump_solist(sol);
    benedias::concurrent::check_solist(sol);
}


// experimental function
void testx()
{
    constexpr   unsigned n_buckets=8;
    uint32_t slot = 5;
    uint32_t key_n_buckets = benedias::concurrent::sol_bucket_key(n_buckets);
    uint32_t key_step = benedias::concurrent::sol_bucket_key(n_buckets/2);
    
    benedias::concurrent::so_key key = benedias::concurrent::sol_bucket_key(slot);
    printf("key=%x bucket_from_key=%d bucket_from_n_buckets=%x key_step=%x\n",
            key,
            benedias::concurrent::reverse_hasht_bits(key),
            key_n_buckets,
            key_step
            );
    do
    {
        key -= key_step;
        slot = benedias::concurrent::reverse_hasht_bits(key);
        printf("key=%x slot_from_key=%d\n",
            key,
            benedias::concurrent::reverse_hasht_bits(key));

    }while(key);
}

int main( int argc, char* argv[] )
{
    std::setlocale(LC_ALL, "en_US.UTF-8");
    std::srand(std::time(nullptr)); // use current time as seed for random generator
    void (*tf)() = test3;
    if (argc > 1)
    {
        switch(*argv[1])
        {
            case '0':
                tf = test0; break;
            case '1':
                tf = test1; break;
            case '2':
                tf = test2; break;
            case '3':
                tf = test3; break;
            case 'x':
                tf = testx; break;
        }
    }

    tf();
    std::cout << "All Done. " << std::endl;
    return 0;
}
