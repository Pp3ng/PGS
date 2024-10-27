PGS_TARGET = pgs
SOURCE = pgs.cpp

all: $(PGS_TARGET)

$(PGS_TARGET): $(SOURCE)
	@g++ $(SOURCE) -std=c++20 -O3 -Wall -lz -o $(PGS_TARGET)

clean:
	@rm -f $(PGS_TARGET)

