#pragma once

#include "GlProgram.h"
#include <QImage>
#include <QSize>
#include <map>
#include <memory>

class Renderer : protected QOpenGLFunctions_4_3_Core {
public:
    Renderer(const QString& shaderDirectory, const QString& graphPath);
    Renderer(const QString& shaderDirectory, QJsonObject graph, QString resourceRoot);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    void render(GLuint destination, QSize size);
    QImage capture(QSize size);
    std::vector<float> captureFloat(QSize size);
    void mesh(const std::vector<float>& vertices);
    // Original particle records: 8 floats each (position+size, rotation), drawn as instanced quads.
    void particles(const std::vector<float>& records);
    // Firmware draw mask: a particle pass draws when its "particleBit" is set.
    void particleMask(unsigned mask) { particleMask_ = mask; }
    void texture(const QString& name, QSize size, const void* rgba, bool floating = false, bool rectangle = false);
    void uniform(const QString& pass, const QString& parameter, const QJsonArray& values);
private:
    struct Target { GLuint texture=0, framebuffer=0; QSize size; GLenum type=GL_TEXTURE_2D; };
    struct Pass { QString name; QJsonObject specification; std::unique_ptr<GlProgram> program; Target target; };
    Target createTarget(QSize size, GLenum format, GLenum type=GL_TEXTURE_2D);
    GLuint imageTexture(const QString& path, GLenum wrap, GLenum type);
    void destroyTarget(Target& target);
    void bindMesh(const QString& path);
    void drawFullscreen();
    void drawMesh();
    void drawParticles(unsigned bit);
    std::vector<Pass> passes_;
    std::map<QString, GLuint> images_;
    std::map<GLuint,QSize> imageSizes_;
    std::map<QString, Target> suppliedTextures_;
    std::vector<GLuint> ownedTextures_;
    GLuint vertexArray_=0, vertices_=0, indices_=0, meshVertices_=0, meshIndices_=0;
    GLsizei indexCount_=0, particleCount_=0;
    GLuint particleRecords_=0, particleCorners_=0;
    unsigned particleMask_=~0u;
    QString root_;
    QJsonObject graph_;
};
