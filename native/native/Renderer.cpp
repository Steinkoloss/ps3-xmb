#include "Renderer.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QtEndian>
#include <array>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace {
[[noreturn]] void error(const QString& text) { throw std::runtime_error(text.toStdString()); }
GLenum pixelFormat(const QString& name) {
    if (name == "rgba8") return GL_RGBA8;
    if (name == "rgba16f") return GL_RGBA16F;
    if (name == "rgba32f") return GL_RGBA32F;
    error("Unknown render-target format: " + name);
}
}

Renderer::Renderer(const QString& directory, const QString& graphPath) {
    if (!initializeOpenGLFunctions()) error("OpenGL 4.3 is required for the original-program baseline");
    graph_ = readObject(graphPath);
    if (graph_.value("schema").toString() != "moyou.render-graph.v1") error("Unsupported render graph");
    root_ = QFileInfo(graphPath).absolutePath();
    const auto manifest = readObject(QDir(directory).filePath("manifest.json"));
    if (manifest.value("schema").toString() != "moyou.native-shaders.v1") error("Unsupported shader manifest");
    const auto programs = manifest.value("programs").toObject();
    glGenVertexArrays(1, &vertexArray_);
    glBindVertexArray(vertexArray_);
    glGenBuffers(1, &vertices_);
    glGenBuffers(1, &indices_);
    glGenBuffers(1, &meshVertices_);
    glGenBuffers(1, &meshIndices_);
    constexpr float quad[] = {
        -1,-1,0,1, 0,1, 1,-1,0,1, 1,1, 1,1,0,1, 1,0, -1,1,0,1, 0,0,
    };
    glBindBuffer(GL_ARRAY_BUFFER, vertices_);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    constexpr uint32_t triangles[] = {0,1,2,0,2,3};
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indices_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof triangles, triangles, GL_STATIC_DRAW);
    if (graph_.contains("mesh")) bindMesh(QDir(root_).filePath(graph_.value("mesh").toString()));
    for (const auto& value : graph_.value("passes").toArray()) {
        auto specification = value.toObject();
        auto name = specification.value("name").toString();
        if (name.isEmpty()) error("Render pass has no name");
        for (const auto& previous : passes_) if (previous.name == name) error("Duplicated original pass name: " + name);
        Pass pass;
        pass.name = name;
        pass.specification = specification;
        pass.program = std::make_unique<GlProgram>(static_cast<QOpenGLFunctions_4_3_Core&>(*this), directory, programs,
            specification.value("vertex").toString(), specification.value("fragment").toString());
        const auto uniforms = specification.value("uniforms").toObject();
        for (auto i = uniforms.begin(); i != uniforms.end(); ++i) pass.program->set(i.key(), i.value().toArray());
        passes_.push_back(std::move(pass));
    }
    if (passes_.empty()) error("Render graph has no original programs");
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glDisable(GL_DITHER);
}

Renderer::~Renderer() {
    for (auto& pass : passes_) destroyTarget(pass.target);
    glDeleteTextures(static_cast<GLsizei>(ownedTextures_.size()), ownedTextures_.data());
    GLuint buffers[] = {vertices_, indices_, meshVertices_, meshIndices_};
    glDeleteBuffers(4, buffers);
    glDeleteVertexArrays(1, &vertexArray_);
}

