idf_component_register(
    SRCS "src/canvas.c" "src/font5x8.c" "src/dui.c" "src/button.c" "src/qrcodegen.c" "src/font7x12.c"
    INCLUDE_DIRS "include"
    REQUIRES ducky   # menu.h: the settings model the menu screen draws
)
