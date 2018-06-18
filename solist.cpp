#include "solist.hpp"
#include <iostream>

namespace benedias {
    namespace concurrent {

inline uint32_t brev_classic (uint32_t a)
{
    uint32_t m;
    a = (a >> 16) | (a << 16);                            // swap halfwords
    m = 0x00ff00ff; a = ((a >> 8) & m) | ((a << 8) & ~m); // swap bytes
    m = m^(m << 4); a = ((a >> 4) & m) | ((a << 4) & ~m); // swap nibbles
    m = m^(m << 2); a = ((a >> 2) & m) | ((a << 2) & ~m);
    m = m^(m << 1); a = ((a >> 1) & m) | ((a << 1) & ~m);
    return a;
}

/* Knuth's algorithm from http://www.hackersdelight.org/revisions.pdf. Retrieved 8/19/2015 */
inline uint32_t brev_knuth (uint32_t a)
{
    uint32_t t;
    a = (a << 15) | (a >> 17);
    t = (a ^ (a >> 10)) & 0x003f801f; 
    a = (t + (t << 10)) ^ a;
    t = (a ^ (a >>  4)) & 0x0e038421; 
    a = (t + (t <<  4)) ^ a;
    t = (a ^ (a >>  2)) & 0x22488842; 
    a = (t + (t <<  2)) ^ a;
    return a;
}


hash_t reverse_hasht_bits(hash_t hashv)
{
#if 0
    register SOLH_Key_t kv = kv_in;
    register SOLH_Key_t rkv = 0;
#if 0
    uint32_t msk = 1<<((sizeof(kv) * 8) - 1);
    for(unsigned ix=1; (kv > 1) && (ix < (sizeof(kv) * 8)); ix++)
    {
        if (kv & 2)
            rkv |= msk;
        msk >>= 1;
        kv >>= 1;
    }
#else
    register SOLH_Key_t msk = 1<<((sizeof(kv) * 8) - 2);
    kv >>= 1;
    while(kv)
    {
        if (kv & 1)
            rkv |= msk;
        msk >>= 1;
        kv >>= 1;
    }
#endif
    return rkv;
#else
    // bit 0 is reserved for dummy/data node marking.
    // return brev_knuth(hashv) & 0xfffffffe;
    return brev_knuth(hashv);
#endif
}

    } //namespace concurrent
} //namespace benedias

