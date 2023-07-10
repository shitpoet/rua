#!/bin/bash
clang -O2 -march=native -lm $(pkg-config --cflags --libs sdl2) -lpthread rua.c -o rua
