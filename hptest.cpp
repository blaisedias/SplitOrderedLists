/*

Copyright (C) 2018  Blaise Dias

This file is part of sqzbsrv.

sqzbsrv is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

sqzbsrv is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this file.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "hazard_pointer.hpp"
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <memory>

using   benedias::concurrent::hazard_pointer_domain;
using   benedias::concurrent::hazard_pointer_context;

unsigned scope = 0;
void indent()
{
    unsigned v = scope;
    while(v--)
        std::cout << "\t";
}


struct  B
{
    unsigned v;
    explicit B(unsigned x):v(x)
    {
        indent(); std::cout << "CTOR B " << this << " " << v << std::endl;
    }
    ~B()
    {
#if 0
        // :-( MSAN generates a fault for this, but not the equivalent 
        // sequence of statements below.
        // This is because MSAN requires all libraries linked against,
        // including libc to be built with --fsanitize=memory.
        // For now we will evade that issue, MSAN has been useful.
        indent(); std::cout << "DTOR B " << this << " " << v << std::endl;
#else
        indent(); 
        std::cout << "DTOR B ";
        std::cout << this;
        std::cout << " " << v;
        std::cout << std::endl;
#endif
    }

    friend std::ostream& operator << (std::ostream& ostream, const B& b)
    {
        ostream << b.v << " (" << &b << ") ";
        return ostream;
    }
};

// simple test of bucket initialisation
void test0()
{
indent();std::cout << "hpdom scope start" << std::endl;
    {
        ++scope;
        auto hpdom = hazard_pointer_domain<B>();
indent();std::cout << "hp1 scope start" << std::endl;
        {
            ++scope;
            auto hpc1 = hazard_pointer_context<B, 3, 3>(&hpdom);
            B** hp1 = hpc1.hazard_pointers;
            hp1[0] = new B(1);
            hp1[1] = new B(2);
            hp1[2] = new B(3);
indent();std::cout << "hp1 hazps are " << *hp1[0] << ", " << *hp1[1] << ", " << *hp1[2] << std::endl;
indent();std::cout << "hp2 scope start" << std::endl;
            {
                ++scope;
                auto hpc2 = hazard_pointer_context<B, 3, 3>(&hpdom);
    
                B** hp2 = hpc2.hazard_pointers;
                hp2[0] = new B(4);
indent();std::cout << "hp2 hazps are " << *hp2[0] << std::endl;
                indent();std::cout << "hp2 delete " << *hp1[0] << std::endl;
                hpc2.delete_item(hp1[0]);
                indent();std::cout << "hp2 delete " << *hp1[1] << std::endl;
                hpc2.delete_item(hp1[1]);
                indent();std::cout << "hp2 delete " << *hp1[2] << std::endl;
                hpc2.delete_item(hp1[2]);
                indent();std::cout << "hp2 delete " << *hp2[0] <<  std::endl;
                hpc2.delete_item(hp2[0]);
            }
            --scope;
indent();std::cout << "hp2 scope end" << std::endl;
        }
        --scope;
indent();std::cout << "hp1 scope end" << std::endl;
    }
    --scope;
indent();std::cout << "hpdom scope end" << std::endl;
}


int main( int argc, char* argv[] )
{
    std::setlocale(LC_ALL, "en_US.UTF-8");
    std::srand(std::time(nullptr)); // use current time as seed for random generator
    void (*tf)() = test0;
    if (argc > 1)
    {
        switch(*argv[1])
        {
            case '0':
                tf = test0; break;
#if  0                
            case '1':
                tf = test1; break;
            case '2':
                tf = test2; break;
            case '3':
                tf = test3; break;
            case 'x':
                tf = testx; break;
#endif
        }
    }

    tf();
    std::cout << "All Done. " << std::endl;
    return 0;
}
