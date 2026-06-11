cmake_minimum_required(VERSION 3.24)
project(SkyMPFixes LANGUAGES CXX)

add_library(${PROJECT_NAME} SHARED src/main.cpp)
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_20)
target_compile_definitions(${PROJECT_NAME} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
