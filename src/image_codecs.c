#include "metadata.h"

// No STBI_WINDOWS_UTF8 here: art files are opened through path_fopen_read
// (wide-path attempt with a narrow fallback) and decoded from FILE*/memory,
// so stb_image never opens paths itself.
#define STBI_MAX_DIMENSIONS ART_MAX_DIMENSION
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
