CXX = g++
CXXFLAGS = -Wall -Wextra -std=c++17 -I.

TARGET = proxy-server
SRC = main.cpp

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SRC)

clean:
	rm -f $(TARGET)

.PHONY: all clean
