# palettebench — headless SH1 palette trainer bench (desktop and phone; depends on splatcore only)
add_executable(palettebench main.cpp)
target_link_libraries(palettebench PRIVATE splatcore pocketsplat_warnings)
