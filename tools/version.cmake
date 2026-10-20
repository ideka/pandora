# Writes a header with the source revision the executable was built from (git describe, "-dirty"
# when the tree has uncommitted changes) and the build time; run on every build, rewritten only
# when it changes so nothing recompiles needlessly.
#   cmake -DSRC=<source dir> -DOUT=<header> -P tools/version.cmake
execute_process(COMMAND git -C "${SRC}" describe --always --dirty --abbrev=8
                OUTPUT_VARIABLE rev OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT rev)
    set(rev "unknown")
endif()
string(TIMESTAMP now "%Y-%m-%d %H:%M")
set(text "#pragma once\n#define PANDORA_VERSION \"${rev}\"\n#define PANDORA_BUILD_TIME \"${now}\"\n")
if(EXISTS "${OUT}")
    file(READ "${OUT}" old)
    # Keep the file (and its build time) while the revision is unchanged.
    string(REGEX MATCH "PANDORA_VERSION \"([^\"]*)\"" _ "${old}")
    if(CMAKE_MATCH_1 STREQUAL rev)
        return()
    endif()
endif()
file(WRITE "${OUT}" "${text}")
