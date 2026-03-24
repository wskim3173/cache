CXXFLAGS := -g -Wall -std=c++11 -lm
CXX=c++

all: cachesim

cachesim: cachesim.o cachesim_driver.o
	$(CXX) -o cachesim cachesim.o cachesim_driver.o

submit: cachesim
	tar -cvf please_rename_me.tar cachesim.cpp cachesim.hpp

clean:
	rm -f cachesim *.o *.tar
