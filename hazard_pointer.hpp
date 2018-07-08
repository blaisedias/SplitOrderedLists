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

#ifndef BENEDIAS_HAZARD_POINTER_HPP
#define BENEDIAS_HAZARD_POINTER_HPP
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <utility>
#include <memory>
#include <algorithm>
#include "mark_ptr_type.hpp"

#if 0
// for printf debugging.
#include <iostream>
#include <cstdio>
#endif

namespace benedias {
    namespace concurrent {
        /// Hazard pointer storage type for generic manipulation of hazard pointers
        /// the algorithms only use pointer values not contents, so type is no relevant.
        /// Using a generic type means a single implementation independent of type,
        /// suffices and generates less code.
        typedef void*   generic_hazptr_t;

        /// hazard pointer chunk, manages reservation and release of a hazard
        /// pointers in blocks of a size fixed at creation time.
        /// A collection of hazard pointer chunks form the pool for a hazard
        /// pointer domain.
        /// The generic hazard pointer chunk uses generic_hazptr_t
        class hazp_chunk_generic {
            private:
            generic_hazptr_t   *haz_ptrs = nullptr;
            // bitmap of reserved hazard pointers (1 bit maps to an "array" of length=blk_size)
            uint32_t    bitmap=0;

            protected:
            static constexpr unsigned  NUM_HAZP_CHUNK_BLOCKS=32;
            static constexpr uint32_t  FULL = UINT32_MAX;

            const unsigned  blk_size;
            const unsigned  hp_count;

            /// Constructor
            /// \@param blocksize the granularity of hazard pointer allocation desried.
            hazp_chunk_generic(unsigned blocksize):blk_size(blocksize),hp_count(blocksize * NUM_HAZP_CHUNK_BLOCKS)
            {
                haz_ptrs = new generic_hazptr_t[hp_count];
                std::fill(haz_ptrs, haz_ptrs + hp_count, nullptr);
            }

            virtual ~hazp_chunk_generic()
            {
                delete [] haz_ptrs;
            }

            protected:
            /// Copy the entire set of hazard pointers managed by this instance.
            /// \@param dest - destination buffer
            /// \@prama count - size of the destination buffer.
            unsigned copy_hazard_pointers(generic_hazptr_t *dest, unsigned count) const
            {
                //copy must be of the whole chunk
                assert(count >= hp_count);
                std::copy(haz_ptrs, haz_ptrs + hp_count, dest);
                return hp_count;
            }

