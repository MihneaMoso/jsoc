target := main
cflags := -Wall -Wextra
oflags := -O2 -march=native
cc := gcc

main: main.c
	cc $(cflags) $(oflags) -o main main.c

all: main