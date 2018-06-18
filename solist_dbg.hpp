/*

Copyright (C) 2017,2018  Blaise Dias

sharedobj is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

It is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with sharedobj.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef BENEDIAS_SOLIST_DBG_HPP
#define BENEDIAS_SOLIST_DBG_HPP
#include "solist.hpp"
#include <cstdio>
#include <iostream>
#include <memory>
namespace benedias {
    namespace concurrent {

    template <typename T> void dump_solist_buckets(solist_accessor<T>& sa)
    {
        std::shared_ptr<solist<T>> sol = sa.so_list;

        fprintf(stderr,
                "(=== dump_solist_buckets %p\n", &sol);
        for(uint32_t x=0; x < sol->size; ++x)
        {
            if (nullptr != sol->buckets[x])
            {
                fprintf(stderr,"%d) %p 0x%08x 0x%08x %d\n", x, sol->buckets[x],
                        sol->buckets[x]->key,
                        sol->buckets[x]->hashv,
                        sol->buckets[x]->hashv % sol->size
                        );
            }
            else
            {
                fprintf(stderr,"%d)\n", x);
            }
        }
        std::cerr << std::endl << "===)" << std::endl;
    }

    template <typename T> void dump_solist_keys(solist_accessor<T>& sa)
    {
        sa.zap();
        std::shared_ptr<solist<T>> sol = sa.so_list;

        solist_bucket *cur = sol->buckets[0];
        fprintf(stderr,
                "(=== dump_solist_keys %p\n", &sol);

        while(cur)
        {
            fprintf(stderr, "0x%08x, ", cur->key);
            cur = cur->next();
        }
        std::cerr << std::endl;
        cur = sol->buckets[0];
        while(cur)
        {
            fprintf(stderr, "0x%08x, ", cur->hashv);
            cur = cur->next();
        }
        std::cerr << std::endl << "===)" << std::endl;
    }

    template <typename T> void dump_solist_key_order(solist_accessor<T>& sa)
    {
        std::shared_ptr<solist<T>> sol = sa.so_list;

        solist_bucket *cur = sol->buckets[0];
        fprintf(stderr,
                "(=== dump_solist_key_order %p\n", &sol);

        while(cur)
        {
            fprintf(stderr, "0x%08x, ", cur->key);
            cur = cur->next();
        }
        std::cerr << std::endl << "===)" << std::endl;
    }

    template <typename T> void dump_solist(solist_accessor<T>& sa)
    {
        std::shared_ptr<solist<T>> sol = sa.so_list;

        solist_bucket *cur = sol->buckets[0];
        fprintf(stderr,
                "(=== dump_solist %p size=%d", &sol, sol->size);
        while(cur)
        {
            if (cur->key & DATABIT)
            {
                auto curnode = reinterpret_cast<solist_node<T>*>(cur);
                fprintf(stderr, "0x%08x|", cur->key);
                std::cerr << curnode->payload << ", ";
            }
            else
            {
                fprintf(stderr, "\n 0x%08x|- ", cur->key);
            }
            cur = cur->next();
        }
        std::cerr << std::endl;
#if 0
        std::cerr << "buckets" << std::endl;
        for(uint32_t x=0; x < sol->size; ++x)
        {
            fprintf(stderr,"%d) ", x);
            if (nullptr != sol->buckets[x])
            {
                fprintf(stderr,"0x%08x 0x%08x\n",
                        sol->buckets[x]->key,
                        sol->buckets[x]->hashv
                       );
            }
            else
            {
                fprintf(stderr,"\n");
            }
        }
#endif
        std::cerr << "===)" << std::endl;
    }

    template <typename T> void dump_solist_items(solist_accessor<T>& sa)
    {
        std::shared_ptr<solist<T>> sol = sa.so_list;

        solist_bucket *cur = sol->buckets[0];
        fprintf(stderr,
                "(=== dump_solist_items %p size=%d\n", &sol, sol->size);
        while(cur)
        {
            if (cur->key & DATABIT)
            {
                auto curnode = reinterpret_cast<solist_node<T>*>(cur);
                std::cerr << curnode->payload << ", ";
            }
            cur = cur->next();
        }
        std::cerr << std::endl << "===)" << std::endl;
    }


    template <typename T> void check_solist(solist_accessor<T>& sa)
    {
        std::shared_ptr<solist<T>> sol = sa.so_list;

        fprintf(stderr,
                "(=== check_solist %p ", &sol);
        fprintf(stderr,
                "checking for monotonically increasing keys ");
        solist_bucket *cur = sol->buckets[0];
        hash_t  key = cur->key;
        cur = cur->next();
        while(cur)
        {
            if (!(cur->key > key))
            {
                fprintf(stderr, "\nFail:: %p 0x%08x %p; prev=0x%08x", cur, cur->key, cur->next(), key);
            }
            key = cur->key;
            cur = cur->next();
        }
        std::cerr << "===)" << std::endl;
    }

    } //namespace concurrent
} //namespace benedias
#endif // #define BENEDIAS_SOLIST_DBG_HPP

