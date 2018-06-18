# SplitOrderLists
An implementation of Split-Ordered Lists in C++ as decribed in 

Split-Ordered Lists - Lock-free Resizable Hash Tables
Ori Shalev
Tel Aviv University
and
Nir Shavit
Tel-Aviv University and Sun Microsystems Laboratories

which in turn uses hazard pointers as described in

Safe Memory Reclamation for Dynamic Lock-Free Objects Using Atomic Reads and Writes
Maged M. Michael
IBM Thomas J. Watson Research Center


## Status:
Very much a work in progress.

* hazard pointers have not been implemented (yet).
* functionality tested in a single threaded manner for the moment.

When finished this will be moved to blaisedias/concurrent
