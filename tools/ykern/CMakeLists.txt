# ykern CLI — interactive browser for the kernel-object tree.
add_executable(ykern main.c)
target_link_libraries(ykern PRIVATE ykern_ycore ykern_ynetlink)

# Place the binary at the build-dir root so it's runnable as
# `./<build-dir>/ykern` rather than `./<build-dir>/tools/ykern/ykern`.
set_target_properties(ykern PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
