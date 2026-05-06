# ykern_ynetlink — netlink + generic netlink transport
add_library(ykern_ynetlink STATIC
    ynetlink.c
    genl-namespace.c
    genl-family.c
)

target_include_directories(ykern_ynetlink PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(ykern_ynetlink PUBLIC ykern_ycore)
