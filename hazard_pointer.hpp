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
///  hazard_pointer_domain ---> hazptr_pool(1)->hazptr_pool(2)-......->hazptr_pool(N)
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
///  requesting the hazard_pointer_domain<T>, which in turn uses or creates hazptr_pool
///  of matching block size to fulfil the request.
///  The lifetime of hazptr_pool instance is bound to the hazard_pointer_domain,
///     creation is always after creation of the hazard_pointer_domain
///     and destruction is when the hazard_pointer_domain is destroyed.
///  The lifetime of the hazard_pointer_context<T> instances lies within the 
///  lifetime of the hazard_pointer_domain<T> instance.
///     creation is always after creation of the hazard_pointer_domain
///     destruction is always before the destruction of the hazard_pointer_domain
///  The primary functions of hazard_pointer_domain are
///     *) management of hazard pointer allocation to hazard_pointer_contexts
///     *) handling of hazard pointer deletion on overflow
///     *) handling of hazard pointer deletion after destruction of a hazard_pointer_context.
///     *) facilitate creation of snapshots of the set hazard pointer values, for
///          deletion checking.
///
///  hazard_pointer_context<T> instances can be created and destroyed as required, the
///  objective is to make code accessing the containers similar if not identical to
///  standard containers.
///
///  Deleted items are "queued" in the hazard_pointer_context in an array of
///  fixed size.
///  The pathological case where the number of items deleted exceeds the 
///  delete array size, and none can be deleted due to liveness, is handled by queueing
///  deletions onto the domain delete list, memory is allocated to queue the deletion,
///  which can result in blocking.
///
///  The tradeoffs 
///         - amortisation cost is not constant.
///         - deletion is more expensive than when using a simple array of hazard pointers.
///         - memory fences used for delete lists on the hazard_pointer_domain, so there
///            is a performance hit on deletes performed at the domain level.
///
///
///  It is possible to combine the deletions across all threads, by specifying the 
///  delete queue length as 0.
///  All deletions will be queued on to the hazard pointer domain instance.
///  The cost is memory required to queue the deletion and fences required for
///  thread safe queueing.
///  However this increases the possibility of keeping the delete amortisation cost
///  constant, actual deletions are only performed when the number of deletions is
///  > than the total number of hazard pointers.
///
///  Notable features of this scheme and implementation
///         - hazard pointer pool creation is linked to creation of hazard pointer 
///             contexts.
///         - The set of hazard pointer pools only ever grows, it never shrinks.
///         - memory fences used to manage the list of hazard pointer pools,
///             so there is a performance hit on creation of a hazard pointer pool
///             instances.
namespace benedias {
    namespace concurrent {
        /// Hazard pointer storage type for generic manipulation of hazard pointers.
        /// The algorithms only use the pointer values and not the contents,
        /// so type is not relevant.
        /// Using a generic type means a single implementation independent of type,
        /// suffices and generates less code.
        typedef void*   generic_hazptr_t;

        /// hazard pointer pool, manages reservation and release of a hazard
        /// pointers in blocks of a size fixed at creation time.
        /// A collection of hazard pointer pools form the set of hazard pointers
        /// for a hazard pointer domain.
        /// The hazard pointer pool class is type agnostic.

        // Instances and hazard pointer storage is allocated using std::allocator,
        // which is blocking, this is not an issue, unless the creation of hazard
        // pointer contexts is required to be non-blocking.
        // TODO: plumbing to use a custom allocator.
        class hazptr_pool {
            private:
            generic_hazptr_t   *haz_ptrs = nullptr;
            // bitmap of reserved hazard pointers (1 bit maps to an "array" of length=blk_size)
            uint32_t    bitmap=0;

            protected:
            /// Number of blocks of hazard pointers in a pool.
            static constexpr std::size_t  HAZPTR_POOL_BLOCKS = sizeof(bitmap)*8;
            static constexpr uint32_t  HAZPTR_POOL_BITMAP_FULL = UINT32_MAX;

            const std::size_t  blk_size;
            const std::size_t  hp_count;

            public:
            // Pools of hazard pointers can be chained.
            hazptr_pool *next = nullptr;

