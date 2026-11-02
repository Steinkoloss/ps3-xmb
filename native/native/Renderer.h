#pragma once

#include "GlProgram.h"
#include <QImage>
#include <QRectF>
#include <QSize>
#include <array>
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
    // Asynchronous float capture: render and start a PBO read; collect waits for it.
    void requestFloat(QSize size);
    std::vector<float> collectFloat();
    void mesh(const std::vector<float>& vertices);
    // Original particle records: 8 floats each (position+size, rotation), drawn as instanced quads.
    void particles(const std::vector<float>& records);
    // Firmware draw mask: a particle pass draws when its "particleBit" is set.
    void particleMask(unsigned mask) { particleMask_ = mask; }
    void texture(const QString& name, QSize size, const void* rgba, bool floating = false, bool rectangle = false);
    void uniform(const QString& pass, const QString& parameter, const QJsonArray& values);
    void fragmentUniform(const QString& pass, const QString& parameter, const QJsonArray& values);
    // Fullscreen pass texture coordinates over a sub-rectangle (top-left origin, 0-1): a
    // screen showing one part of a wider canvas. Empty restores the full quad.
    void quadRect(const QString& pass, QRectF rect);
    // Geometry passes (sheet, particles) rasterize a canvas-sized viewport offset so only this
    // rectangle (top-left origin, 0-1) lands in the target; shaders see unchanged clip space.
    void geometryViewport(QRectF rect) { geometryViewport_ = rect; }
    // Render the graph at source size offscreen, then draw it into the destination so each
    // destination corner (top-left, top-right, bottom-right, bottom-left) shows the given
    // source point (0-1, top-left origin): a rotated slice of a tilted canvas. Empty source
    // size renders directly.
    void mapping(QSize source, const std::array<QPointF, 4>& corners) { mappedSize_ = source; mappedCorners_ = corners; }
private:
    struct Target { GLuint texture=0, framebuffer=0; QSize size; GLenum type=GL_TEXTURE_2D; };
    struct Pass { QString name; QJsonObject specification; std::unique_ptr<GlProgram> program; Target target; };
    Target createTarget(QSize size, GLenum format, GLenum type=GL_TEXTURE_2D);
    GLuint imageTexture(const QString& path, GLenum wrap, GLenum type);
    void destroyTarget(Target& target);
    void bindMesh(const QString& path);
    void drawFullscreen(const QRectF* rect = nullptr);
    void renderGraph(GLuint destination, QSize size);
    void drawMapped(GLuint destination, QSize size);
    void drawMesh();
    void drawParticles(unsigned bit);
    std::vector<Pass> passes_;
    std::map<QString, GLuint> images_;
    std::map<GLuint,QSize> imageSizes_;
    std::map<QString, Target> suppliedTextures_;
    std::vector<GLuint> ownedTextures_;
    GLuint vertexArray_=0, vertices_=0, indices_=0, meshVertices_=0, meshIndices_=0;
    GLsizei indexCount_=0, particleCount_=0;
    GLuint particleRecords_=0, particleCorners_=0, croppedQuad_=0;
    std::map<QString, QRectF> quadRects_;
    QRectF geometryViewport_{0,0,1,1};
    QSize mappedSize_;
    std::array<QPointF, 4> mappedCorners_{};
    Target mappedTarget_;
    GLuint mappedProgram_=0, mappedQuad_=0;
    unsigned particleMask_=~0u;
    Target readTarget_;
    GLuint readBuffer_=0;
    GLsync readFence_=nullptr;
    QSize readSize_;
    QString root_;
    QJsonObject graph_;
};
