#!/bin/bash
set -e

usage() {
	echo "$(basename $0) <bc file> <function name> [<buffer size>]"
	exit 1
}

[ $# -lt 2 ] && usage

FILE=$1
FUNCTION=$2
SIZE=$3

if [ "$SIZE" == "" ]; then
  llvm-klee $FILE $FUNCTION > main.c
else
  llvm-klee $FILE $FUNCTION -s $SIZE > main.c
fi

clang -c -emit-llvm main.c -o main.bc -D__KLEE__
llvm-link main.bc $FILE -o single.bc

klee --libc=uclibc --posix-runtime single.bc
