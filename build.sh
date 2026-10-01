#!/bin/sh
# Build walksim. Requires raylib installed system-wide (see instructions).
set -e
g++ -O2 -Wall -std=c++11 game.cpp terrain.cpp player.cpp config.cpp lighting.cpp viewer.cpp -o game \
    -lraylib -lGL -lm -lpthread -ldl -lrt -lX11
echo "Built ./game  (run: ./game  or  ./game --view assets/house_01.glb)"
