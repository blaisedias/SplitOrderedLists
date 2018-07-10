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
using   benedias::concurrent::hazard_pointer;

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
        indent(); std::cout << "CTOR B " << this << ", v=" << v << std::endl;
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
        std::cout << ", v=" << v;
        std::cout << std::endl;
#endif
    }

    friend std::ostream& operator << (std::ostream& ostream, const B& b)
    {
        ostream << b.v << " (" << &b << ") ";
        return ostream;
    }
};

// Simple test of hazard pointer deletion.
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
            B* b1 = new B(1);
            B* b2 = new B(2);
            B* b3 = new B(3);
            hpc1.store(0, b1);
            hpc1.store(1, b2);
            hpc1.store(2, b3);
indent();std::cout << "hp1 hazps are " << hpc1.at(0) << ", " << hpc1.at(1) << ", " << hpc1.at(2) << std::endl;
indent();std::cout << "hp2 scope start" << std::endl;
            {
                ++scope;
                auto hpc2 = hazard_pointer_context<B, 3, 3>(&hpdom);
                B* b4 = new B(4);
                hpc2.store(0, b4);
indent();std::cout << "hp2 hazps are " << hpc1.at(0) << std::endl;
                indent();std::cout << "hp2 delete " << b1 << std::endl;
                hpc2.delete_item(b1);
                indent();std::cout << "hp2 delete " << b2 << std::endl;
                hpc2.delete_item(b2);
                indent();std::cout << "hp2 delete " << b3 << std::endl;
                hpc2.delete_item(b3);
                indent();std::cout << "hp2 delete " << b4 <<  std::endl;
                hpc2.delete_item(b4);
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

// Simple test of hazard pointer deletion.
// using hazard_pointer<T>
void test1()
{
indent();std::cout << "hpdom scope start" << std::endl;
    {
        ++scope;
        auto hpdom = hazard_pointer_domain<B>();
indent();std::cout << "hp1 scope start" << std::endl;
        {
            ++scope;
            auto hpc1 = hazard_pointer_context<B, 3, 6>(&hpdom);
            auto hps1 = hpc1.hazard_ptrs;
            B* b1 = new B(1);
            B* b2 = new B(2);
            B* b3 = new B(3);
            hps1[0] = b1;
            hps1[1] = b2;
            hps1[2] = b3;
indent();std::cout << "hp1 hazps are " << hpc1.at(0) << ", " << hpc1.at(1) << ", " << hpc1.at(2) << std::endl;
indent();std::cout << "hp2 scope start" << std::endl;
            {
                ++scope;
                auto hpc2 = hazard_pointer_context<B, 3, 6>(&hpdom);
                B* b4 = new B(4);
                hpc2.hazard_ptrs[0] = b4;
indent();std::cout << "hp2 hazps are " << hpc1.at(0) << std::endl;
                indent();std::cout << "hp2 delete " << b1 << std::endl;
                hpc2.delete_item(b1);
                indent();std::cout << "hp2 delete " << b2 << std::endl;
                hpc2.delete_item(b2);
                indent();std::cout << "hp2 delete " << b3 << std::endl;
                hpc2.delete_item(b3);
                indent();std::cout << "hp2 delete " << b4 <<  std::endl;
                hpc2.delete_item(b4);
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
    if (argc < 2)
        tf();
    for(int n=1; n < argc; ++n)
    {
        switch(*argv[n])
        {
            case '0':
                test0(); break;
            case '1':
                test1(); break;
#if  0                
            case '2':
                tf = test2; break;
            case '3':
                tf = test3; break;
            case 'x':
                tf = testx; break;
#endif
        }
    }
    std::cout << "All Done. " << std::endl;
    return 0;
}