            /// Constructor
            /// \@param blocksize the granularity of hazard pointer allocation desried.
            hazptr_pool(std::size_t blocksize);

            virtual ~hazptr_pool();

            /// Copy the entire set of hazard pointers managed by this instance.
            /// \@param dest - destination buffer
            /// \@prama count - size of the destination buffer.
            std::size_t copy_hazard_pointers(generic_hazptr_t *dest, std::size_t count) const;

            /// lock-free thread safe reservation of blocks of pointers.
            /// reservation will only succeed if the requested length is
            /// matches the block size of the hazard pointer pool, and
            /// there is at least one free block.
            /// \@param len - the number of hazard pointers desired.
            /// \@return pointer to the block of hazard pointers or nullptr.
            generic_hazptr_t* reserve_impl(std::size_t len);

            /// lock-free thread safe release of blocks of pointers.
            /// The pointers are only "released" if supplied pointer is to a
            /// block of hazard pointers contained in this pool.
            /// \@param pointer to the first hazard pointer in the block.
            /// \@return true if hazard pointers were released, false otherwise.
            bool release_impl(generic_hazptr_t* ptr);

            /// Returns simple reservation status for this pool.
            /// This function is a helper function, intended for checking at destruction
            /// when it can be "safely assumed" that further reservations will not
            /// be made.
            /// \@return true if there are active reservations within the pool.
            inline bool has_reservations()
            {
                return 0 != bitmap;
            }

            inline std::size_t count() const
            {
                return hp_count;
            }
        };

        // constexpr uintptr_t mark_bits_maskoff = ~1;

        /// Class to snapshot the set of hazard pointers in a domain at a given
        /// point in time.
        class hazptrs_snapshot
        {
            const hazptr_pool* pools;
            //TODO: try a version with vectors to measure performance.
            generic_hazptr_t* ptrvalues = nullptr;
            generic_hazptr_t* begin = nullptr;
            generic_hazptr_t* end = nullptr;
            std::size_t size = 0;

            // Move helper function.
            void reset();

            public:
                // Partially movable
                hazptrs_snapshot(hazptrs_snapshot&& other);
                hazptrs_snapshot& operator=(const hazptrs_snapshot&& other)=delete;

                hazptrs_snapshot(const hazptr_pool* pools_head);
                virtual ~hazptrs_snapshot();

                template<class T> inline bool search(T* ptr)
                {
                    return std::binary_search(begin, end, reinterpret_cast<generic_hazptr_t>(ptr));
                }
        };

        /// Hazard pointer class template, similar to other 'smart pointers'
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

                void operator delete(void *php) = delete;

                // Hazard pointers can never be instantiated on the heap.
                // Hazard pointers can only be instantiated at specified memory locations.
                void* operator new(std::size_t size) = delete;
                void* operator new(std::size_t size, void* p)
                {
                    return p;
                }

                hazard_pointer& operator=(hazard_pointer<T>* other)
                {
                    __atomic_store(&ptr, &other->ptr, __ATOMIC_RELEASE);
                    return *this;
                }

            template <typename U, class Allocator> friend class hazard_pointer_domain;

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


        /// Abstract class that defines the interface to 
        /// safely reclaim the object pointed to,
        /// using the domain memory allocator.
        struct domain_reclaimer
        {
            virtual void reclaim_object(generic_hazptr_t item_ptr)=0;
        };

        /// A type agnostic hazard pointer domain defines the set of pointers protected
        /// and checked against for safe memory reclamation.
        /// Typically a type agnostic hazard pointer domain instance will be associated
        /// type sensitive hazard pointer domains.
        /// A single instance may be shared across multiple types and containers,
        /// the benefit is that type independent resources can be shared across all
        /// instances of lock free containers and classes.
        class hazptr_domain
        {
            private:
            struct hazp_delete_node
            {
                hazp_delete_node* next;
                generic_hazptr_t payload;
                domain_reclaimer& reclaimer;

                hazp_delete_node(generic_hazptr_t datap, domain_reclaimer& recl):payload(datap),reclaimer(recl) {}
                ~hazp_delete_node()=default;
            };

