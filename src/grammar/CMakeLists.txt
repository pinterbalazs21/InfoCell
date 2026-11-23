add_library(grammar-lib STATIC
    PatternInduction.cc
    PatternInduction.h
)

target_include_directories(grammar-lib
    PUBLIC
        ${HEADER_ROOT_DIR}
)
