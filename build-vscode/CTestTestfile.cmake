# CMake generated Testfile for 
# Source directory: /Volumes/拯救者PSSD/Fighting
# Build directory: /Volumes/拯救者PSSD/Fighting/build-vscode
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[lab_tests]=] "/Volumes/拯救者PSSD/Fighting/build-vscode/lab_tests")
set_tests_properties([=[lab_tests]=] PROPERTIES  _BACKTRACE_TRIPLES "/Volumes/拯救者PSSD/Fighting/CMakeLists.txt;85;add_test;/Volumes/拯救者PSSD/Fighting/CMakeLists.txt;0;")
add_test([=[lab_stress_smoke]=] "/Volumes/拯救者PSSD/Fighting/build-vscode/lab_stress" "--ticks" "3000" "--history" "512" "--state-delay" "5")
set_tests_properties([=[lab_stress_smoke]=] PROPERTIES  _BACKTRACE_TRIPLES "/Volumes/拯救者PSSD/Fighting/CMakeLists.txt;90;add_test;/Volumes/拯救者PSSD/Fighting/CMakeLists.txt;0;")
