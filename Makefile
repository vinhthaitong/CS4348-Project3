CXX = g++
CXXFLAGS = -std=c++17 -pthread -Wall -Wextra -pedantic

.PHONY: all clean
all: pipegrep

pipegrep: pipegrep.cpp
	$(CXX) $(CXXFLAGS) pipegrep.cpp -o pipegrep

clean:
	rm -f pipegrep
