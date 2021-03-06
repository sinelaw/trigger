local {

.=.

.PHONY: default clean

default: $./out/fs_override.so $./out/test_fs_tree $./out/test_build_rules $./out/main
check-syntax: default
clean:
	rm -f out/*

WARNINGS=-Wswitch-enum -Wall -Werror -Wextra
GPP=g++ -no-pie
GCC=gcc -no-pie
CLANGPP=clang++

CXX=${GPP} -g ${WARNINGS} -std=c++11 -pthread  -msse4.2
CC=${GCC} -g ${WARNINGS} -std=gnu11


$./out:
	mkdir -p "$@"

$./out/%.o: $./%.cpp
	${CXX} -c "$<" -o "$@"

$./out/fs_override.so:
	${CC} -o "$@" -Winit-self -shared -fPIC -D_GNU_SOURCE fshook/*.c -ldl

$./out/test_fs_tree: $./test_fs_tree.cpp $./out/fs_tree.o $./out/debug.o
	${CXX} $^ -lbsd -lleveldb -o "$@"

$./out/test_build_rules: $./test_build_rules.cpp $./out/build_rules.o $./out/debug.o
	${CXX} $^  -o "$@"

$./out/main: $./out/main.o $./out/build_rules.o $./out/job.o $./out/debug.o
	${CXX} $^ -lbsd -lleveldb  -o "$@"

local }
