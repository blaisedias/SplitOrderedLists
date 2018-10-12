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
#include "hazard_pointer.hpp"

namespace benedias {
    namespace concurrent {

        hazp_chunk_generic::hazp_chunk_generic(std::size_t blocksize):blk_size(blocksize),hp_count(blocksize * NUM_HAZP_CHUNK_BLOCKS)
        {
            haz_ptrs = new generic_hazptr_t[hp_count];
            std::fill(haz_ptrs, haz_ptrs + hp_count, nullptr);
        }        

        hazp_chunk_generic::~hazp_chunk_generic()
        {
            delete [] haz_ptrs;
        }

        std::size_t hazp_chunk_generic::copy_hazard_pointers(generic_hazptr_t *dest, std::size_t count) const
        {
            //copy must be of the whole chunk
            assert(count >= hp_count);
            std::copy(haz_ptrs, haz_ptrs + hp_count, dest);
            return hp_count;
        }

        generic_hazptr_t* hazp_chunk_generic::reserve_impl(std::size_t len)
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
        /// block of hazard pointers contained in this chunk.
        /// \@param pointer to the first hazard pointer in the block.
        /// \@return true if hazard pointers were released, false otherwise.
        bool hazp_chunk_generic::release_impl(generic_hazptr_t* ptr)
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
    }
}
