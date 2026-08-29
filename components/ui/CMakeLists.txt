idf_component_register(
    SRCS "src/canvas.c" "src/font5x8.c" "src/dui.c" "src/button.c"
    INCLUDE_DIRS "include"
    REQUIRES ducky   # menu.h: the settings model the menu screen draws
)
