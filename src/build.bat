@ECHO OFF

SET MSYS=C:\MinGW\1.0\bin
SET MGW=C:\MinGW\mingw64\bin
SET PATH=%MGW%;%MSYS%;%PATH%

Title "x86-64-bmi2"
mingw32-make clean
mingw32-make.exe profile-build ARCH=x86-64-bmi2 COMP=mingw -j 12
strip sugar.exe
ren sugar.exe "SugaR AI ICCF 1.40 bmi2.exe"

Title "x86-64-avx2"
mingw32-make clean
mingw32-make.exe profile-build ARCH=x86-64-avx2 COMP=mingw -j 12
strip sugar.exe
ren sugar.exe "SugaR AI ICCF 1.40 avx2.exe"

Title "x86-64-modern"
mingw32-make clean
mingw32-make profile-build ARCH=x86-64-modern COMP=mingw -j 12
strip sugar.exe
ren sugar.exe "SugaR AI ICCF 1.40 modern.exe"

Title "x86-64-sse41-popcnt"
mingw32-make clean
mingw32-make profile-build ARCH=x86-64-sse41-popcnt COMP=mingw -j 12
strip sugar.exe
ren sugar.exe "SugaR AI ICCF 1.40 sse41-popcnt.exe"

Title "x86-64-sse3-popcnt"
mingw32-make clean
mingw32-make profile-build ARCH=x86-64-sse3-popcnt COMP=mingw -j 12
strip sugar.exe
ren sugar.exe "SugaR AI ICCF 1.40 sse3-popcnt.exe"

Title "x86-64"
mingw32-make clean
mingw32-make profile-build ARCH=x86-64 COMP=mingw -j 12
strip sugar.exe
ren sugar.exe "SugaR AI ICCF 1.40 64.exe"
mingw32-make clean
PAUSE