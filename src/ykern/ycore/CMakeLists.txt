# ykern_ycore — foundational types: result, error, text, trace, object, path, root
add_library(ykern_ycore STATIC
    result.c
    text.c
    ytrace.c
    object.c
    path.c
    root.c
)

target_include_directories(ykern_ycore PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

# pthread for ytrace mutex; rt is no longer required for clock_gettime on glibc
# but we keep it for portability across older toolchains
find_package(Threads REQUIRED)
target_link_libraries(ykern_ycore PUBLIC Threads::Threads)
