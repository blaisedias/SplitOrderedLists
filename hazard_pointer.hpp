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
        // Hazard pointer storage type for generic manipulation of hazard pointers
        // the algorithms only use pointer values not contents,
        // so a single implementation independent of type suffices.
        typedef void*   hazptr_st;
        class hazp_pool_generic {
            private:
            static constexpr uint32_t  FULL = UINT32_MAX;

            hazptr_st   *haz_ptrs = nullptr;
            // bitmap of reserved hazard pointers (blocks of size S)
            uint32_t    bitmap=0;

            protected:
            static constexpr unsigned  NUM_HAZP_POOL_BLOCKS=32;
            const unsigned  blk_size;
            const unsigned  hp_count;

            hazp_pool_generic(unsigned bsize):blk_size(bsize),hp_count(bsize * NUM_HAZP_POOL_BLOCKS)
            {
                haz_ptrs = new hazptr_st[hp_count];
                std::memset(haz_ptrs, 0, sizeof(*haz_ptrs) * hp_count);
            }

            virtual ~hazp_pool_generic()
            {
                delete [] haz_ptrs;
            }

            protected:
            unsigned copy_hazard_pointers(void *dest, unsigned count)
            {
                if (count > hp_count)
                    count = hp_count;
                std::memcpy(dest, haz_ptrs, count);
                return count;
            }

            hazptr_st* reserve_impl(unsigned len)
            {
                uint32_t    mask=1;
                uint32_t    ix=0;
                hazptr_st*  reserved = nullptr;
                uint32_t    expected = bitmap;

                if (len != blk_size)
                    return nullptr;
                while (expected != FULL && nullptr == reserved)
                {
                    mask = 1;
                    ix = 0;
                    while (0 != (expected & mask) && ix < NUM_HAZP_POOL_BLOCKS)
                    {
                        mask <<= 1;
                        ++ix;
                    }

                    if (ix < NUM_HAZP_POOL_BLOCKS)
                    {
                        uint32_t desired = expected | mask;
                        if(__atomic_compare_exchange(&bitmap, &expected, &desired,
                                false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
                        {
                            reserved = &haz_ptrs[ix * blk_size];
                        }
                        else
                        {
                            // CAS failed, so expected will have been updated to the new value
                            // of bitmap.
                        }
                    }
                }

                return reserved;
            }

            bool release_impl(hazptr_st* ptr)
            {
                if (ptr < &haz_ptrs[0] || ptr >= &haz_ptrs[hp_count])
                    return false;
                uint32_t    mask=1;
                for(auto x = 0; x < blk_size; x++)
                {
                    ptr[x] = 0;
                }
                for(unsigned ix = 0; ix < NUM_HAZP_POOL_BLOCKS; ++ix)
                {
                    if(ptr == &haz_ptrs[ix * blk_size])
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

        //Pool of hazard pointers, which are allocated (reserve)
        //and freed (release) atomically.
        template <typename T> struct hazp_pool:hazp_pool_generic
        {
            // Pools of hazard pointers can be chained.
            hazp_pool<T> *next = nullptr;

            hazp_pool(unsigned blk_size):hazp_pool_generic(blk_size){}
            ~hazp_pool()=default;

            inline unsigned block_size()
            {
                return hazp_pool_generic::blk_size;
            }

            inline unsigned count()
            { 
                return hazp_pool_generic::hp_count;
            }

            inline T** reserve(unsigned len)
            {
                return reinterpret_cast<T**>(hazp_pool_generic::reserve_impl(len));
            }

            inline bool release(T** ptr)
            {
                return hazp_pool_generic::release_impl(reinterpret_cast<hazptr_st*>(ptr));
            }

            inline unsigned copy_hazard_pointers(T** dest, unsigned num)
            {
                return hazp_pool_generic::copy_hazard_pointers(dest, num);
            }
        };

        //Holds pointer to a deleted object of type T
        template <typename T> struct hazp_delete_node
        {
            hazp_delete_node<T>* next;
            T* payload;

            hazp_delete_node(T* datap):payload(datap) {}
            ~hazp_delete_node()=default;
        };

        // A hazard pointer domain defines the set of pointers protected
        // and checked against at delete.
        template <typename T> class hazard_pointer_domain
        {
            private:
            // For now keep two lists of hazard pointer pools
            // TODO: Consider using a single list for pools, and
            // allow pools with arbitrary block sizes;
            // sizes. The down-side is performance at reservation and release,
            // since single list would have to be traversed until the correct
            // pool is found.
            // linked lists of hazard pointer pools.
            hazp_pool<T>* pools_head = nullptr;

            //list of deleted nodes overflow from hazard_pointer_context instances,
            //or no longer in a hazard_pointer_context scope (the 
            //hazard_pointer_context instance was destroyed, but deletes were
            //pending).
            //FIXME: volatile? or atomic load from/store to?
            hazp_delete_node<T>* delete_head = nullptr;

            void pools_new(hazp_pool<T>** phead, unsigned blocklen)
            {
                hazp_pool<T>* pool = new hazp_pool<T>(blocklen);
                do
                {
                    pool->next = *phead;
                }while(!__atomic_compare_exchange(phead, &pool->next, &pool,
                            false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED));
            }

            void delete_pools(hazp_pool<T>* head)
            {
                for(auto p = head; nullptr != p; )
                {
                    auto pnext = p->next;
                    delete p;
                    p = pnext;
                }
            }

            bool pools_release(hazp_pool<T>* head, T**ptr)
            {
                bool released = false;
                for(auto p = head; nullptr != p && !released; )
                {
                    auto pnext = p->next;
                    released = p->release(ptr);
                    p = pnext;
                }
                return released;
            }

            unsigned pools_count_ptrs(hazp_pool<T>* head)
            {
                unsigned count = 0;
                for(auto p = head; nullptr != p; )
                {
                    count += p->count();
                    p = p->next;
                }
                return count;
            }

            unsigned pools_copy_ptrs(hazp_pool<T>* head, T**dest, unsigned len)
            {
                unsigned count = 0;
                for(auto p = head; nullptr != p; )
                {
                    assert(count + p->count() <= len);
                    count += p->copy_hazard_pointers(dest + count, p->count());
                    p = p->next;
                }
                return count;
            }

            T** pools_reserve(hazp_pool<T>* head, unsigned blocklen)
            {
                T** reservation = nullptr;
                for(auto p = head; nullptr != p && nullptr == reservation; )
                {
                    reservation = p->reserve(blocklen);
                }
                return reservation;
            }

            public:

            hazard_pointer_domain()=default;
            ~hazard_pointer_domain()
            {
                collect();
                // If we are terminating, then all items schedulde for delete
                // should be deleted.
                // FIXME: all associated hazard_pointer_context instances 
                // should be destroyed. at the very least we should check
                // assert, or emit a warning message.
                // Alternatively hazard_pointer_context instances use a shared
                // pointer to the hazard_pointer_domain.
                assert(nullptr == delete_head);
                delete_pools(pools_head);
           }

            T** reserve(unsigned blocklen)
            {
                T** reservation = pools_reserve(pools_head, blocklen);
                if (nullptr == reservation)
                {
                    pools_new(&pools_head, blocklen);
                }
                reservation = pools_reserve(pools_head, blocklen);
                assert(nullptr != reservation);
                return reservation;
            }

            void release(T** ptr)
            {
                bool released = pools_release(pools_head, ptr);
                assert(released);
            }

            void push_delete_node(hazp_delete_node<T>* del_node)
            {
                hazp_delete_node<T>* desired = del_node;
                do
                {
                    del_node->next = delete_head;
                }
                while(!__atomic_compare_exchange(&delete_head, &del_node->next, &desired,
                            false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED));
            }

            inline void enqueue_for_delete(T* item_ptr)
            {
                auto del_entry = new hazp_delete_node<T>(item_ptr);
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
                hazptr_st v1 = *((hazptr_st*)ap);
                hazptr_st v2 = *((hazptr_st*)bp);
                if(v1 < v2)
                    return -1;
                if(v1 > v2)
                    return 1;
                return 0;
            }

            //TODO: try a version with vectors to measure performance.
            T** snapshot_ptrs(unsigned *pcount)
            {
                unsigned count = pools_count_ptrs(pools_head);

                *pcount = count;
                T** hpvalues = new T*[count];
                std::memset(hpvalues, 0, sizeof(*hpvalues) * count);
                unsigned n_copied = pools_copy_ptrs(pools_head, hpvalues, count);
                assert(n_copied == count);

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
            //FIXME: need to come up with a scheme to run this function
            //often enough if there is items to be deleted.
            void collect()
            {
                hazp_delete_node<T>* del_head = __atomic_exchange_n(
                        &delete_head, nullptr,  __ATOMIC_ACQ_REL);
                T** hp_values;
                unsigned num_values;
                hp_values = snapshot_ptrs(&num_values);

                hazp_delete_node<T>** pprev = &del_head;
                while(nullptr != *pprev)
                {
                    hazp_delete_node<T>* cur = *pprev;
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
                    unsigned num_values;
                    hp_values = domain->snapshot_ptrs(&num_values);
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

