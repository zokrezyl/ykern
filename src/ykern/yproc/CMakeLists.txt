# ykern_yproc — procfs (/proc) transport.
add_library(ykern_yproc STATIC
    yproc.c
)

target_include_directories(ykern_yproc PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(ykern_yproc PUBLIC ykern_ycore)
