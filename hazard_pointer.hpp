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
///
///  hazard_pointer_domain ---> hazp_chunk(1)->hazp_chunk(2)-......->hazp_chunk(N)
///   |
///   |___________________ hazard_pointer_context(1) (belongs to thread 1)
///   |___________________ hazard_pointer_context(1) (belongs to thread 2)
///   |___________________ hazard_pointer_context(1) (belongs to thread 3)
///   |___________________ hazard_pointer_context(1) (belongs to thread 4)
///
///  Typically a single instance of hazard_pointer_domain<T> is bound to a container of type T,
///  every thread that needs to access the container creates a hazard_pointer_context<T> bound
///  to the hazard_pointer_domain<T> instance.
///  Each hazard_pointer_context instance reserves blocks of hazard pointers by
///  requesting the hazard_pointer_domain<T>, which in turn uses or creates hazp_chunk<T>
///  of matching block size to fulfil the request.
///  The lifetime of hazp_chunk<T> is bound to the hazard_pointer_domain,
///     creation is always after creation of the hazard_pointer_domain
///     destruction is when the hazard_pointer_domain is destroyed.
///  The lifetime of the hazard_pointer_context<T> instances lies within the 
///  lifetime of the hazard_pointer_domain<T> instance.
///     creation is always after creation of the hazard_pointer_domain
///     destruction is always before the destruction of the hazard_pointer_domain
///  The primary functions of the hazard_pointer_domain are
///     *) management of hazard pointer allocation to hazard_pointer_contexts
///     *) handling of hazard pointer deletion on overflow
///     *) handling of hazard pointer deletion after destruction of a hazard_pointer_context.
///     *) facilitate creation of snapshots of the set hazard pointer values, for
///          deletion checking.
///
///  hazard_pointer_context<T> instances can be created and destroyed as required, this
///  flexibility is intended to make code accessing the containers similar to standard containers.
///  The pathological case where the number of items deleted exceeds the 
///  delete array size, and none can be deleted due to liveness, is handled by queueing
///  deletions onto the domain delete list.
///
///  The tradeoffs are
///         amortisation cost may not be constant.
///         deletion is probably more expensive than when using a simple array of hazard pointers.
///         complexity of managing pools of hazard pointers
///         memory fences used for hazard pointer chunk pool, so there is a performance
///          hit on creation of hazard pointer chunk.
///         memory fences used for delete lists on the hazard_pointer_domain, so there
///          is a performance hit on deletes performed at the domain level.
///         the pool of hazard pointers only ever grows, it never shrinks.
///
///  It is possible to combine the deletions across all threads, by specifying the 
///     delete queue length as 0.
///     All deletions will be queued on to the hazard pointer domain instance.
///     The cost is memory required to queue the deletion and fences required for
///     thread safe queueing.
///     However this increases the possibility of keeping the delete amortisation cost
///     constant, actual deletions are only performed when the number of deletions is
///      > than the total number of hazard pointers.
///
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
            static constexpr std::size_t  NUM_HAZP_CHUNK_BLOCKS=sizeof(bitmap)*8;
            static constexpr uint32_t  FULL = UINT32_MAX;

            const std::size_t  blk_size;
            const std::size_t  hp_count;

            /// Constructor
            /// \@param blocksize the granularity of hazard pointer allocation desried.
            hazp_chunk_generic(std::size_t blocksize):blk_size(blocksize),hp_count(blocksize * NUM_HAZP_CHUNK_BLOCKS)
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
            std::size_t copy_hazard_pointers(generic_hazptr_t *dest, std::size_t count) const
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
            generic_hazptr_t* reserve_impl(std::size_t len)
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
                for(std::size_t x = 0; x < blk_size; x++)
                {
                    if (nullptr != *(ptr +x)) 
                        __atomic_store_n(ptr + x, 0x0, __ATOMIC_RELEASE);
                }
                for(std::size_t ix = 0; ix < NUM_HAZP_CHUNK_BLOCKS; ++ix)
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
            /// This function is a helper function, intended for checking at destruction
            /// when it can be "safely assumed" that further reservations will not
            /// be made.
            /// \@return true if there are active reservations within the chunk.
            inline bool has_reservations()
            {
                return 0 != bitmap;
            }

            inline std::size_t size()
            {
                return hp_count;
            }
        };

        /// This class is a type wrapper for the generic hazard pool class,
        /// allows us to keep all "dangerous" casting restricted, as opposed to
        /// peppering the code base.
        /// It is also has fields to facilitate chaining of hazard pointer chunks
        /// into a list.
        template <typename T> struct hazp_chunk:hazp_chunk_generic
        {
            // Pools of hazard pointers can be chained.
            hazp_chunk<T> *next = nullptr;

            hazp_chunk(std::size_t blk_size):hazp_chunk_generic(blk_size){}
            ~hazp_chunk()=default;

            using hazp_chunk_generic::has_reservations;
            using hazp_chunk_generic::size;

            inline std::size_t block_size()
            {
                return hazp_chunk_generic::blk_size;
            }

            inline std::size_t count() const
            {
                return hazp_chunk_generic::hp_count;
            }

            inline T** reserve(std::size_t len)
            {
                return reinterpret_cast<T**>(hazp_chunk_generic::reserve_impl(len));
            }

            inline bool release(T** ptr)
            {
                return hazp_chunk_generic::release_impl(reinterpret_cast<generic_hazptr_t*>(ptr));
            }

            inline std::size_t copy_hazard_pointers(T** dest, std::size_t num) const
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

        // constexpr uintptr_t mark_bits_maskoff = ~1;

        /// Takes a snapshot of the hazard pointers in a domain at a given
        /// point in time.
        template <typename T> class hazard_pointers_snapshot
        {
            const hazp_chunk<T>* pools;
            //TODO: try a version with vectors to measure performance.
            T** ptrvalues = nullptr;
            T** begin = nullptr;
            T** end = nullptr;
            std::size_t size = 0;

            void reset()
            {
                ptrvalues = begin = end = nullptr;
                size = 0;
                pools = nullptr;
            }

            public:
                // Partially movable
                hazard_pointers_snapshot(hazard_pointers_snapshot<T>&& other)
                {
                    pools = std::move(other.pools);
                    ptrvalues = std::move(other.ptrvalues);
                    begin = std::move(other.begin);
                    end = std::move(other.end);
                    size = std::move(other.size);
                    other.reset();
                }

                hazard_pointers_snapshot& operator=(const hazard_pointers_snapshot<T>&& other)=delete;

                hazard_pointers_snapshot(const hazp_chunk<T>* pools_head):pools(pools_head)
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
                    std::size_t count = 0;
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

                    /// TODO: only if underlying type is an instance of mark_ptr_type
                    for(uintptr_t* p = reinterpret_cast<uintptr_t*>(begin);
                            p < reinterpret_cast<uintptr_t*>(end);
                            ++p)
                    {
                        *p &= mark_bits_maskoff;
                    }
                }

                ~hazard_pointers_snapshot()
                {
                    if (nullptr != ptrvalues)
                    {
                        delete [] ptrvalues;
                    }
                }

                inline bool search(T* ptr)
                {
                    return std::binary_search(begin, end, ptr);
                }
        };


        /// A hazard pointer domain defines the set of pointers protected
        /// and checked against for safe memory reclamation.
        /// Typically a hazard pointer domain instance will be associated with
        /// a single instance of a container class.
        template <typename T> class hazard_pointer_domain
        {
            private:
            /// linked lists for the pool of hazard pointer.
            /// For lock-free operation this list is only ever added to
            /// over the lifetime of the domain.
            hazp_chunk<T>* pools_head = nullptr;

            // domain hazard pointer count
            int    hp_count=0;

            /// list of delete nodes, overflow from hazard_pointer_context instances,
            /// or no longer in a hazard_pointer_context scope (the
            /// hazard_pointer_context instance was destroyed, but deletes were
            /// pending).
            /// To keep operations lock-free, this list is only added to,
            /// by inserting at the head of the list atomically,
            /// Or atomically swapped out with an empty list at processing time,
            /// (see collect).
            hazp_delete_node<T>* delete_head = nullptr;
            // domain pending deletes count.
            // This counter is used trigger collect cycles when the number
            // of deletes exceeds the number of hazard pointers.
            // delete_count and the delete list cannot both
            // be updated  in a single atomic operation.
            // For this reason delete_count is in reality a close approximation
            // of the length of the delete list.
            // For triggering purposes this approximation is considered good enough.
            // Because delete_count is zeroed out before the delete list is 
            // swapped out its value may be higher than the number of pending
            // deletes.
            int    delete_count=0;

            /// For lock-free operation, we push new hazard pointer chunks
            /// to head of the list (pool) atomically.
            void pools_new(hazp_chunk<T>** phead, std::size_t blocklen)
            {
                hazp_chunk<T>* chunk = new hazp_chunk<T>(blocklen);
                do
                {
                    chunk->next = *phead;
                }while(!__atomic_compare_exchange(phead, &chunk->next, &chunk,
                            false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED));
                hp_count += chunk->size(); 
            }

            /// Attempt to fulfil a reservation request by requesting
            /// a block from with the pool of hazard pointer chunks.
            /// Reserving hazard pointers is "expensive",
            /// and is amortised at hazard pointer context creation,
            /// which is not expected to be frequent operation.
            /// \@param head - start of pool of hazard pointer chunks.
            /// \@param blocklen - number of hazard pointers required
            /// \@return - nullptr or pointer to the block of hazard pointers.
            T** pools_reserve(hazp_chunk<T>* head, std::size_t blocklen)
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

            hazard_pointer_domain()=default;

            /// Since the instances of domain pointers are accessible, 
            /// are only accessible via shared pointers, this should
            /// be run when the last live reference (shared pointer)
            /// is destroyed.
            ~hazard_pointer_domain()
            {
                // The domain is being destroyed, so all items scheduled for delete
                // should be deleted.
                collect();

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

            public:
            /// Create a hazard pointer domain object. 
            /// The return type is std::shared ptr for safe access across
            /// multiple thread scopes.
            /// \return shared pointer to the domain object.
            static std::shared_ptr<hazard_pointer_domain<T>> make()
            {
                // This round about way, to ensure that the lifetime of
                // hazard pointer domain objects exceeds the lifetime of
                // all associated hazard_pointer_context objects, so
                // prevent access to the constructors and destructors.
                struct makeT:public hazard_pointer_domain<T> {};
                return std::make_shared<makeT>();
            }

            /// Fulfil a reservation request using the pool of hazard pointer chunks
            /// creating new hazard pointer chunks if required.
            /// \@param blocklen - the number hazard pointers required.
            T** reserve(std::size_t blocklen)
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
                __atomic_add_fetch(&delete_count, 1, __ATOMIC_RELEASE);
            }

            /// Add a pointer to the delete list.
            /// Creates and pushes a delete node onto the delete list,
            /// lock free and wait free.
            inline void enqueue_for_delete(T* item_ptr, bool can_collect=true)
            {
                auto del_entry = new hazp_delete_node<T>(item_ptr);
                push_delete_node(del_entry);

                if (can_collect && delete_count > hp_count)
                {
                    // zero the delete count to reduce triggering multiple concurrent
                    // collect cycles.
                    // preemptive scheduling means that it is possible,
                    // that multiple threads enter collect cycles concurrently,
                    // because the check and clearing of the delete counter
                    // are not atomic. 
                    // Multiple collect cycles can be run concurrently without compromising
                    // integrity, but this would be more expensive than it should be.
                    __atomic_store_n(&delete_count, 0, __ATOMIC_RELEASE);
                    collect();
                }
            }

            /// Add a set of pointers to the delete list.
            /// Creates and pushes a delete nodes onto the delete list,
            /// lock free and wait free.
            void enqueue_for_delete(T** items_ptr, std::size_t count, bool can_collect=true)
            {
                for(std::size_t x = 0; x < count; ++x)
                {
                    if (nullptr != items_ptr[x])
                    {
                        push_delete_node(new hazp_delete_node<T>(items_ptr[x]));
                        items_ptr[x] = nullptr;
                    }
                }

                if (can_collect && delete_count > hp_count)
                {
                    // zero the delete count to reduce triggering multiple concurrent
                    // collect cycles.
                    // preemptive scheduling means that it is possible,
                    // that multiple threads enter collect cycles concurrently,
                    // because the check and clearing of the delete counter
                    // are not atomic. 
                    // Multiple collect cycles can be run concurrently without compromising
                    // integrity, but this would be more expensive than it should be.
                    __atomic_store_n(&delete_count, 0, __ATOMIC_RELEASE);
                    collect();
                }
            }

            /// Delete objects on the delete list if no live pointers to
            /// those objects exist.
            /// Serialising the execution of this function, is not required,
            /// because ownership of the shared delete list, is transferred
            /// atomically to thread running the collect function.
            void collect()
            {
                // swap the shared delete list with the empty local delete
                // list, which is only accessed by the this function instance.
                // Multiple instances of collect may run concurrently and safely,
                // more often than not a single instance will have most if not all
                // nodes to be deleted in its local delete list.
                hazp_delete_node<T>* local_delete_head = __atomic_exchange_n(
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
                hazp_delete_node<T>** pprev = &local_delete_head;
                if(nullptr == *pprev)
                {
                    // nothing to do.
                    return;
                }
                pprev = &local_delete_head;
                hazard_pointers_snapshot<T>  hps(pools_head);
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

            inline hazard_pointers_snapshot<T> snapshot()
            {
                return std::move(hazard_pointers_snapshot<T>(pools_head));
            }
        };

        template <typename T> class hazard_pointer
        {
            private:
                T*  ptr;
                // Non copyable.
                hazard_pointer(const hazard_pointer&) = delete;
                hazard_pointer& operator=(const hazard_pointer&) = delete;
                // Non movable.
                hazard_pointer(hazard_pointer&& other) = delete;
                hazard_pointer& operator=(const hazard_pointer&&) = delete;

                hazard_pointer()
                {
                    __atomic_store_n(&ptr, nullptr, __ATOMIC_RELEASE);
                }
                ~hazard_pointer()
                {
                    __atomic_store_n(&ptr, nullptr, __ATOMIC_RELEASE);
                }

                // Hazard pointers can never be instantiated on the heap.
                void* operator new(std::size_t size) = delete;
                void operator delete(void *php) = delete;

                // Hazard pointers can only be instantiated at specified memory locations.
                void* operator new(std::size_t size, void* p)
                {
                    return p;
                }

                hazard_pointer& operator=(hazard_pointer<T>* other)
                {
                    __atomic_store(&ptr, &other->ptr, __ATOMIC_RELEASE);
                    return *this;
                }

            template <typename U, std::size_t US, std::size_t UR> friend class hazard_pointer_context;

            public:
                hazard_pointer& operator=(T* nptr)
                {
                    __atomic_store_n(&ptr, nptr, __ATOMIC_RELEASE);
                    return *this;
                }

                hazard_pointer& operator=(T** pptr)
                {
                    __atomic_store(&ptr, pptr, __ATOMIC_RELEASE);
                    return *this;
                }

                T* operator()() const
                {
                    return ptr;
                }

                T* operator->() const
                {
                    return ptr;
                }

                T operator*() const
                {
                    return *ptr;
                }

        };

        /// This class encapsulates the execution context required for 
        /// use of hazard_pointers by a single thread.
        /// It implements the SMR algorithm described by Maged Micheal,
        /// in "Safe Memory Reclamation for Dynamic Lock-Free Objects
        /// Using Atomic Reads and Write".
        /// The implementation is not verbatim.
        template <typename T, std::size_t S, std::size_t R> class hazard_pointer_context
        {
            private:
            std::shared_ptr<hazard_pointer_domain<T>> domain;
            T* deleted[R]={};
            std::size_t del_index=0;
            hazard_pointer<T>*const hazard_ptrs;

            public:
            /// Number of hazard pointers in the array.
            const std::size_t size;

            // Non copyable.
            hazard_pointer_context(const hazard_pointer_context&) = delete;
            hazard_pointer_context& operator=(const hazard_pointer_context&) = delete;

            hazard_pointer_context& operator=(const hazard_pointer_context&& other)=delete;
            // Partially movable, to allow returning of hazard_pointer_context objects.
            hazard_pointer_context(hazard_pointer_context<T,S,R>&& other):
                domain(std::move(other.domain)), hazard_ptrs(std::move(other.hazard_ptrs)),size(std::move(other.size))
            {
                for(unsigned i=0; i < R; ++i)
                    deleted[i] = std::move(other.deleted[i]);
                del_index = std::move(other.del_index);
            }

            hazard_pointer_context(std::shared_ptr<hazard_pointer_domain<T>> dom):
                domain(dom), hazard_ptrs(reinterpret_cast<hazard_pointer<T>* const>(domain->reserve(S))), size(S)
            {
                //FIXME: throw exception.
                assert(hazard_ptrs != nullptr);
                for(std::size_t i = 0; i < S; ++i)
                {
                    hazard_ptrs[i] = new (hazard_ptrs + i) hazard_pointer<T>();
                }
            }

            ~hazard_pointer_context()
            {
                // we support partial move constructor, so maybe no domain.
                if (domain)
                {
                    for(std::size_t i = 0; i < S; ++i)
                    {
                        hazard_ptrs[i].~hazard_pointer<T>();
                    }
                    // Release the hazard pointers
                    domain->release(reinterpret_cast<T**>(hazard_ptrs));
                    // Delegate deletion of nodes to be deleted
                    // to the domain.
                    domain->enqueue_for_delete(deleted, R);
                    domain->collect();
                }
            }

            inline hazard_pointer<T>* const hazard_pointers() const
            {
                return hazard_ptrs;
            }

            /// Safely delete an object or schedule the object deletion.
            void delete_item(T* item_ptr)
            {
                if (R > 0)
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
                else
                {
                    domain->enqueue_for_delete(item_ptr);
                }
            }

            /// Safely reclaim storage for deleted objects
            /// or schedule reclamation for deleted object.
            void reclaim()
            {
                hazard_pointers_snapshot<T>  hps = domain->snapshot();
                for(std::size_t ix=0; ix < R; ++ix)
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
                    std::size_t ixd=0, ixs=R-1;
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

            T* store(std::size_t index, T** pptr)
            {
                assert(index < size);
                hazard_ptrs[index] = pptr;
                return hazard_ptrs[index]();
            }

            void store(std::size_t index, T* ptr)
            {
                assert(index < size);
                hazard_ptrs[index] = ptr;
            }

            T* at(std::size_t index)
            {
                assert(index < size);
                return hazard_ptrs[index]();
            }
        };
        static_assert(sizeof(hazard_pointer<void>) == sizeof(generic_hazptr_t),
               "sizeof hazard_pointer class does not match preallocated storage.");

        // Experimental class to demonstrate how a container class should handle
        // creation of associated hazard_pointer_context objects.
        template <typename T, std::size_t S, std::size_t R> class hazard_pointer_assoc
        {
            std::shared_ptr<hazard_pointer_domain<T>> dom = hazard_pointer_domain<T>::make();
            public:
            hazard_pointer_context<T, S, R> context()
            {
                return std::move(hazard_pointer_context<T, S, R>(dom));
            }
        };
    } //namespace concurrent
} //namespace benedias
#endif // #define BENEDIAS_HAZARD_POINTER_HPP

