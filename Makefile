CC ?= gcc
CXX ?= g++
CFLAGS ?= -O3 -march=native -Wall -Wextra -D_POSIX_C_SOURCE=200809L
CXXFLAGS ?= -O3 -march=native -Wall -Wextra -std=c++17 \
            -I/usr/include/SuperCollider/common \
            -I/usr/include/SuperCollider/server \
            -I/usr/include/SuperCollider/plugin_interface
LDFLAGS ?= -lscsynth -lraylib -lm -lpthread

TARGET = gesture_conductor
OBJS = gesture_conductor.o sc_engine.o

all: $(TARGET)

gesture_conductor.o: gesture_conductor.c sc_engine.h
	$(CC) $(CFLAGS) -c gesture_conductor.c -o gesture_conductor.o

sc_engine.o: sc_engine.cpp sc_engine.h
	$(CXX) $(CXXFLAGS) -c sc_engine.cpp -o sc_engine.o

$(TARGET): $(OBJS)
	$(CXX) -o $(TARGET) $(OBJS) $(LDFLAGS)

synthdefs: synthdefs.scd
	sclang synthdefs.scd

clean:
	rm -f $(TARGET) $(OBJS)

run: $(TARGET)
	./$(TARGET)

run-headless: $(TARGET)
	./$(TARGET) --headless

run-external: $(TARGET)
	./$(TARGET) --no-sc

.PHONY: all clean run run-headless run-external synthdefs
