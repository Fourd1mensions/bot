cmake_minimum_required(VERSION 3.14 FATAL_ERROR)

project(discord-bot
  VERSION 0.0.1
  LANGUAGES CXX
)

if (NOT DEFINED CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if (NOT DEFINED CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

if (NOT DEFINED CMAKE_EXPORT_COMPILE_COMMANDS)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()


include(cmake/PreventInSourceBuilds.cmake)

include(Deps.cmake)
SetupDependencies()

set(MAIN_SOUCES
  src/main.cpp
  src/requests.cpp
  src/bot.cpp
  src/osu.cpp
  src/utils.cpp
  src/message_builder.cpp
)

find_package(fmt REQUIRED)
find_package(spdlog REQUIRED)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(cpr REQUIRED)
find_package(TBB REQUIRED)

include_directories(${CMAKE_SOURCE_DIR})

add_executable(${PROJECT_NAME}
  ${MAIN_SOUCES}
)

target_include_directories(${PROJECT_NAME} PRIVATE
  include/
)

target_link_libraries(${PROJECT_NAME} PRIVATE
  fmt::fmt
  spdlog::spdlog
  Threads::Threads
  OpenSSL::Crypto
  OpenSSL::SSL
  dpp
  cpr::cpr
  tbb
)

target_compile_features(${PROJECT_NAME} INTERFACE cxx_std_${CMAKE_CXX_STANDARD})

