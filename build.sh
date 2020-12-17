#!/bin/bash

build()
{
	if [ -f ../SugaR_AI_$1 ]; then
		rm ../SugaR_AI_$1 &> /dev/null
	fi
	
	make clean
	make profile-build ARCH=$1 -j 2
	if [ ! -f ./sugar ]; then
		return 1
	fi

	strip ./sugar
	mv ./sugar ../linux_build/SugaR_AI_ICCF_$1

	make clean
}

set -e

if [ -d linux_build ]; then
	rm -r linux_build &> /dev/null
fi

mkdir linux_build
	
cd src
build x86-64-bmi2
build x86-64-avx2
build x86-64-modern
build x86-64-sse41-popcnt
build x86-64-ssse3
cd ..
echo *********************************
echo *ALL BUILDS CREATED SUCCESSFULLY*
echo *********************************
