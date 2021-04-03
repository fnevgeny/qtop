add_executable(qtop qtop.c)

set(CMAKE_C_FLAGS "-Wall -Wextra -g -O2")

find_path(PBS_INCLUDE_DIR pbs_ifl.h HINTS "/opt/pbs/include")

find_library(PBS_LIBRARY NAMES pbs PATHS "/opt/pbs/lib")

target_include_directories(qtop PUBLIC ${PBS_INCLUDE_DIR})

target_link_libraries(qtop LINK_PUBLIC ${PBS_LIBRARY} ncurses)

install(TARGETS qtop DESTINATION bin)
