/*

Copyright (C) 2014-2019  Blaise Dias

This file is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

sqzbsrv is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with sqzbsrv.  If not, see <http://www.gnu.org/licenses/>.
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

uint32_t values[32] =
{
//0, 22, 21, 9, 25, 2, 30, 31, 13, 8, 28, 16, 27, 12, 7, 6, 10, 23, 19, 24, 1, 29, 18, 17, 20, 3, 15, 11, 26, 4, 14, 5
10, 17, 1, 26, 29, 30, 3, 8, 20, 16, 24, 14, 27, 13, 15, 22, 0, 28, 5, 25, 23, 19, 7, 18, 12, 31, 21, 9, 11, 2, 6, 4
};

// test split ordered list expansion
void test_expansion()
{
    solist_accessor<uint32_t> sol(2);
#if 0
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
            std::cerr << v << std::endl;
            sol.insert_node(v, v);
            marked[v] = true;
            ++n_gen;
        }
        benedias::concurrent::dump_solist(sol);
        benedias::concurrent::dump_solist_buckets(sol);
    }

    for (unsigned v=0; v < count; ++v)
    {
        if (!marked[v])
        {
            std::cerr << v << std::endl;
            sol.insert_node(v, v);
            marked[v] = true;
            ++n_gen;
            benedias::concurrent::dump_solist(sol);
            benedias::concurrent::dump_solist_buckets(sol);
        }
    }
#else
    uint32_t count = sizeof(values)/sizeof(values[0]);

    for (unsigned ix=0; ix < count; ++ix)
    {
        uint32_t v = values[ix];
        std::cerr << v << std::endl;
        sol.insert_node(v, v);
        benedias::concurrent::dump_solist(sol);
        benedias::concurrent::dump_solist_buckets(sol);
    }

#endif
    std::cerr << std::endl;
    benedias::concurrent::check_solist(sol);
}


int main( int argc, char* argv[] )
{
    std::setlocale(LC_ALL, "en_US.UTF-8");
    std::srand(std::time(nullptr)); // use current time as seed for random generator
    test_expansion();
    std::cout << "All Done. " << std::endl;
    return 0;
}
