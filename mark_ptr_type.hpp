/*

Copyright (C) 2018  Blaise Dias

This file is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This file is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this file.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef _MARK_PTR_TYPE_HPP_INCLUDED
#define _MARK_PTR_TYPE_HPP_INCLUDED

#include <atomic>
#include <cassert>

namespace benedias {
namespace concurrent {

constexpr   uintptr_t   mark_bits_mask=1;
constexpr   uintptr_t   mark_bits_maskoff=~mark_bits_mask;

template <typename T> class mark_ptr_type
{
    private:
        uintptr_t   upv = 0;
    public:

    inline void operator=(T* p)
    {
        upv = reinterpret_cast<uintptr_t>(p) | (upv & mark_bits_mask);
    }

    inline T* operator()(bool *mark)
    {
        *mark = (0 != (upv & mark_bits_mask));
        return reinterpret_cast<T*>(upv & mark_bits_maskoff);
    }

    inline T* operator()()
    {
        return reinterpret_cast<T*>(upv & mark_bits_maskoff);
    }

    inline T* operator->()
    {
        return reinterpret_cast<T*>(upv & mark_bits_maskoff);
    }

    explicit mark_ptr_type()
    {
    }

    explicit mark_ptr_type(T* p)
    {
        upv =  reinterpret_cast<uintptr_t>(p);
    }

    inline bool CAS(T* expected, T* desired)
    {
        uintptr_t pv_expected = reinterpret_cast<uintptr_t>(expected);
        uintptr_t pv_desired = reinterpret_cast<uintptr_t>(desired);
        return __atomic_compare_exchange(&upv, &pv_expected, &pv_desired,
                   false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
    }

    inline bool CAS(T* expected, T* desired, bool mark)
    {
        uintptr_t pv_expected = reinterpret_cast<uintptr_t>(expected);
        uintptr_t pv_desired = reinterpret_cast<uintptr_t>(desired);
        if (mark)
        {
            pv_desired |= mark_bits_mask;
        }
        return __atomic_compare_exchange(&upv, &pv_expected, &pv_desired,
                   false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
    }

    inline bool CAS(T* expected, bool mark)
    {
        uintptr_t pv_expected = reinterpret_cast<uintptr_t>(expected);
        uintptr_t pv_desired = reinterpret_cast<uintptr_t>(expected);
        if (mark)
        {
            pv_desired |= mark_bits_mask;
        }
        return __atomic_compare_exchange(&upv, &pv_expected, &pv_desired,
                   false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
    }

    inline bool mark()
    {
        uintptr_t v = __atomic_fetch_or(&upv, mark_bits_mask, __ATOMIC_ACQ_REL);
        return (0 == (mark_bits_mask & v));
    }

    inline bool CAS(T* expected, bool marked, T* desired, bool mark)
    {
        uintptr_t pv_expected = reinterpret_cast<uintptr_t>(expected);
        uintptr_t pv_desired = reinterpret_cast<uintptr_t>(desired);

        if (marked)
        {
            pv_expected |= mark_bits_mask;
        }

        if (mark)
        {
            pv_desired |= mark_bits_mask;
        }

        return __atomic_compare_exchange(&upv, &pv_expected, &pv_desired,
                   false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
    }
    
    inline void reset()
    {
        upv = 0;
    }

    ~mark_ptr_type() = default;
};

} // namespace concurrent
} // namespace benedias
#endif // _MARK_PTR_TYPE_HPP_INCLUDED