            /// lock-free thread safe reservation of blocks of pointers.
            /// reservation will only succeed if the requested length is
            /// matches the block size of the hazard pointer chunk, and
            /// there is at least on free block.
            /// \@param len - the number of hazard pointers desired.
            /// \@return pointer to the block of hazard pointers or nullptr.
            generic_hazptr_t* reserve_impl(unsigned len)
            {
                uint32_t    mask=1;
                uint32_t    ix=0;
                generic_hazptr_t*  reserved = nullptr;
                uint32_t    expected = bitmap;

                if (len != blk_size)
                    return nullptr;
                while (expected != FULL && nullptr == reserved)
                {
                    mask = 1;
                    ix = 0;
                    while (0 != (expected & mask) && ix < NUM_HAZP_CHUNK_BLOCKS)
                    {
                        mask <<= 1;
                        ++ix;
                    }

                    if (ix < NUM_HAZP_CHUNK_BLOCKS)
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

            /// lock-free thread safe release of blocks of pointers.
            /// The pointers are only "released" if supplied pointer is to a
            /// block of hazard pointers managed by this chunk.
            /// \@param pointer to the first hazard pointer in the block.
            /// \@return true if hazard pointers were released, false otherwise.
            bool release_impl(generic_hazptr_t* ptr)
            {
                // To facilitate clients walking down a list of hazard pointer
                // chunks and invoking release until the correct chunk instance
                // actually releases the block, check address range, return
                // false if the block does not belong to this chunk instance.
                if (ptr < &haz_ptrs[0] || ptr >= &haz_ptrs[hp_count])
                    return false;

                uint32_t    mask=1;
                for(auto x = 0; x < blk_size; x++)
                {
                    ptr[x] = 0;
                }
                for(unsigned ix = 0; ix < NUM_HAZP_CHUNK_BLOCKS; ++ix)
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

            /// Returns simple reservation status for this chunk.
            /// This function is a helper function, intended for checking at destructor
            /// time when it can be "safely assumed" that further reservations will not
            /// be made.
            /// \@return true if there are active reservations within the chunk.
            bool has_reservations()
            {
                return 0 != bitmap;
            }
        };

        /// This class is a type wrapper for the generic hazard pool class,
        /// allows us to keep all "dangerous" casting restricted, as opposed to
        /// peppering the codebase.
        /// It is also has fields to facilitate chaining of hazard pointer chunks
        /// into a list.
        template <typename T> struct hazp_chunk:hazp_chunk_generic
        {
            // Pools of hazard pointers can be chained.
            hazp_chunk<T> *next = nullptr;

            hazp_chunk(unsigned blk_size):hazp_chunk_generic(blk_size){}
            ~hazp_chunk()=default;

            using hazp_chunk_generic::has_reservations;

            inline unsigned block_size()
            {
                return hazp_chunk_generic::blk_size;
            }

            inline unsigned count() const
            {
                return hazp_chunk_generic::hp_count;
            }

            inline T** reserve(unsigned len)
            {
                return reinterpret_cast<T**>(hazp_chunk_generic::reserve_impl(len));
            }

            inline bool release(T** ptr)
            {
                return hazp_chunk_generic::release_impl(reinterpret_cast<generic_hazptr_t*>(ptr));
            }

            inline unsigned copy_hazard_pointers(T** dest, unsigned num) const
            {
                return hazp_chunk_generic::copy_hazard_pointers(reinterpret_cast<generic_hazptr_t*>(dest), num);
            }
        };

        /// Holder for pointer to a deleted object of type T
        template <typename T> struct hazp_delete_node
        {
            hazp_delete_node<T>* next;
            T* payload;

            hazp_delete_node(T* datap):payload(datap) {}
            ~hazp_delete_node()=default;
        };

        template <typename T> class hazard_pointers_snapshot;

        /// A hazard pointer domain defines the set of pointers protected
        /// and checked against for safe memory recalamation.
        /// Typically a hazard pointer domain instance will be associated with
        /// a single instance of a conatiner class.
        template <typename T> class hazard_pointer_domain
        {
            private:
            /// linked lists for the pool of hazard pointer.
            /// For lock-free operation this list is only ever added to
            /// over the lifetime of the domain.
            hazp_chunk<T>* pools_head = nullptr;

            /// list of delete nodes, overflow from hazard_pointer_context instances,
            /// or no longer in a hazard_pointer_context scope (the
            /// hazard_pointer_context instance was destroyed, but deletes were
            /// pending).
            /// To keep operations lock-free, this list is only added to,
            /// by inserting at the head of the list atomically,
            /// Or atomically swapped out with an empty list at processing time,
            /// (see collect).
            hazp_delete_node<T>* delete_head = nullptr;

            /// For lock-free operation, we push new hazard pointer chunks
            /// to head of the list (pool) atomically.
            void pools_new(hazp_chunk<T>** phead, unsigned blocklen)
            {
                hazp_chunk<T>* pool = new hazp_chunk<T>(blocklen);
                do
                {
                    pool->next = *phead;
                }while(!__atomic_compare_exchange(phead, &pool->next, &pool,
                            false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED));
            }

            /// Attempt to fulfill a reservation request by requesting
            /// a block from with the pool of hazard pointer chunks.
            /// Reserving hazard pointers is "expensive",
            /// and is amortised at hazard pointer context creation,
            /// which is not expected to be frequent operation.
            /// \@param head - start of pool of hazard pointer chunks.
            /// \@param blocklen - num of hazard pointers required
            /// \@return - nullptr or pointer to the block of hazard pointers.
            T** pools_reserve(hazp_chunk<T>* head, unsigned blocklen)
            {
                T** reservation = nullptr;
                for(auto p = head; nullptr != p && nullptr == reservation; )
                {
                    reservation = p->reserve(blocklen);
                }
                return reservation;
            }

            /// Release previously reserved hazard pointer reservations.
            /// Releasing hazard pointers is "expensive",
            /// and is amortised at hazard pointer context destruction.
            /// which is not expected to be frequent.
            /// \@param head - pointer to list of hazard pointer blocks.
            /// \@param ptr - hazard pointer block to be released.
            bool pools_release(hazp_chunk<T>* head, T**ptr)
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

            template <typename U> friend class hazard_pointers_snapshot;
            public:

            hazard_pointer_domain()=default;
            ~hazard_pointer_domain()
            {
                // The domain is being destroyed, so all items scheduled for delete
                // should be deleted.
                collect();

                // FIXME: all associated hazard_pointer_context instances
                // should be destroyed. at the very least we should check
                // assert, or emit a warning message.
                // Alternatively, hazard_pointer_context instances can use a
                // shared pointer to the hazard_pointer_domain.
                assert(nullptr == delete_head);

                // Now delete all pools.
                for(hazp_chunk<T>* p = __atomic_exchange_n(
                        &pools_head, nullptr,  __ATOMIC_ACQ_REL); nullptr != p; )
                {
                    assert(!p->has_reservations());
                    auto pnext = p->next;
                    delete p;
                    p = pnext;
                }
            }

            /// Fulfill a reservation request using the pool of hazard pointer chunks
            /// creating new hazard pointer chunks if required.
            /// \@param blocklen - the number hazard pointers required.
            T** reserve(unsigned blocklen)
            {
                T** reservation = pools_reserve(pools_head, blocklen);
                if (nullptr == reservation)
                {
                    pools_new(&pools_head, blocklen);
                    reservation = pools_reserve(pools_head, blocklen);
                }
                assert(nullptr != reservation);
                return reservation;
            }

            /// Release hazard pointers previously reserved.
            /// \@param ptr - the hazard pointer(s) to be released.
            void release(T** ptr)
            {
                bool released = pools_release(pools_head, ptr);
                assert(released);
            }

            /// Push a delete node onto the delete list, lock free and wait free.
            void push_delete_node(hazp_delete_node<T>* del_node)
            {
                hazp_delete_node<T>* desired = del_node;
                do
                {
                    // Theoretically we only need to do this first time,
                    // as the "expected" field is updated to the current
                    // value on failed __atomic_compare_exchange calls.
                    del_node->next = delete_head;
                }
                while(!__atomic_compare_exchange(&delete_head, &del_node->next, &desired,
                            false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED));
            }

            /// Add a pointer to the delete list.
            /// Creates and pushes a delete node onto the delete list,
            /// lock free and wait free.
            inline void enqueue_for_delete(T* item_ptr)
            {
                auto del_entry = new hazp_delete_node<T>(item_ptr);
                push_delete_node(del_entry);
            }

            /// Add a set of pointers to the delete list.
            /// Creates and pushes a delete nodes onto the delete list,
            /// lock free and wait free.
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

            /// Delete objects on the delete list if no live pointers to
            /// those objects exist.
            /// Note: Serialisation of execution of this function, is not required.
            /// FIXME: A scheme needs to be designed such that this function
            /// is run often enough when delete list is not empty.
            void collect()
            {
                // swap the shared delete list with the empty local delete
                // list, which is only accessed by the this function instance.
                // Multiple instances of collect may run concurrently and safely,
                // more often than not a single instance will have most if not all
                // nodes to be deleted in its local delete list.
                hazp_delete_node<T>* local_delete_head = __atomic_exchange_n(
                        &delete_head, nullptr,  __ATOMIC_ACQ_REL);

                hazard_pointers_snapshot<T>  hps(*this);

                // Now check each node for safe deletion (no live pointers to
                // the node in the snapshot (hpvalues)), and delete if possible.
                hazp_delete_node<T>** pprev = &local_delete_head;
                while(nullptr != *pprev)
                {
                    hazp_delete_node<T>* cur = *pprev;
                    if (!hps.search(cur->payload))
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

                // put nodes that could not be deleted back on the shared list.
                while(nullptr != local_delete_head)
                {
                    auto del_entry = local_delete_head;
                    local_delete_head = local_delete_head->next;
                    push_delete_node(del_entry);
                }
            }
        };

        /// Takes a snapshot of the hazard pointers in a domain at a given
        /// point in time.
        template <typename T> class hazard_pointers_snapshot
        {
            const hazp_chunk<T>* pools;
            //TODO: try a version with vectors to measure performance.
            T** ptrvalues = nullptr;
            T** begin = nullptr;
            T** end = nullptr;
            unsigned size = 0;
            public:
                hazard_pointers_snapshot(hazard_pointer_domain<T>& domain):pools(domain.pools_head)
                {
                    // snapshot the pools, by copying the head pointer.
                    // pools are not deleted, and new pools are added to the start
                    // of the list.
                    for(auto p = pools; nullptr != p; p=p->next)
                    {
                        size += p->count();
                    }
                    ptrvalues = new T*[size];
                    // Then copy that number of pointers from the pools,
                    // if new pools have been added since the snapshot of the count,
                    // those values cannot be of interest in the snapshot *because*
                    // new pointers to deleted items cannot be created.
                    unsigned count = 0;
                    for(auto p = pools; nullptr != p; p = p->next)
                    {
                        count += p->copy_hazard_pointers(ptrvalues + count, p->count());
                    }
                    assert(count == size);
                    end = ptrvalues + size;
                    begin = ptrvalues;
                    std::sort(begin, end);
                    // move begin to the last entry with a nullptr value
                    // if it exists.
                    // variation on binary search.
                    do
                    {
                        unsigned halfc = count >> 1;
                        if(begin[halfc] == nullptr)
                        {
                            begin += halfc;
                            count -= halfc;
                        }
                        else
                        {
                            count = halfc;
                        }
                    }while(count > 1);
                }

                ~hazard_pointers_snapshot()
                {
                    delete [] ptrvalues;
                }

                inline bool search(T* ptr)
                {
                    return std::binary_search(begin, end, ptr);
                }
        };

        /// To use hazard pointers in a hazard_pointer_domain,
        /// create an instance of hazard_pointer_context.
        /// This class is designed for use by a single thread.
        /// It implements the SMR algorithm described by Maged Micheal,
        /// in "Safe Memory Reclamation for Dynamic Lock-Free Objects
        /// Using Atomic Reads and Write".
        /// The implementation is not verbatim.
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

            /// Safely delete an object or schedule the object deletion.
            void delete_item(T* item_ptr)
            {
                assert(del_index <= R);
                deleted[del_index] = item_ptr;
                ++del_index;

                // Number of deleted objects has reached the limit
                // of local storage, attempt to delete.
                if (del_index == R)
                {
                    // overflow
                    reclaim();
                }
            }

            /// Safely reclaim storage for deleted objects
            /// or schedule reclamation for deleted object.
            void reclaim()
            {
                hazard_pointers_snapshot<T>  hps(*domain);
                for(unsigned ix=0; ix < R; ++ix)
                {
                    if (!hps.search(deleted[ix]))
                    {
                        // on delete zero out the array field,
                        // and reduce the delete index var.
                        --del_index;
                        delete deleted[ix];
                        deleted[ix] = nullptr;
                    }
                }

                // del_index is not guaranteed to be valid here
                if (del_index == R)
                {
                    // Could not delete anything, so enqueue for delete
                    // on the domain.
                    domain->enqueue_for_delete(deleted, R);
                    del_index = 0;
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
        };
    } //namespace concurrent
} //namespace benedias
#endif // #define BENEDIAS_HAZARD_POINTER_HPP

