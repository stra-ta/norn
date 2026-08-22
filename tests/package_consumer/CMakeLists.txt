cmake_minimum_required(VERSION 3.25)

project(norn_package_consumer LANGUAGES CXX)

# The driver sets CMAKE_PREFIX_PATH to a private install prefix and passes no
# source include paths, so this find_package must be satisfied entirely by the
# installed Norn package.
find_package(norn CONFIG REQUIRED)

add_executable(core_consumer core_consumer.cpp)
target_link_libraries(core_consumer PRIVATE norn::core)

add_executable(norn_consumer norn_consumer.cpp)
target_link_libraries(norn_consumer PRIVATE norn::norn)
