file(READ "${TEXTURE_MANAGER_HEADER}" texture_manager_header)
file(READ "${TEXTURE_MANAGER_SOURCE}" texture_manager_source)

if (texture_manager_header MATCHES "CreateTextureFromPngFile")
  message(FATAL_ERROR "Legacy texture creation must not be exposed alongside leases")
endif ()

if (NOT texture_manager_source MATCHES
    "_residency\\(\\[\\]\\(GLuint id\\) \\{ glDeleteTextures\\(1, &id\\); \\}\\)")
  message(FATAL_ERROR "The final texture lease must delete its OpenGL texture directly")
endif ()
