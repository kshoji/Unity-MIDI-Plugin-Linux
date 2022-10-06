#!/bin/bash
set -e
mkdir -p build build/bin build/obj

g++ -g -c -O2 -fPIC -lasound -pthread -o build/obj/plugin.o plugin.cpp
g++ -shared -o build/bin/MIDIPlugin.so build/obj/plugin.o /usr/lib/x86_64-linux-gnu/libasound.so
objcopy --only-keep-debug build/bin/MIDIPlugin.so build/bin/MIDIPlugin.debug
strip --strip-debug build/bin/MIDIPlugin.so
