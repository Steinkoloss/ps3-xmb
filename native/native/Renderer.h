#pragma once

#include "GlProgram.h"
#include <QImage>
#include <QSize>
#include <map>
#include <memory>

class Renderer : protected QOpenGLFunctions_4_3_Core {
public:
    Renderer(const QString& shaderDirectory, const QString& graphPath);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    void render(GLuint destination, QSize size);
    QImage capture(QSize size);
private:
    struct Target { GLuint texture=0, framebuffer=0; QSize size; GLenum type=GL_TEXTURE_2D; };
    struct Pass { QString name; QJsonObject specification; std::unique_ptr<GlProgram> program; Target target; };
    Target createTarget(QSize size, GLenum format, GLenum type=GL_TEXTURE_2D);
    GLuint imageTexture(const QString& path, GLenum wrap, GLenum type);
    void destroyTarget(Target& target);
    void bindMesh(const QString& path);
    void drawFullscreen();
    void drawMesh();
    std::vector<Pass> passes_;
    std::map<QString, GLuint> images_;
    std::vector<GLuint> ownedTextures_;
    GLuint vertexArray_=0, vertices_=0, indices_=0, meshVertices_=0, meshIndices_=0;
    GLsizei indexCount_=0;
    QString root_;
    QJsonObject graph_;
};
