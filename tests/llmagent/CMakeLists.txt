cmake_minimum_required(VERSION 3.16)
project(llmagent_unit_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT DEFINED VENDOR_DIR)
  set(VENDOR_DIR "${CMAKE_SOURCE_DIR}/../../src/Bot/LlmAgent/Vendor")
endif()

if(NOT DEFINED LLMAGENT_DIR)
  set(LLMAGENT_DIR "${CMAKE_SOURCE_DIR}/../../src/Bot/LlmAgent")
endif()

find_package(Threads REQUIRED)

add_executable(llmagent_unit_tests
  doctest_main.cpp
  test_config_load.cpp
  test_goal_parser.cpp
  test_http_client.cpp
  test_digest_shape.cpp
  test_manager_threading.cpp
  test_selector.cpp
  ${LLMAGENT_DIR}/LlmAgentConfig.cpp
  ${LLMAGENT_DIR}/Schemas/Goal.cpp
  ${LLMAGENT_DIR}/Client/LlmHttpClient.cpp
  ${LLMAGENT_DIR}/Tiers/Tier0_StateDigest.cpp
  ${LLMAGENT_DIR}/LlmAgentManager.cpp
  ${LLMAGENT_DIR}/Selector/BotSelector.cpp
)

target_include_directories(llmagent_unit_tests PRIVATE
  ${CMAKE_SOURCE_DIR}
  ${VENDOR_DIR}
  ${LLMAGENT_DIR}
)

target_link_libraries(llmagent_unit_tests PRIVATE
  Threads::Threads
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(llmagent_unit_tests PRIVATE
    -Wall -Wextra -Wpedantic -Wno-unused-parameter
  )
endif()

target_compile_definitions(llmagent_unit_tests PRIVATE LLMAGENT_UNIT_TESTS)
