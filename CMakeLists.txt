cmake_minimum_required(VERSION 3.16)
project(sdbg LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wno-deprecated-declarations)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(GTKMM REQUIRED IMPORTED_TARGET gtkmm-4.0)
pkg_check_modules(GTKSOURCE REQUIRED IMPORTED_TARGET gtksourceview-5)

function(link_util target)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(${target} PRIVATE util)
  endif()
endfunction()

add_library(dbg_session STATIC
  src/session.cpp
  src/target_pty.cpp
)
target_include_directories(dbg_session PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(dbg_session PUBLIC Threads::Threads)
link_util(dbg_session)

add_executable(sdbg
  src/app.cpp
)
target_link_libraries(sdbg PRIVATE
  dbg_session
  PkgConfig::GTKMM
  PkgConfig::GTKSOURCE
)

add_executable(sdbg-dap
  src/dap_main.cpp
)
target_link_libraries(sdbg-dap PRIVATE dbg_session)

enable_testing()

add_executable(test_backend tests/test_backend.cpp)
target_include_directories(test_backend PRIVATE src tests)

add_executable(test_pty tests/test_pty.cpp)
target_include_directories(test_pty PRIVATE src tests)
target_link_libraries(test_pty PRIVATE dbg_session)

add_executable(test_session tests/test_session.cpp)
target_include_directories(test_session PRIVATE src tests)
target_link_libraries(test_session PRIVATE dbg_session)

add_test(NAME backend COMMAND test_backend)
add_test(NAME pty COMMAND test_pty)
add_test(NAME session COMMAND test_session)

install(TARGETS sdbg sdbg-dap RUNTIME DESTINATION bin)
install(FILES README.md DESTINATION share/doc/sdbg)
