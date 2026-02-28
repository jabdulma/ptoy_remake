#pragma once

#define VER_MAJOR 0
#define VER_MINOR 9
#define VER_PATCH 0

// Stringify helpers — lets you write VERSION_STRING_W in a string literal
#define VER_STR_(a, b, c)  #a "." #b "." #c
#define VER_STR(a, b, c)   VER_STR_(a, b, c)
#define VERSION_STRING     VER_STR(VER_MAJOR, VER_MINOR, VER_PATCH)
#define VERSION_STRING_W   L"" VERSION_STRING
