@echo off
rem Builds and runs the host tests with a MinGW g++ on PATH.
cd /d "%~dp0"
g++ -std=c++11 -Wall -Wextra -DOPENOS_HOST_TEST -Ishim -I../../src/Runtime test_main.cpp ../../src/Runtime/OtaManifest.cpp -o host_tests.exe || exit /b 1
host_tests.exe
