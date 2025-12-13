CC = gcc
CFLAGS = -Wall -Wextra -O2 -fPIC
SRC = $(wildcard src/*.c)
OBJ = $(SRC:src/%.c=build/%.o)

LIBNAME = jsoc

all: static shared

build/%.o: src/%.c
	mkdir -p build
	$(CC) $(CFLAGS) -Iinclude -c $< -o $@

static: $(OBJ)
	ar rcs build/lib$(LIBNAME).a $(OBJ)

shared: $(OBJ)
	$(CC) -shared -o build/lib$(LIBNAME).so $(OBJ)
	
test: src/jsoc.c
	$(CC) -o build/jsoc src/jsoc.c src/dynarray.c
	./build/jsoc

clean:
	rm -rf build
