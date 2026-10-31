#pragma once

#include <QJsonObject>
#include <QOpenGLFunctions_4_3_Core>
#include <QOpenGLShaderProgram>
#include <QString>
#include <QSize>
#include <array>
#include <map>
#include <vector>

class GlProgram {
public:
    GlProgram(QOpenGLFunctions_4_3_Core& gl, const QString& directory,
              const QJsonObject& manifest, const QString& vertex, const QString& fragment);
    ~GlProgram();
    GlProgram(const GlProgram&) = delete;
    GlProgram& operator=(const GlProgram&) = delete;
    void set(const QString& name, const QJsonArray& values);
    void bind();
    void orientTexture(GLuint unit, bool flip, QSize size);
    const std::map<GLuint, GLenum>& samplers() const { return samplers_; }
private:
    void initializeConstants(const QJsonObject& metadata, bool vertex);
    QOpenGLFunctions_4_3_Core& gl_;
    QOpenGLShaderProgram program_;
    QJsonObject vertexMetadata_, fragmentMetadata_;
    std::array<float, 512 * 4> vertexConstants_{};
    std::vector<float> fragmentConstants_;
    GLuint buffers_[2]{};
    std::map<GLuint, GLenum> samplers_;
};

QByteArray readBytes(const QString& path);
QJsonObject readObject(const QString& path);
