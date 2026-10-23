# zlib_memory.c always provides zcalloc()/zcfree(), so no upstream source is
# modified and the choice between the two implementations happens in the
# source, at compile time, on CONFIG_ZLIB_PREFER_PSRAM.
idf_component_register(SRCS "zlib_memory.c"
                       PRIV_REQUIRES heap)

# Only the static library is needed; the shared library, tests, contrib apps
# and install/package rules are switched off.
set(ZLIB_BUILD_SHARED OFF)
set(ZLIB_BUILD_STATIC ON)
set(ZLIB_BUILD_TESTING OFF)
set(ZLIB_INSTALL OFF)

# Upstream is happy to be added as a subdirectory and does not require a
# top-level project beyond its own project() call.
add_subdirectory(zlib)

# zlib keeps its stream allocator in zcalloc()/zcfree() and compiles its own
# out of zutil.c when MY_ZCALLOC is defined, so the ones in zlib_memory.c take
# their place. Every allocation zlib makes for a stream then goes through
# zlib_memory.c. Defining this unconditionally keeps zlib's malloc() out of the
# binary, whether PSRAM is in use or not.
target_compile_definitions(zlibstatic PRIVATE MY_ZCALLOC=1)

# zlibstatic already exports the generated zconf.h and zlib.h as PUBLIC.
# PUBLIC here gives those to consumers of the component as well; zlibstatic
# stays a separate archive on the final link line.
target_link_libraries(${COMPONENT_LIB} PUBLIC zlibstatic)

# zcalloc()/zcfree() live in zlib_memory.c (this component archive). The only
# references are inside zlibstatic, so without -u the linker drops that object
# before libz.a is scanned.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,-u,zcalloc")
