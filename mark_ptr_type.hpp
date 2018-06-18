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

template <typename T> union union_ptr_uintptr_t 
{
    T*  ptr;
    uintptr_t   v;

    union_ptr_uintptr_t():ptr(nullptr) {}
    explicit union_ptr_uintptr_t(T* p):ptr(p) {}
};


template <typename T> class mark_ptr_type
{
    private:
        union_ptr_uintptr_t<T>   ptr_v;
    public:

    inline void operator=(T* p)
    {
        ptr_v.ptr = p;
        assert(0 == (ptr_v.v & mark_bits_mask));
    }

    inline T* operator()(bool *mark)
    {
        mark = 0 != (ptr_v.v & mark_bits_mask);
        return reinterpret_cast<T*>(ptr_v.v & mark_bits_maskoff);
    }

    inline T* operator()()
    {
        return reinterpret_cast<T*>(ptr_v.v & mark_bits_maskoff);
    }

    inline T* operator->()
    {
        return reinterpret_cast<T*>(ptr_v.v & mark_bits_maskoff);
    }

    explicit mark_ptr_type()
    {
    }

    explicit mark_ptr_type(T* p)
    {
        ptr_v.ptr = p;
        assert(0 == (ptr_v.v & mark_bits_mask));
    }

    inline bool CAS(T* expected, T* desired)
    {
        union_ptr_uintptr_t<T> pv_expected(expected);
        union_ptr_uintptr_t<T> pv_desired(desired);
        return __atomic_compare_exchange(&ptr_v.v, &pv_expected.v, &pv_desired.v,
                   false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
    }

    inline bool CAS(T* expected, T* desired, bool mark)
    {
        union_ptr_uintptr_t<T> pv_expected(expected);
        union_ptr_uintptr_t<T> pv_desired(desired);
        if (mark)
        {
            pv_desired.v |= mark_bits_mask;
        }
        return __atomic_compare_exchange(&ptr_v.v, &pv_expected.v, &pv_desired.v,
                   false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
    }

    inline bool CAS(T* expected, bool marked, T* desired, bool mark)
    {
        union_ptr_uintptr_t<T> pv_expected(expected);
        union_ptr_uintptr_t<T> pv_desired(desired);
        if (marked)
        {
            pv_expected.v |= mark_bits_mask;
        }
        if (mark)
        {
            pv_desired.v |= mark_bits_mask;
        }
        return __atomic_compare_exchange(&ptr_v.v, &pv_expected.v, &pv_desired.v,
                   false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
    }

    ~mark_ptr_type() = default;
};

} // namespace concurrent
} // namespace benedias
#endif // _MARK_PTR_TYPE_HPP_INCLUDED

