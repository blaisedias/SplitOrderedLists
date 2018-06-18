#
#
TARG_ARCH=$(shell uname -m)

ifeq ($(TARG_ARCH),x86_64)
	LIBDIRS = -L/usr/lib/x86_64-linux-gnu
	TARG_LIBS = 
	TARG_CF = 
endif
ifeq ($(TARG_ARCH),i686)
	LIBDIRS = 
	TARG_LIBS = 
	TARG_CF = 
endif
ifeq ($(TARG_ARCH),armv6l)
	LIBDIRS = -L/usr/lib/arm-linux-gnueabihf
	TARG_LIBS =
	TARG_CF =
endif

DEFS =
SRC = .
INCLUDES = -I .
TESTSRC = test/src
BIN = ./bin
OD = ./obj
LIBS = $(TARG_LIBS)
SANITIZE =  -fsanitize=safe-stack 
SANITIZE =  -fsanitize=address -fno-omit-frame-pointer -fsanitize=undefined
#SANITIZE =  -fsanitize=memory -fno-omit-frame-pointer -fsanitize=undefined
GD = ./Makefile
CF = -std=c++14 -Wall -g $(TARG_CF) $(DEFS) $(SANITIZE)
CC = clang++

OBJS = 	

all: $(BIN)/test1 $(BIN)/test_expansion

.PHONY: clean

clean:
	rm -f $(OD)/*
	rm -f $(BIN)/*

#{
Makefile.deps: $(SRC)/*.cpp
	./mkdeps.py $(SRC) -o '$(OD)'
	touch Makefile.deps

include Makefile.deps
#}
# Alternative makefile way of doing above.
#
#https://www.gnu.org/software/make/manual/make.html#Automatic-Prerequisites
#%.d: %.c
#        @set -e; rm -f $@; \
#         $(CC) -M $(CPPFLAGS) $< > $@.$$$$; \
#         sed 's,\($*\)\.o[ :]*,\1.o $@ : ,g' < $@.$$$$ > $@; \
#         rm -f $@.$$$$
#
#include $(sources:.c=.d)
#
$(OD)/%.o: $(SRC)/%.cpp $(GD) | $(OD)
	$(CC) $(CF) -c -o $(@) $< $(INCLUDES)

$(OD)/%.o: $(SRC)/%.c $(GD) | $(OD)
	$(CC) $(CF) -c -o $(@) $< $(INCLUDES)

$(OD)/%.o: $(TESTSRC)/%.cpp $(GD) | $(OD)
	$(CC) $(CF) -c -o $(@) $< $(INCLUDES)

$(OD)/%.o: $(TESTSRC)/%.c $(GD) | $(OD)
	$(CC) $(CF) -c -o $(@) $< $(INCLUDES)


$(BIN)/test1 : $(OD)/test1.o $(OD)/solist.o | $(BIN)
	$(CC) $(CF) -o $(@) $^ $(LIBDIRS) $(LIBS)

$(BIN)/test_expansion : $(OD)/test_expansion.o $(OD)/solist.o | $(BIN)
	$(CC) $(CF) -o $(@) $^ $(LIBDIRS) $(LIBS)


$(BIN):
	mkdir -p $@

$(OD):
	mkdir -p $@

