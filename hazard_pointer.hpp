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
along with this file.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef BENEDIAS_HAZARDD_POINTER_HPP
#define BENEDIAS_HAZARDD_POINTER_HPP
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <utility>
#include <memory>
#include <algorithm>
#include "mark_ptr_type.hpp"
#if 1
#include <iostream>
#include <cstdio>
#endif

//FIXME: At the moment the typename T propagates to supporting classes,
//which means even though we are dealing with pointers (albeit to different types),
//code is needlessly replicated.
//It should be possible to rework so that at the bottom end uintptr_t are used
//with a few judiciously placed reinterpret_cast or dynamic_cast statements.
namespace benedias {
    namespace concurrent {
        //Pool of hazard pointers, which are allocated (reserve)
        //and freed (release) atomically.
        template <typename T, unsigned S> struct hazp_pool
        {
            static constexpr uint32_t  FULL = -1;
            // Pools of hazard pointers can be chained.
            struct hazp_pool<T, S> *next = nullptr;
            // bitmap of reserved hazard pointers (blocks of size S)
            uint32_t bitmap = 0;
            T* array[32 * S];

            hazp_pool()=default;
            ~hazp_pool()=default;

            unsigned block_size() { return S; }
            unsigned count() { return 32 * S; }

            T** reserve()
            {
                uint32_t    mask;
                uint32_t    ix;
                T** reserved = nullptr;

                uint32_t expected = bitmap;
                while (expected != FULL && nullptr == reserved)
                {
                    mask = 1;
                    ix = 0;
                    while (0 != (expected & mask) && ix < 32)
                    {
                        mask <<= 1;
                        ++ix;
                    }

                    if (ix < 32)
                    {
                        uint32_t desired = expected | mask;
                        if(__atomic_compare_exchange(&bitmap, &expected, &desired,
                                false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
                        {
                            reserved = &array[ix * S];
                        }
                    }
                }

                return reserved;
            }

            bool release(T** ptr)
            {
                if (ptr < &array[0] || ptr >= &array[32 * S])
                    return false;

                uint32_t    mask=1;
                for(auto x = 0; x < S; x++)
                {
                    ptr[x] = nullptr;
                }
                for(unsigned ix = 0; ix < 32; ++ix)
                {
                    if(ptr == &array[ix * S])
                    {
                        assert(mask == (bitmap & mask));
                        uint32_t    expected, desired;
                        do
                        {
                            expected = bitmap;
                            desired = expected ^ mask;
                        }while(!__atomic_compare_exchange(&bitmap, &expected, &desired,
                                false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED));
                        break;
                    }
                    mask <<= 1;
                }

                return true;
            }
        };

        //Holds pointer to a deleted object of type T
        template <typename T> struct hazp_delete_node
        {
            struct hazp_delete_node<T>* next;
            T* payload;

            hazp_delete_node(T* datap):payload(datap) {}
            ~hazp_delete_node()=default;
        };

        // A hazard pointer domain defines the set of pointers protected
        // and checked against at delete.
        template <typename T> class hazard_pointer_domain
        {
            private:
            // linked lists of hazard pointer pools.
            struct hazp_pool<T, 1>* singles_head = new hazp_pool<T, 1>();
            // The triples pool exists to meet the requirement for lock-free
            // singly linked lists.
            struct hazp_pool<T, 3>* triples_head = new hazp_pool<T, 3>();

            //list of deleted nodes overflow from hazard_pointer_context instances,
            //or no longer in a hazard_pointer_context scope (The context was destroyed).
            //FIXME: volatile? or atomic load from/store to?
            struct hazp_delete_node<T>* delete_head = nullptr;

            public:

            hazard_pointer_domain()=default;
            ~hazard_pointer_domain()
            {
                collect();
                // If we are terminating, then all items schedule for delete
                // should be deleted.
                // FIXME: all associated hazard_pointer_context instances 
                // should be destroyed. at the very least we should check
                // assert, or emit a warning message.
                assert(nullptr == delete_head);
                for(auto p = singles_head; nullptr != p; )
                {
                    auto pnext = p->next;
                    delete p;
                    p = pnext;
                }
                for(auto p = triples_head; nullptr != p; )
                {
                    auto pnext = p->next;
                    delete p;
                    p = pnext;
                }
            }

            T** reserve(unsigned blocklen)
            {
                if(blocklen == 1)
                    return singles_head->reserve();
                if(blocklen == 3)
                    return triples_head->reserve();
                return nullptr;
            }

            void release(T** ptr)
            {
                bool released = false;
                {
                    auto p = singles_head;
                    while(p && !released)
                    {
                        released = p->release(ptr);
                        p=p->next;
                    }
                }
                {
                    auto p = triples_head;
                    while(p && !released)
                    {
                        released = p->release(ptr);
                        p=p->next;
                    }
                }
                assert(released);
            }

            void push_delete_node(struct hazp_delete_node<T>* del_node)
            {
                struct hazp_delete_node<T>* desired;
                do
                {
                    del_node->next = delete_head;
                    desired = del_node;
                }
                while(!__atomic_compare_exchange(&delete_head, &del_node->next, &desired,
                            false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED));
            }

            inline void enqueue_for_delete(T* item_ptr)
            {
                auto del_entry = new struct hazp_delete_node<T>(item_ptr);
                push_delete_node(del_entry);
            }

            void enqueue_for_delete(T** items_ptr, unsigned count)
            {
                for(unsigned x = 0; x < count; ++x)
                {
                    if (nullptr != items_ptr[x])
                    {
                        enqueue_for_delete(items_ptr[x]);
                        items_ptr[x] = nullptr;
                    }
                }
            }

            static int cmphp(const void* ap, const void* bp)
            {
                uintptr_t v1 = *((uintptr_t*)ap);
                uintptr_t v2 = *((uintptr_t*)bp);
                if(v1 < v2)
                    return -1;
                if(v1 > v2)
                    return 1;
                return 0;
            }

            //TODO: try a version with vectors to measure performance.
            T** snapshot_ptrs(unsigned *pcount)
            {
                unsigned count = 0;
                {
                    auto p = singles_head;
                    while(p)
                    {
                        count += p->count();
                        p = p->next;
                    }
                }
                {
                    auto p = triples_head;
                    while(p)
                    {
                        count += p->count();
                        p = p->next;
                    }
                }

                *pcount = count;
                unsigned ix = 0;
                T** hpvalues = new T*[count];
                std::memset(hpvalues, 0, sizeof(*hpvalues) * count);
                {
                    auto p = triples_head;
                    while(p)
                    {
                        assert(ix + p->count() <= count);
                        std::memcpy(hpvalues + ix, p->array, p->count());
                        ix += p->count();
                        p = p->next;
                    }
                }
                {
                    auto p = singles_head;
                    while(p)
                    {
                        assert(ix + p->count() <= count);
                        std::memcpy(hpvalues + ix, p->array, p->count());
                        ix += p->count();
                        p = p->next;
                    }
                }
                assert(ix == count);
#if 1       // clang memory sanitizer was generating errors even though qsort was not :-(
                std::sort(hpvalues, hpvalues+count);
#else
                qsort(hpvalues, count, sizeof(hpvalues[0]), cmphp);
#endif
                return hpvalues;
            }

            inline bool search(T*value, T** hp_values, unsigned num_values)
            {
#if 1       //clang memory sanitizer was generating errors even though bsearch or linear search was not :-(1
                return std::binary_search(hp_values, hp_values + num_values, value);
#else
#if 1
                return (nullptr == bsearch(value, hp_values, num_values, sizeof(*hp_values), cmphp));
#else
                for(unsigned x=0; x < num_values; ++x)
                {
                    if (nullptr == hp_values[x])
                        continue;
                    if (hp_values[x] == value)
                    {
                        return true;
                    }
                }
#endif
                return false;
#endif
            }

            //FIXME: serialise execution of this function.
            void collect()
            {
                struct hazp_delete_node<T>* del_head = __atomic_exchange_n(
                        &delete_head, nullptr,  __ATOMIC_ACQ_REL);
                T** hp_values;
                T** hp_values_end;
                unsigned num_values;
                hp_values = snapshot_ptrs(&num_values);
                hp_values_end = hp_values + num_values;

                struct hazp_delete_node<T>** pprev = &del_head;
                while(nullptr != *pprev)
                {
                    struct hazp_delete_node<T>* cur = *pprev;
                    if (!search(cur->payload, hp_values, num_values))
                    {
                        // delink
                        *pprev = cur->next;
                        delete cur->payload;
                        delete cur;
                    }
                    else
                    {
                        //step forward
                        pprev = &cur->next;
                    }
                }

                while(nullptr != del_head)
                {
                    auto del_entry = del_head;
                    del_head = del_head->next;
                    push_delete_node(del_entry);
                }
                delete [] hp_values;
            }

        };

        //To use hazard pointers using  hazard_pointer_domain,
        //create an instance.
        //This class is designed for use by a single thread.
        template <typename T, unsigned S, unsigned R> class hazard_pointer_context
        {
            private:
            hazard_pointer_domain<T>* domain;
            T* deleted[R];
            unsigned del_index=0;

            public:
            T** hazard_pointers;
            const unsigned num_hazard_pointers;

            hazard_pointer_context(hazard_pointer_domain<T>* dom):domain(dom),num_hazard_pointers(S)
            {
                hazard_pointers = domain->reserve(S);
                for(unsigned x=0; x<R; ++x) { deleted[x] = nullptr;}
                //FIXME: throw exception.
                assert(hazard_pointers != nullptr);
            }

            ~hazard_pointer_context()
            {
                // Release the hazard pointers
                domain->release(hazard_pointers);
                // Delegate deletion of nodes to be deleted 
                // to the domain. 
                domain->enqueue_for_delete(deleted, R);
                domain->collect();
            }

            void delete_item(T* item_ptr)
            {
                assert(del_index <= R);
                deleted[del_index] = item_ptr;
                ++del_index;

                if (del_index == R)
                {
                    // overflow
                    T** hp_values;
                    T** hp_values_end;
                    unsigned num_values;
                    hp_values = domain->snapshot_ptrs(&num_values);
                    hp_values_end = hp_values + num_values;
                    for(unsigned ix=0; ix < R; ++ix)
                    {
                        if (!domain->search(deleted[ix], hp_values, num_values))
                        {
                            // on delete zero out the array field,
                            // and reduce the delete index var.
                            --del_index;
                            delete deleted[ix];
                            deleted[ix] = nullptr;
                        }
                    }
                    delete [] hp_values;

                    // del_index is not guaranteed to be valid here
                    if (del_index == R)
                    {
                        // Could not delete anything, so enqueue for delete
                        // on the domain.
#if 0
                        domain->enqueue_for_delete(deleted[del_index -1]);
                        deleted[--del_index] = nullptr;
#else
                        domain->enqueue_for_delete(deleted, R);
                        del_index = 0;
#endif
                    }
                    else
                    {
                        // deleted at least one, move undeleted items up,
                        // so that ix_delete is valid once again.
                        unsigned ixd=0, ixs=R-1;
                        while(ixd < ixs)
                        {
                            while(nullptr != deleted[ixd] && ixd < ixs)
                                ++ixd;
                            while(nullptr == deleted[ixs] && ixd < ixs)
                                --ixs;
                            if (nullptr != deleted[ixs] 
                                    && nullptr == deleted[ixd])
                            {
                                std::swap(deleted[ixs], deleted[ixd]);
                                ++ixd;
                                --ixs;
                            }
                        }
                    }
                }
            }
        };

    } //namespace concurrent
} //namespace benedias
#endif // #define BENEDIAS_HAZARDD_POINTER_HPP

