CFLAGS=-I$(RTE_SDK)/include  -std=c++11 -Wall -msse4.2
LDFLAGS=-L$(RTE_SDK)/lib
CXX=clang++
#CXX=g++

all:port_test

port_test:main.o utils.o dpdk.o access_cmdline.o
	$(CXX) -o $@ $^ $(LDFLAGS) -Wl,--whole-archive -ldpdk -Wl,--no-whole-archive -lpthread -ldl

main.o:main.cc
	$(CXX) $^ $(CFLAGS) -o $@ -c
utils.o:utils.cc
	$(CXX) $^ $(CFLAGS) -o $@ -c
dpdk.o:dpdk.cc
	$(CXX) $^ $(CFLAGS) -o $@ -c
access_cmdline.o:access_cmdline.cc
	$(CXX) $^ $(CFLAGS) -o $@ -c

clean:
	rm -rf *.o port_test