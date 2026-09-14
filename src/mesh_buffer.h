#pragma once

#include "gl_lite.h"

class MeshBuffer {
public:
    void Initialize();
    void Release();
    void Draw(GLenum primitive) const;

    GLuint Buffer() const { return vbo_; }
    GLsizei &VertexCount() { return vertexCount_; }

private:
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    GLsizei vertexCount_ = 0;
};

