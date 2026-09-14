#include "gl_lite.h"

PFNGLGENVERTEXARRAYSPROC glGenVertexArrays = nullptr;
PFNGLBINDVERTEXARRAYPROC glBindVertexArray = nullptr;
PFNGLDELETEVERTEXARRAYSPROC glDeleteVertexArrays = nullptr;
PFNGLGENBUFFERSPROC glGenBuffers = nullptr;
PFNGLBINDBUFFERPROC glBindBuffer = nullptr;
PFNGLBUFFERDATAPROC glBufferData = nullptr;
PFNGLDELETEBUFFERSPROC glDeleteBuffers = nullptr;
PFNGLVERTEXATTRIBPOINTERPROC glVertexAttribPointer = nullptr;
PFNGLENABLEVERTEXATTRIBARRAYPROC glEnableVertexAttribArray = nullptr;
PFNGLDISABLEVERTEXATTRIBARRAYPROC glDisableVertexAttribArray = nullptr;
PFNGLCREATESHADERPROC glCreateShader = nullptr;
PFNGLSHADERSOURCEPROC glShaderSource = nullptr;
PFNGLCOMPILESHADERPROC glCompileShader = nullptr;
PFNGLGETSHADERIVPROC glGetShaderiv = nullptr;
PFNGLGETSHADERINFOLOGPROC glGetShaderInfoLog = nullptr;
PFNGLDELETESHADERPROC glDeleteShader = nullptr;
PFNGLCREATEPROGRAMPROC glCreateProgram = nullptr;
PFNGLATTACHSHADERPROC glAttachShader = nullptr;
PFNGLLINKPROGRAMPROC glLinkProgram = nullptr;
PFNGLGETPROGRAMIVPROC glGetProgramiv = nullptr;
PFNGLGETPROGRAMINFOLOGPROC glGetProgramInfoLog = nullptr;
PFNGLUSEPROGRAMPROC glUseProgram = nullptr;
PFNGLDELETEPROGRAMPROC glDeleteProgram = nullptr;
PFNGLGETUNIFORMLOCATIONPROC glGetUniformLocation = nullptr;
PFNGLUNIFORM2FPROC glUniform2f = nullptr;

namespace {
template <typename T>
bool Load(void *(*loader)(const char *), const char *name, T &out) {
    out = reinterpret_cast<T>(loader(name));
    return out != nullptr;
}
} // namespace

bool LoadGLFunctions(void *(*loader)(const char *name)) {
    bool ok = true;
    ok &= Load(loader, "glGenVertexArrays", glGenVertexArrays);
    ok &= Load(loader, "glBindVertexArray", glBindVertexArray);
    ok &= Load(loader, "glDeleteVertexArrays", glDeleteVertexArrays);
    ok &= Load(loader, "glGenBuffers", glGenBuffers);
    ok &= Load(loader, "glBindBuffer", glBindBuffer);
    ok &= Load(loader, "glBufferData", glBufferData);
    ok &= Load(loader, "glDeleteBuffers", glDeleteBuffers);
    ok &= Load(loader, "glVertexAttribPointer", glVertexAttribPointer);
    ok &= Load(loader, "glEnableVertexAttribArray", glEnableVertexAttribArray);
    ok &= Load(loader, "glDisableVertexAttribArray", glDisableVertexAttribArray);
    ok &= Load(loader, "glCreateShader", glCreateShader);
    ok &= Load(loader, "glShaderSource", glShaderSource);
    ok &= Load(loader, "glCompileShader", glCompileShader);
    ok &= Load(loader, "glGetShaderiv", glGetShaderiv);
    ok &= Load(loader, "glGetShaderInfoLog", glGetShaderInfoLog);
    ok &= Load(loader, "glDeleteShader", glDeleteShader);
    ok &= Load(loader, "glCreateProgram", glCreateProgram);
    ok &= Load(loader, "glAttachShader", glAttachShader);
    ok &= Load(loader, "glLinkProgram", glLinkProgram);
    ok &= Load(loader, "glGetProgramiv", glGetProgramiv);
    ok &= Load(loader, "glGetProgramInfoLog", glGetProgramInfoLog);
    ok &= Load(loader, "glUseProgram", glUseProgram);
    ok &= Load(loader, "glDeleteProgram", glDeleteProgram);
    ok &= Load(loader, "glGetUniformLocation", glGetUniformLocation);
    ok &= Load(loader, "glUniform2f", glUniform2f);
    return ok;
}
