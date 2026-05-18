# Fixed seed header regeneration.
#
# The header is committed so consumers do not need Python at build time, but
# when seeds_main.txt is edited locally this target brings it back in sync.

find_package(Python3 QUIET COMPONENTS Interpreter)

if(Python3_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/contrib/seeds/generate-seeds.py")
  add_custom_command(
    OUTPUT "${CMAKE_SOURCE_DIR}/include/consensus/chainparamsseeds.h"
    COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_SOURCE_DIR}/contrib/seeds/generate-seeds.py"
    DEPENDS
      "${CMAKE_SOURCE_DIR}/contrib/seeds/seeds_main.txt"
      "${CMAKE_SOURCE_DIR}/contrib/seeds/generate-seeds.py"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Regenerating include/consensus/chainparamsseeds.h from contrib/seeds/seeds_main.txt"
    VERBATIM
  )
  add_custom_target(dinero_fixed_seeds_gen ALL
    DEPENDS "${CMAKE_SOURCE_DIR}/include/consensus/chainparamsseeds.h"
  )
else()
  message(STATUS "Python3 not found; chainparamsseeds.h regeneration is manual "
                 "(run contrib/seeds/generate-seeds.py after editing seeds_main.txt)")
endif()
