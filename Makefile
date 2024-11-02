CXX = g++
CC = gcc
ARCH_FLAGS = -march=native -mtune=native
OPT_FLAGS = -O3 -flto -fno-omit-frame-pointer
COMMON_FLAGS = -Wall -Wextra -Wpedantic -I./src/include $(ARCH_FLAGS) $(OPT_FLAGS)
CXXFLAGS = -std=c++20 $(COMMON_FLAGS)
CFLAGS = $(COMMON_FLAGS)
LDFLAGS = -pthread -lz -flto=auto -flto-partition=one -fno-fat-lto-objects

SRC_DIR = src
IMPL_DIR = $(SRC_DIR)/impl
BUILD_DIR = build
BIN = pgs

# source files
IMPL_SRCS_CPP = $(wildcard $(IMPL_DIR)/*.cpp)
IMPL_SRCS_C = $(wildcard $(IMPL_DIR)/*.c)
MAIN_SRC = $(SRC_DIR)/main.cpp
ALL_SRCS = $(IMPL_SRCS_CPP) $(IMPL_SRCS_C) $(MAIN_SRC)

# object files  
IMPL_OBJS_CPP = $(IMPL_SRCS_CPP:$(IMPL_DIR)/%.cpp=$(BUILD_DIR)/%.o)
IMPL_OBJS_C = $(IMPL_SRCS_C:$(IMPL_DIR)/%.c=$(BUILD_DIR)/%.o)
MAIN_OBJ = $(MAIN_SRC:$(SRC_DIR)/%.cpp=$(BUILD_DIR)/%.o)
ALL_OBJS = $(IMPL_OBJS_CPP) $(IMPL_OBJS_C) $(MAIN_OBJ)

# dependency files
DEPS = $(ALL_OBJS:.o=.d)

all: $(BIN)

$(BIN): $(ALL_OBJS)
	$(CXX) $(ALL_OBJS) -o $@ $(LDFLAGS)

# compile C++ source files
$(BUILD_DIR)/%.o: $(IMPL_DIR)/%.cpp
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

# compile C source files  
$(BUILD_DIR)/%.o: $(IMPL_DIR)/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# compile main.cpp
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)
	rm -f $(BIN)

-include $(DEPS)

.PHONY: all clean