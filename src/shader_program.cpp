#include "shader_program.h"

#include "logger.h"

namespace {

GLuint CompileShader(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint success = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512]{};
        glGetShaderInfoLog(shader, sizeof(infoLog), nullptr, infoLog);
        LOG_ERROR("Shader compilation failed: %s", infoLog);
    }
    return shader;
}

} // namespace

GLuint CreateShaderProgram(const char *vertexSource, const char *fragmentSource) {
    GLuint vertexShader = CompileShader(GL_VERTEX_SHADER, vertexSource);
    GLuint fragmentShader = CompileShader(GL_FRAGMENT_SHADER, fragmentSource);

    GLuint program = glCreateProgram();
    glAttachShader(program, vertexShader);
    glAttachShader(program, fragmentShader);
    glLinkProgram(program);

    GLint success = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        char infoLog[512]{};
        glGetProgramInfoLog(program, sizeof(infoLog), nullptr, infoLog);
        LOG_ERROR("Shader program linking failed: %s", infoLog);
    }

    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);
    return program;
}

