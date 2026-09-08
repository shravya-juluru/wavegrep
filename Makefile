# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Shravya Juluru
#
# Makefile for fsdb_query tool
# Links against Synopsys FsdbReader API

VERDI_HOME ?= /tools/Synopsys/verdi/X-2025.06
FSDB_INC   = $(VERDI_HOME)/share/FsdbReader
FSDB_LIB   = $(VERDI_HOME)/share/FsdbReader/LINUX64

CXX      = g++
CXXFLAGS = -I$(FSDB_INC) -m64 -fPIC -DLINUX -O2 -std=c++11
LDFLAGS  = -L$(FSDB_LIB) -lnffr -lnsys -lpthread -ldl -lrt -lm -lz
RPATH    = -Wl,-rpath,$(FSDB_LIB)

TARGET = fsdb_query

all: $(TARGET)

$(TARGET): fsdb_query.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS) $(RPATH)

clean:
	rm -f $(TARGET)

.PHONY: all clean
