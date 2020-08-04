
CXX := g++

CXXFLAGS := -std=c++20 -W -Wall -Wshadow -Wextra -Wpedantic -O3 -mtune=native

TARGETS := mpp
all: $(TARGETS)



mpp: mpp.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< -lpthread

.PHONY: clean
clean:
	@rm -f $(TARGETS) *.ii *.bc *.o *.s *~
