cmake_minimum_required(VERSION 3.16)
project(ptxlite VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(PTXLITE_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

if(MSVC)
  add_compile_options(/W4)
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Wshadow)
  if(PTXLITE_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
  endif()
endif()

# ---- core library: everything except the command-line front ends ----------
add_library(ptxlite_core
  src/IR.cpp
  src/Lexer.cpp
  src/Parser.cpp
  src/Verifier.cpp
  src/CFG.cpp
  src/ConstantFolding.cpp
  src/DeadCodeElim.cpp
  src/RegAlloc.cpp
  src/ObjectFile.cpp
  src/Compiler.cpp
)
target_include_directories(ptxlite_core PUBLIC src)

# ---- tools -----------------------------------------------------------------
add_executable(ptxlite tools/ptxlite.cpp)
target_link_libraries(ptxlite PRIVATE ptxlite_core)

add_executable(ptxlite-objdump tools/ptxlite-objdump.cpp)
target_link_libraries(ptxlite-objdump PRIVATE ptxlite_core)

# ---- tests -----------------------------------------------------------------
enable_testing()


# Every example must compile and disassemble cleanly...
foreach(example vec_add saxpy row_sum const_fold)
  add_test(NAME compile_${example}
    COMMAND ptxlite ${CMAKE_CURRENT_SOURCE_DIR}/examples/${example}.ptx
            -o ${CMAKE_CURRENT_BINARY_DIR}/${example}.pobj -v)
  add_test(NAME objdump_${example}
    COMMAND ptxlite-objdump ${CMAKE_CURRENT_BINARY_DIR}/${example}.pobj)
  set_tests_properties(objdump_${example} PROPERTIES DEPENDS compile_${example})
endforeach()

# ...and the intentionally broken file must be rejected.
add_test(NAME reject_errors
  COMMAND ptxlite ${CMAKE_CURRENT_SOURCE_DIR}/examples/errors.ptx -o ${CMAKE_CURRENT_BINARY_DIR}/errors.pobj)
set_tests_properties(reject_errors PROPERTIES WILL_FAIL TRUE)
