cmake_minimum_required(VERSION 3.7)
project(Filters)

include(filters.cmake)

add_definitions(-fpic)

SET(LIBDJVU_SRC
        ${CMAKE_CURRENT_SOURCE_DIR}/dec_djvu.c
)

SET(LIBDJVU_LIBS
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/libdjvulibre.a
)

SET(LIBDJVU_INC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_filter(libdjvu
        "${LIBDJVU_SRC}"
        "${LIBDJVU_LIBS}"
        []
        ""
        "${LIBDJVU_INC}"
        ""
        "1")
