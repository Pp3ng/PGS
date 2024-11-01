CXX = g++
CXXFLAGS = -std=c++20 -Wall -Wextra -Wpedantic -I./src/include -O3
LDFLAGS = -pthread -lz

SRC_DIR = src
IMPL_DIR = $(SRC_DIR)/impl
BUILD_DIR = build
BIN = pgs

IMPL_SRCS = $(wildcard $(IMPL_DIR)/*.cpp)
MAIN_SRC = $(SRC_DIR)/main.cpp
ALL_SRCS = $(IMPL_SRCS) $(MAIN_SRC)

IMPL_OBJS = $(IMPL_SRCS:$(IMPL_DIR)/%.cpp=$(BUILD_DIR)/%.o)
MAIN_OBJ = $(MAIN_SRC:$(SRC_DIR)/%.cpp=$(BUILD_DIR)/%.o)
ALL_OBJS = $(IMPL_OBJS) $(MAIN_OBJ)

all: $(BIN)

$(BIN): $(ALL_OBJS)
	$(CXX) $(ALL_OBJS) -o $@ $(LDFLAGS)

$(BUILD_DIR)/%.o: $(IMPL_DIR)/%.cpp
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)
	rm -f $(BIN)

.PHONY: all clean