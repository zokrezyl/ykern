# Smoke tests for ykern — small CLI walkers that exercise the library end-to-end.
add_executable(walk walk.c)
target_link_libraries(walk PRIVATE ykern_ycore ykern_ynetlink)
