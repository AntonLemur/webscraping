cmake_minimum_required(VERSION 3.10)
project(Parser CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Перечисляем все файлы исходного кода через пробел
add_executable(parser main.cpp)

target_link_libraries(parser PRIVATE 
    cpr
    curl
    ssl
    crypto
    pthread
    gumbo
    gq
)

