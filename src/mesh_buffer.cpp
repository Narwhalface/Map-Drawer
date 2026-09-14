#include "mesh_buffer.h"

void MeshBuffer::Initialize() {
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), reinterpret_cast<void *>(0));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(float),
                          reinterpret_cast<void *>(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
}

void MeshBuffer::Release() {
    if (vao_ != 0) glDeleteVertexArrays(1, &vao_);
    if (vbo_ != 0) glDeleteBuffers(1, &vbo_);
    vao_ = 0;
    vbo_ = 0;
    vertexCount_ = 0;
}

void MeshBuffer::Draw(GLenum primitive) const {
    if (vertexCount_ <= 0) return;
    glBindVertexArray(vao_);
    glDrawArrays(primitive, 0, vertexCount_);
}

