# Turn a file into a C array: cmake -DIN=file -DOUT=file.c -DSYM=name -P embed.cmake
file(READ "${IN}" hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
get_filename_component(name "${IN}" NAME)
file(WRITE "${OUT}" "/* ${name}, embedded by cmake/embed.cmake */\nconst unsigned char ${SYM}[] = {${bytes}};\nconst unsigned int ${SYM}_len = sizeof(${SYM});\n")