Renderer::Target Renderer::createTarget(QSize size, GLenum format, GLenum type) {
    Target target;
    target.size = size;
    target.type = type;
    glGenTextures(1, &target.texture);
    glBindTexture(type, target.texture);
    glTexImage2D(type, 0, static_cast<GLint>(format), size.width(), size.height(), 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(type, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(type, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(type, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(type, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &target.framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, type, target.texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) error("Original pass framebuffer is incomplete");
    return target;
}

void Renderer::destroyTarget(Target& target) {
    if (target.framebuffer) glDeleteFramebuffers(1, &target.framebuffer);
    if (target.texture) glDeleteTextures(1, &target.texture);
    target = {};
}

GLuint Renderer::imageTexture(const QString& path, GLenum wrap, GLenum type) {
    auto key = path + QString::number(wrap) + ":" + QString::number(type);
    if (auto existing = images_.find(key); existing != images_.end()) return existing->second;
    QImage image(path);
    if (image.isNull()) error("Cannot decode original texture: " + path);
    image = image.convertToFormat(QImage::Format_RGBA8888);
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(type, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(type, 0, GL_RGBA8, image.width(), image.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, image.constBits());
    glTexParameteri(type, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(type, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(type, GL_TEXTURE_WRAP_S, static_cast<GLint>(wrap));
    glTexParameteri(type, GL_TEXTURE_WRAP_T, static_cast<GLint>(wrap));
    ownedTextures_.push_back(texture);
    images_.emplace(key, texture);
    return texture;
}

void Renderer::bindMesh(const QString& path) {
    auto bytes = readBytes(path);
    if (bytes.size() != 128 * 128 * 9 * 4) error("Expected an unmodified 128 x 128 original mesh capture");
    std::vector<float> vertices(static_cast<size_t>(bytes.size()) / 4);
    for (size_t i = 0; i < vertices.size(); ++i) {
        auto word = qFromLittleEndian<uint32_t>(bytes.constData() + i * 4);
        vertices[i] = std::bit_cast<float>(word);
        if (!std::isfinite(vertices[i])) error("Non-finite original mesh coordinate");
    }
    glBindBuffer(GL_ARRAY_BUFFER, meshVertices_);
    glBufferData(GL_ARRAY_BUFFER, bytes.size(), vertices.data(), GL_STATIC_DRAW);
    std::vector<uint32_t> triangles;
    for (uint32_t y = 0; y < 127; ++y)
        for (uint32_t x = 0; x < 127; ++x) {
            auto a = y * 128 + x, b = a + 128;
            triangles.insert(triangles.end(), {a,b,a+1,a+1,b,b+1});
        }
    indexCount_ = static_cast<GLsizei>(triangles.size());
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, meshIndices_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(triangles.size() * sizeof(uint32_t)), triangles.data(), GL_STATIC_DRAW);
}

void Renderer::drawFullscreen() {
    glBindBuffer(GL_ARRAY_BUFFER, vertices_);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indices_);
    for (GLuint i = 0; i < 16; ++i) glDisableVertexAttribArray(i);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(8);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(float), nullptr);
    glVertexAttribPointer(8, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), reinterpret_cast<void*>(4 * sizeof(float)));
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
}

void Renderer::drawMesh() {
    if (!indexCount_) error("Original sheet pass has no mesh source");
    glBindBuffer(GL_ARRAY_BUFFER, meshVertices_);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, meshIndices_);
    for (GLuint i = 0; i < 16; ++i) glDisableVertexAttribArray(i);
    for (GLuint i : {0u, 8u, 9u}) glEnableVertexAttribArray(i);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 9 * sizeof(float), nullptr);
    glVertexAttribPointer(9, 3, GL_FLOAT, GL_FALSE, 9 * sizeof(float), reinterpret_cast<void*>(4 * sizeof(float)));
    glVertexAttribPointer(8, 2, GL_FLOAT, GL_FALSE, 9 * sizeof(float), reinterpret_cast<void*>(7 * sizeof(float)));
    glDrawElements(GL_TRIANGLES, indexCount_, GL_UNSIGNED_INT, nullptr);
}

void Renderer::render(GLuint destination, QSize size) {
    glBindVertexArray(vertexArray_);
    std::map<QString, Target*> previous;
    for (size_t index = 0; index < passes_.size(); ++index) {
        auto& pass = passes_[index];
        const auto& spec = pass.specification;
        QSize dimensions(spec.value("width").toInt(size.width()), spec.value("height").toInt(size.height()));
        if (dimensions.width() < 1 || dimensions.height() < 1) error("Invalid original render-target size");
        bool final = index + 1 == passes_.size();
        if (final && dimensions != size) error("Final original pass size differs from its output surface");
        if (!final && pass.target.size != dimensions) {
            destroyTarget(pass.target);
            pass.target = createTarget(dimensions, pixelFormat(spec.value("format").toString("rgba8")),
                spec.value("rectangle").toBool() ? GL_TEXTURE_RECTANGLE : GL_TEXTURE_2D);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, final ? destination : pass.target.framebuffer);
        glViewport(0, 0, dimensions.width(), dimensions.height());
        glDisable(GL_BLEND);
        glClearColor(0,0,0,0);
        glClear(GL_COLOR_BUFFER_BIT);
        pass.program->bind();
        const auto bindings = spec.value("textures").toObject();
        for (const auto& [unit, type] : pass.program->samplers()) {
            (void)type;
            if (!bindings.contains(QString::number(unit))) error("Original texture binding is missing in " + pass.name);
        }
        for (auto i = bindings.begin(); i != bindings.end(); ++i) {
            bool valid;
            auto unit = i.key().toUInt(&valid);
            if (!valid || !pass.program->samplers().contains(unit)) error("Invalid original texture unit");
            auto type = pass.program->samplers().at(unit);
            auto binding = i.value().toObject();
            glActiveTexture(GL_TEXTURE0 + unit);
            GLuint texture;
            if (binding.contains("pass")) {
                auto dependency = previous.find(binding.value("pass").toString());
                if (dependency == previous.end()) error("Original pass samples a missing predecessor");
                if (dependency->second->type != type) error("Original pass samples an incompatible texture target");
                texture = dependency->second->texture;
            } else {
                texture = imageTexture(QDir(root_).filePath(binding.value("file").toString()),
                    binding.value("repeat").toBool() ? GL_REPEAT : GL_CLAMP_TO_EDGE, type);
            }
            glBindTexture(type, texture);
        }
        if (spec.value("geometry").toString() == "sheet") drawMesh();
        else drawFullscreen();
        if (auto status = glGetError(); status != GL_NO_ERROR)
            error("Original pass " + pass.name + " failed, OpenGL error " + QString::number(status, 16));
        previous[pass.name] = &pass.target;
    }
}

QImage Renderer::capture(QSize size) {
    auto target = createTarget(size, GL_RGBA8);
    render(target.framebuffer, size);
    glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
    QImage image(size, QImage::Format_RGBA8888);
    glReadPixels(0, 0, size.width(), size.height(), GL_RGBA, GL_UNSIGNED_BYTE, image.bits());
    destroyTarget(target);
    return image.flipped(Qt::Vertical);
}
