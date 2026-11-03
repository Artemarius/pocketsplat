# splatconv — desktop CLI over splatcore
add_executable(splatconv src/main.cpp)
target_link_libraries(splatconv PRIVATE splatcore pocketsplat_warnings)
