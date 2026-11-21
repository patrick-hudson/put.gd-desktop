#!/bin/bash
# chain3.sh WAITFILE MATRIXSCRIPT LIST BUILDJOBS [toolbox]
cd ~/work/r1805
until [ -f "$1" ]; do sleep 15; done
cp pending3/Recognize.h pending3/Recognize.cpp src/src/ocr/; cp pending3/ocr_method.cpp src/tests/integration/ocr/
touch src/src/ocr/Recognize.h src/src/ocr/Recognize.cpp src/tests/integration/ocr/ocr_method.cpp
if [ "$5" = toolbox ]; then T="toolbox run -c putgd-build"; else T=""; fi
$T bash build.sh $4
grep -q BUILD_DONE build.log || exit 1
$T bash $2 $3
