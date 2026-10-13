/* GL_EXT_memory_object / GL_EXT_memory_object_win32, taken from the Khronos
 * glext.h (MIT licence, see glcorearb.h). The Vulkan renderer (pgraph/vk/
 * display.c) shows its display image through a GL texture that aliases the
 * Vulkan memory, as xemu does. Extension entry points are optional in
 * gl_loader.c: without them only the Vulkan renderer is unavailable.
 * Local to xboxrecomp, see src/gpu/PROVENANCE.md. */
#pragma once

#ifndef GL_EXT_memory_object
#define GL_EXT_memory_object 1
#define GL_TEXTURE_TILING_EXT             0x9580
#define GL_DEDICATED_MEMORY_OBJECT_EXT    0x9581
#define GL_NUM_TILING_TYPES_EXT           0x9582
#define GL_TILING_TYPES_EXT               0x9583
#define GL_OPTIMAL_TILING_EXT             0x9584
#define GL_LINEAR_TILING_EXT              0x9585
typedef void (APIENTRYP PFNGLDELETEMEMORYOBJECTSEXTPROC) (GLsizei n, const GLuint *memoryObjects);
typedef GLboolean (APIENTRYP PFNGLISMEMORYOBJECTEXTPROC) (GLuint memoryObject);
typedef void (APIENTRYP PFNGLCREATEMEMORYOBJECTSEXTPROC) (GLsizei n, GLuint *memoryObjects);
typedef void (APIENTRYP PFNGLTEXSTORAGEMEM2DEXTPROC) (GLenum target, GLsizei levels, GLenum internalFormat, GLsizei width, GLsizei height, GLuint memory, GLuint64 offset);
GLAPI void APIENTRY glDeleteMemoryObjectsEXT (GLsizei n, const GLuint *memoryObjects);
GLAPI GLboolean APIENTRY glIsMemoryObjectEXT (GLuint memoryObject);
GLAPI void APIENTRY glCreateMemoryObjectsEXT (GLsizei n, GLuint *memoryObjects);
GLAPI void APIENTRY glTexStorageMem2DEXT (GLenum target, GLsizei levels, GLenum internalFormat, GLsizei width, GLsizei height, GLuint memory, GLuint64 offset);
#endif

#ifndef GL_EXT_memory_object_win32
#define GL_EXT_memory_object_win32 1
#define GL_HANDLE_TYPE_OPAQUE_WIN32_EXT   0x9587
#define GL_HANDLE_TYPE_OPAQUE_WIN32_KMT_EXT 0x9588
typedef void (APIENTRYP PFNGLIMPORTMEMORYWIN32HANDLEEXTPROC) (GLuint memory, GLuint64 size, GLenum handleType, void *handle);
GLAPI void APIENTRY glImportMemoryWin32HandleEXT (GLuint memory, GLuint64 size, GLenum handleType, void *handle);
#endif
