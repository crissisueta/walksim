#!/bin/sh
# Build walksim with the bundled raylib library.
set -e
g++ -O2 -Wall -std=c++11 -Iraylib/src game.cpp terrain.cpp city.cpp building.cpp player.cpp config.cpp lighting.cpp viewer.cpp -o game \
    -Lraylib/src -lraylib -lGL -lm -lpthread -ldl -lrt -lX11
echo "Built ./game  (run: ./game [seed]  or  ./game --view assets/B1.obj)"
