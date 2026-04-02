CXX = g++
CXXFLAGS = -std=c++20 -pthread -O3 -Wall

TARGET = ratelim
SRC = ratelim.cpp

all: build

build: $(SRC)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(TARGET)

run: build
	./$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all build run clean
