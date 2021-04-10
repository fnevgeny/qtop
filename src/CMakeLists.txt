set(CMAKE_C_FLAGS "-Wall -Wextra -g -O2")

set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

add_executable(qtop qtop.c)

find_path(PBS_INCLUDE_DIR pbs_ifl.h HINTS "/opt/pbs/include")

find_library(PBS_LIBRARY NAMES pbs PATHS "/opt/pbs/lib")

target_include_directories(qtop PUBLIC ${PBS_INCLUDE_DIR})

target_link_libraries(qtop LINK_PUBLIC ${PBS_LIBRARY} ncurses z dl pthread)

install(TARGETS qtop DESTINATION bin)
