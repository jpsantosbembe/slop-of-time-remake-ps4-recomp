#!/bin/sh
# Builds Instalar-Ocarina.exe in this folder. Run in the MSYS2 UCRT64 shell, or with C:\msys64\ucrt64\bin on PATH.
set -e
cd "$(dirname "$0")"
clang -O2 -Wall -Werror -mwindows launcher.c -static -o Instalar-Ocarina.exe
sha256sum Instalar-Ocarina.exe
