/*

Copyright (C) 2017-2019  Blaise Dias

This file is free software: you can redistribute it and/or modify
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
#include <mutex>
#include "hazard_pointer.hpp"

namespace benedias {
    namespace concurrent {

static std::once_flag   init_flag;
static void initialise()
{
}

void hazard_pointer_global_init()
{
    try
    {
        std::call_once(init_flag, initialise);
    }
    catch (...)
    {
        // FIXME: 
        assert(false);
    }
}

// hazptr_pool member functions
        hazptr_pool::hazptr_pool(std::size_t blocksize):blk_size(blocksize),hp_count(blocksize * HAZPTR_POOL_BLOCKS)
        {
            // This allocation can be blocking, it will be called as part of the 
            // setup for hazard_pointer_context.
            haz_ptrs = new generic_hazptr_t[hp_count];
            std::fill(haz_ptrs, haz_ptrs + hp_count, nullptr);
        }        

        hazptr_pool::~hazptr_pool()
        {
            delete [] haz_ptrs;
        }

        std::size_t hazptr_pool::copy_hazard_pointers(generic_hazptr_t *dest, std::size_t count) const
        {
            //copy must be of the whole pool
            assert(count >= hp_count);
#if 0
#if 0
            std::copy(haz_ptrs, haz_ptrs + hp_count, dest);
            return hp_count;
#else
            generic_hazptr_t *dest_end
                = std::copy_if(haz_ptrs, haz_ptrs + hp_count, dest,
                        [](generic_hazptr_t sp){return nullptr !=sp;});
            return dest_end - dest;
#endif
#endif
            std::size_t ix_dst = 0;
            for(std::size_t ix_src = 0; ix_src < hp_count; ++ix_src)
            {
                generic_hazptr_t p = __atomic_load_n(haz_ptrs + ix_src, __ATOMIC_ACQUIRE);
                if (nullptr != p)
                {
                    dest[ix_dst] = p;
                    ++ix_dst;
                }
            }
            return ix_dst;
        }

        generic_hazptr_t* hazptr_pool::reserve_impl(std::size_t len)
        {
            uint32_t    mask=1;
            uint32_t    ix=0;
            generic_hazptr_t*  reserved = nullptr;
            uint32_t    expected = bitmap;

            if (len != blk_size)
                return nullptr;
            while (expected != HAZPTR_POOL_BITMAP_FULL && nullptr == reserved)
            {
                mask = 1;
                ix = 0;
                while (0 != (expected & mask) && ix < HAZPTR_POOL_BLOCKS)
                {
                    mask <<= 1;
                    ++ix;
                }

                if (ix < HAZPTR_POOL_BLOCKS)
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
        /// block of hazard pointers contained in this pool.
        /// \@param pointer to the first hazard pointer in the block.
        /// \@return true if hazard pointers were released, false otherwise.
        bool hazptr_pool::release_impl(generic_hazptr_t* ptr)
        {
            // To facilitate clients walking down a list of hazard pointer
            // pools and invoking release until the correct pool instance
            // actually releases the block, check address range, return
            // false if the block does not belong to this pool instance.
            if (ptr < &haz_ptrs[0] || ptr >= &haz_ptrs[hp_count])
                return false;

            uint32_t    mask=1;
            for(std::size_t x = 0; x < blk_size; x++)
            {
                if (nullptr != *(ptr +x)) 
                    __atomic_store_n(ptr + x, 0x0, __ATOMIC_RELEASE);
            }
            for(std::size_t ix = 0; ix < HAZPTR_POOL_BLOCKS; ++ix)
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

// hazptrs_snapshot member functions.
        void hazptrs_snapshot::reset()
        {
            ptrvalues = begin = end = nullptr;
            size = 0;
            pools = nullptr;
        }

        // Partially movable
        hazptrs_snapshot::hazptrs_snapshot(hazptrs_snapshot&& other)
        {
            pools = std::move(other.pools);
            ptrvalues = std::move(other.ptrvalues);
            begin = std::move(other.begin);
            end = std::move(other.end);
            size = std::move(other.size);
            other.reset();
        }


        hazptrs_snapshot::hazptrs_snapshot(const hazptr_pool* pools_head):pools(pools_head)
        {
            // Snapshot the pool of hazard pointer pools,
            // by copying the head pointer of the pool.
            // 1. pools are not deleted from pools,
            // 2. new pools are are added to pools at the start
            // of the list. 
            // So it is safe to iterate of the list as we have it now,
            // and calcuate the number of hazard pointers.
            for(auto p = pools; nullptr != p; p=p->next)
            {
                size += p->count();
            }
            //FIXME: use lockless allocator, especially for inline
            // collect cycles. If collection is restricted to a thread
            // dedicated for that purpose then we can use the std allocator.
            ptrvalues = new generic_hazptr_t[size];
            // Then copy that number of pointers from the pools,
            // if new pools have been added since the snapshot of the count,
            // those values cannot be of interest in the snapshot *because*
            // new pointers to deleted items *will not* be created.
            std::size_t count = 0;
            for(auto p = pools; nullptr != p; p = p->next)
            {
                count += p->copy_hazard_pointers(ptrvalues + count, p->count());
            }
            assert(count <= size);
            end = ptrvalues + count;
            begin = ptrvalues;
            std::sort(begin, end);
#if 0
            // move begin to the last entry with a nullptr value
            // if it exists.
            // variation on binary search.
            do
            {
                std::size_t halfc = count >> 1;
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
#endif
            /// TODO: only if underlying type is an instance of mark_ptr_type
            for(uintptr_t* p = reinterpret_cast<uintptr_t*>(begin);
                    p < reinterpret_cast<uintptr_t*>(end);
                    ++p)
            {
                *p &= mark_bits_maskoff;
            }
        }

        hazptrs_snapshot::~hazptrs_snapshot()
        {
            if (nullptr != ptrvalues)
            {
                delete [] ptrvalues;
            }
        }

//hazptr_domain member functions
            /// For lock-free operation, we push new hazard pointer pools
            /// to head of the list (pool) atomically.
            /// Note that allocation of the hazptr pool may block,
            /// but that will not affect concurrent operations.
            void hazptr_domain::pools_new(hazptr_pool** phead, std::size_t blocklen)
            {
                hazptr_pool* pool = new hazptr_pool(blocklen);
                do
                {
                    pool->next = *phead;
                }while(!__atomic_compare_exchange(phead, &pool->next, &pool,
                            false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED));
                hp_count += pool->count(); 
            }

            /// Attempt to fulfil a reservation request by requesting
            /// a block from with the set of hazard pointer pools.
            /// Reserving hazard pointers is "expensive",
            /// and occurs at hazard pointer context creation.
            /// This performance hit can be amortised using a lock free pool
            /// or cache, of hazard pointer contexts, this will increase memory
            /// usage.
            /// \@param head - start of pool of hazard pointer pools.
            /// \@param blocklen - number of hazard pointers required
            /// \@return - nullptr or pointer to the block of hazard pointers.
            generic_hazptr_t* hazptr_domain::pools_reserve(hazptr_pool* head, std::size_t blocklen)
            {
                generic_hazptr_t* reservation = nullptr;
                for(auto p = head; nullptr != p && nullptr == reservation; )
                {
                    reservation = p->reserve_impl(blocklen);
                }
                return reservation;
            }

            /// Release previously reserved hazard pointer reservations.
            /// Releasing hazard pointers is "expensive",
            /// and occurs at hazard pointer context creation.
            /// This performance hit can be amortised using a lock free pool
            /// or cache, of hazard pointer contexts, this will increase memory
            /// usage.
            /// \@param head - pointer to list of hazard pointer blocks.
            /// \@param ptr - hazard pointer block to be released.
            bool hazptr_domain::pools_release(hazptr_pool* head, generic_hazptr_t* ptr)
            {
                bool released = false;
                for(auto p = head; nullptr != p && !released; )
                {
                    auto pnext = p->next;
                    released = p->release_impl(ptr);
                    p = pnext;
                }
                return released;
            }

            /// Since the instances of domain pointers are only accessible, 
            /// through shared pointers, this will be run when the last live
            /// reference (shared pointer) to this domain is destroyed.
            hazptr_domain::~hazptr_domain()
            {
                // The domain is being destroyed, so all items scheduled for delete
                // should be deleted first.
//                collect(); type agnostic so we cannot realistically delete anything

                //This class is type agnostic, so cannot delete objects pointed to,
                //however we have restricted construction to the hazard_pointer_domain
                //which always run a collect cycle on destruction.

                assert(nullptr == delete_head);
                //Deep hole :-(.
                //Throw exception if all items have not been destroyed instead of assert?

                // Delete all pools.
                for(hazptr_pool* p = __atomic_exchange_n(
                        &pools_head, nullptr,  __ATOMIC_ACQ_REL); nullptr != p; )
                {
                    assert(!p->has_reservations());
                    auto pnext = p->next;
                    delete p;
                    p = pnext;
                }
            }

            /// Push a delete node onto the delete list, lock free and wait free.
            void hazptr_domain::push_delete_node(hazp_delete_node* del_node)
            {
                hazp_delete_node* desired = del_node;
                do
                {
                    // Theoretically we only need to do this first time,
                    // as the "expected" field is updated to the current
                    // value on failed __atomic_compare_exchange calls.
                    del_node->next = delete_head;
                }
                while(!__atomic_compare_exchange(&delete_head, &del_node->next, &desired,
                            false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED));
                __atomic_add_fetch(&delete_count, 1, __ATOMIC_RELEASE);
            }

            /// Fulfill a reservation request using the set of hazard pointer pools
            /// creating a new instance of hazard pointer pool if required.
            /// \@param blocklen - the number of hazard pointers required.
            generic_hazptr_t* hazptr_domain::reserve(std::size_t blocklen)
            {
                generic_hazptr_t* reservation = pools_reserve(pools_head, blocklen);
                if (nullptr == reservation)
                {
                    pools_new(&pools_head, blocklen);
                    reservation = pools_reserve(pools_head, blocklen);
                }
                assert(nullptr != reservation);
                return reservation;
            }

            /// Add a pointer to the delete list.
            /// Creates and pushes a delete node onto the delete list,
            /// lock free and wait free.
            void hazptr_domain::enqueue_for_delete(generic_hazptr_t item_ptr, domain_reclaimer& reclaimer)
            {
                //FIXME: use non-blocking allocator.
                auto del_entry = new hazp_delete_node(item_ptr, reclaimer);
                push_delete_node(del_entry);
            }

            /// Add a set of pointers to the delete list.
            /// Creates and pushes a delete nodes onto the delete list,
            /// lock free and wait free.
            void hazptr_domain::enqueue_for_delete(generic_hazptr_t* items_ptr,
                    domain_reclaimer& reclaimer, std::size_t count)
            {
                for(std::size_t x = 0; x < count; ++x)
                {
                    if (nullptr != items_ptr[x])
                    {
                        //FIXME: use non-blocking allocator.
                        push_delete_node(new hazp_delete_node(items_ptr[x], reclaimer));
                        items_ptr[x] = nullptr;
                    }
                }
            }

            void hazptr_domain::collect_if_required()
            {
                bool do_collect = false;

                while (delete_count > hp_count)
                {
                    int dc = delete_count;
                    do_collect =  __atomic_compare_exchange_n(
                            &delete_count, &dc, 0,
                            false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
                    // zero the delete count to reduce triggering multiple concurrent
                    // collect cycles.
                    // preemptive scheduling means that it is possible,
                    // that multiple threads enter collect cycles concurrently,
                    // because the check and clearing of the delete counter
                    // are not atomic. 
                    // Multiple collect cycles can be run concurrently without compromising
                    // integrity, but this would be more expensive than it should be.
                }

                if (do_collect)
                    collect();
            }

            /// Delete objects on the delete list if no live pointers to
            /// those objects exist.
            /// Serialising the execution of this function, is not required,
            /// because ownership of the shared delete list, is transferred
            /// atomically to thread running the collect function.
            void hazptr_domain::collect()
            {
                // swap the shared delete list with the empty local delete
                // list, which is only accessed by the this function instance.
                // Multiple instances of collect may run concurrently and safely,
                // more often than not a single instance will have most if not all
                // nodes to be deleted in its local delete list.
                hazp_delete_node* local_delete_head = __atomic_exchange_n(
                        &delete_head, nullptr,  __ATOMIC_ACQ_REL);

                if (nullptr == local_delete_head)
                {
                    // This can happen in the event that collect cycles were
                    // triggered concurrently on different thread, because
                    // the check and clear on the delete count is not atomic.
                    return;
                }

                // Now check each node for safe deletion (no live pointers to
                // the node in the snapshot (hpvalues)), and delete if possible.
                hazp_delete_node** pprev = &local_delete_head;
                if(nullptr == *pprev)
                {
                    // nothing to do.
                    return;
                }
                pprev = &local_delete_head;
                hazptrs_snapshot  hps(pools_head);
                while(nullptr != *pprev)
                {
                    hazp_delete_node* cur = *pprev;
                    if (!hps.search(cur->payload))
                    {
                        // delink
                        *pprev = cur->next;
                        cur->reclaimer.reclaim_object(cur->payload);
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
    }
}