            /// linked lists for the pool of hazard pointer.
            /// For lock-free operation this list is only ever added to
            /// over the lifetime of the domain.
            hazptr_pool* pools_head = nullptr;

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
            hazp_delete_node* delete_head = nullptr;

            // domain pending deletes count.
            // This counter is used trigger collect cycles when the number
            // of deletes exceeds the number of hazard pointers.
            // delete_count and the delete list cannot both
            // be updated  in a single atomic operation,
            // is in reality delete_count is a close approximation
            // of the length of the delete list.
            // For triggering purposes this approximation is considered good enough.
            // delete_count is zeroed out before the delete list is 
            // swapped out, so its value may be higher than the number of pending
            // deletes.
            int    delete_count=0;

            /// For lock-free operation, we push new hazard pointer pools
            /// to head of the list (pool) atomically.
            void pools_new(hazptr_pool** phead, std::size_t blocklen);

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
            generic_hazptr_t* pools_reserve(hazptr_pool* head, std::size_t blocklen);

            /// Release previously reserved hazard pointer reservations.
            /// Releasing hazard pointers is "expensive",
            /// and occurs at hazard pointer context creation.
            /// This performance hit can be amortised using a lock free pool
            /// or cache, of hazard pointer contexts, this will increase memory
            /// usage.
            /// \@param head - pointer to list of hazard pointer blocks.
            /// \@param ptr - hazard pointer block to be released.
            bool pools_release(hazptr_pool* head, generic_hazptr_t* ptr);

            hazptr_domain()=default;

            /// Since the instances of domain pointers are only accessible, 
            /// through shared pointers, this will be run when the last live
            /// reference (shared pointer) to this domain is destroyed.
            ~hazptr_domain();

            /// Push a delete node onto the delete list, lock free and wait free.
            void push_delete_node(hazp_delete_node* del_node);

            template <typename U, class Allocator> friend class hazard_pointer_domain;

            /// Create a hazard pointer domain object. 
            /// The return type is std::shared ptr for safe access across
            /// multiple thread scopes.
            /// \return shared pointer to the domain object.
            static std::shared_ptr<hazptr_domain> make()
            {
                // This round about way, to ensure that the lifetime of
                // hazard pointer domain objects exceeds the lifetime of
                // all associated hazard_pointer_context objects, so
                // prevent access to the constructors and destructors.
                struct makeT:public hazptr_domain {};
                return std::make_shared<makeT>();
            }

            public:
            /// Fulfill a reservation request using the set of hazard pointer pools
            /// creating a new instance of hazard pointer pool if required.
            /// \@param blocklen - the number of hazard pointers required.
            generic_hazptr_t* reserve(std::size_t blocklen);

            /// Release hazard pointers previously reserved.
            /// \@param ptr - the hazard pointer(s) to be released.
            inline void release(generic_hazptr_t* hps, std::size_t blocklen)
            {
                bool released = pools_release(pools_head, hps);
                assert(released);
            }

            /// Add a pointer to the delete list.
            /// Creates and pushes a delete node onto the delete list,
            /// lock free and wait free.
            void enqueue_for_delete(generic_hazptr_t item_ptr, domain_reclaimer& reclaimer);

            /// Add a set of pointers to the delete list.
            /// Creates and pushes a delete nodes onto the delete list,
            /// lock free and wait free.
            void enqueue_for_delete(generic_hazptr_t* items_ptr, domain_reclaimer& reclaimer, std::size_t count);

            /// Delete objects on the delete list if no live pointers to
            /// those objects exist.
            /// Serialising the execution of this function, is not required,
            /// because ownership of the shared delete list, is transferred
            /// atomically to thread running the collect function.
            void collect();

            void collect_if_required();

            inline hazptrs_snapshot snapshot()
            {
                // std::move prevents copy elision
                // return std::move(hazptrs_snapshot(pools_head));
                return hazptrs_snapshot(pools_head);
            }
        };


