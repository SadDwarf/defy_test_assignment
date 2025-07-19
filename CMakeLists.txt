cmake_minimum_required(VERSION 3.20)
project(defy_test_assignment)

set(CMAKE_CXX_STANDARD 23)

add_executable(defy_test_assignment main.cpp)

target_compile_options(defy_test_assignment PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Werror>
)