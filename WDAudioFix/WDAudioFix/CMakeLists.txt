cmake_minimum_required(VERSION 3.20)
project(WDAudioFix C CXX)

# Static CRT so the .asi has no runtime DLL dependencies
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
FetchContent_Declare(minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG master)
FetchContent_MakeAvailable(minhook)

add_library(WDAudioFix SHARED audiofix.cpp)
target_compile_features(WDAudioFix PRIVATE cxx_std_17)
target_link_libraries(WDAudioFix PRIVATE minhook ole32)
set_target_properties(WDAudioFix PROPERTIES PREFIX "" SUFFIX ".asi")