        /// A hazard pointer domain defines the set of pointers protected
        /// and checked against for safe memory reclamation.
        /// Typically a hazard pointer domain instance will be associated with
        /// a single instance of a container class, but sharing across multiple
        /// containers of the same type is supported.
        template <typename T, class Allocator=std::allocator<T>> class hazard_pointer_domain: public domain_reclaimer
        {
            std::shared_ptr<hazptr_domain> hp_dom;
            //Allocator used for objects created in this domain.
            Allocator allocatorT;

            private:
            hazard_pointer_domain()
            {
                hp_dom = hazptr_domain::make();
            }

            /// Since the instances of domain pointers are only accessible, 
            /// through shared pointers, this will be run when the last live
            /// reference (shared pointer) to this domain is destroyed.
            ~hazard_pointer_domain()
            {
                // The domain is being destroyed, so all items scheduled for delete
                // should be deleted first.
                hp_dom->collect();
                // FIXME: add a check that there are no items associated with this
                // domain still queued for delete, that would be a bug and 
                // this instance being invoked after destruction,
                // because  delete nodes have reference to this object for
                // memory reclamation.
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

            /// Fulfill a reservation request using the set of hazard pointer pools
            /// creating a new instance of hazard pointer pool if required.
            /// \@param blocklen - the number of hazard pointers required.
            inline hazard_pointer<T>* const reserve(std::size_t blocklen)
            {
                generic_hazptr_t* reservation = hp_dom->reserve(blocklen);
                return new(reinterpret_cast<unsigned char*>(reservation)) hazard_pointer<T>[blocklen];
            }

            /// Release hazard pointers previously reserved.
            /// \@param ptr - the hazard pointer(s) to be released.
            void release(hazard_pointer<T>* const hps, std::size_t blocklen)
            {
                for(std::size_t i = 0; i < blocklen; ++i)
                {
                    hps[i].~hazard_pointer<T>();
                }
                hp_dom->release(reinterpret_cast<generic_hazptr_t*>(hps), blocklen);
            }

            /// Add a pointer to the delete list.
            /// Creates and pushes a delete node onto the delete list,
            /// lock free and wait free.
            inline void enqueue_for_delete(T* item_ptr, bool can_collect=true)
            {
                hp_dom->enqueue_for_delete(reinterpret_cast<generic_hazptr_t>(item_ptr), *this);
                if (can_collect)
                    hp_dom->collect_if_required();
            }

            /// Add a set of pointers to the delete list.
            /// Creates and pushes a delete nodes onto the delete list,
            /// lock free and wait free.
            inline void enqueue_for_delete(T** items_ptr, std::size_t count, bool can_collect=true)
            {
                hp_dom->enqueue_for_delete(reinterpret_cast<generic_hazptr_t*>(items_ptr), *this, count);
                if (can_collect)
                    hp_dom->collect_if_required();
            }

            /// Delete objects on the delete list if no live pointers to
            /// those objects exist.
            /// Serialising the execution of this function, is not required,
            /// because ownership of the shared delete list, is transferred
            /// atomically to thread running the collect function.
            void collect()
            {
                hp_dom->collect();
            }

            /// Run class destructor and free memory allocated for this domain.
            /// this will only be lock-free if the destructor is lock-free and
            /// the allocator is lock-free.
            void reclaim_object(generic_hazptr_t item_ptr)
            {
                T* ptr = reinterpret_cast<T*>(item_ptr);
                ptr->~T();
                allocatorT.deallocate(ptr, 1);
            }

            inline hazptrs_snapshot snapshot()
            {
                return hp_dom->snapshot();
            }
        };

        static_assert(sizeof(hazard_pointer<void>) == sizeof(generic_hazptr_t),
               "sizeof hazard_pointer class does not match preallocated storage.");


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
                domain(dom), hazard_ptrs(domain->reserve(S)), size(S)
            {
                //FIXME: throw exception.
                assert(hazard_ptrs != nullptr);
            }

            ~hazard_pointer_context()
            {
                // we support partial move constructor, so maybe no domain.
                if (domain)
                {
                    // Release the hazard pointers
                    domain->release(hazard_ptrs, S);
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
                hazptrs_snapshot  hps = domain->snapshot();
                for(std::size_t ix=0; ix < R; ++ix)
                {
                    if (!hps.search(deleted[ix]))
                    {
                        // on delete zero out the array field,
                        // and reduce the delete index var.
                        --del_index;
                        domain->reclaim_object(deleted[ix]);
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

        // Demo class to demonstrate how a container class should handle
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

