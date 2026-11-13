#include "Renderer.h"
#include "OriginalImage.h"
#include <QFile>
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

Renderer::Renderer(const QString& directory, const QString& graphPath) :
    Renderer(directory, readObject(graphPath), QFileInfo(graphPath).absolutePath()) {}

Renderer::Renderer(const QString& directory, QJsonObject graph, QString resourceRoot) :
    root_(std::move(resourceRoot)), graph_(std::move(graph)) {
    if (!initializeOpenGLFunctions()) error("OpenGL 4.3 is required for the original-program baseline");
    if (graph_.value("schema").toString() != "moyou.render-graph.v1") error("Unsupported render graph");
    const auto manifest = readObject(QDir(directory).filePath("manifest.json"));
    if (manifest.value("schema").toString() != "moyou.native-shaders.v1") error("Unsupported shader manifest");
    const auto programs = manifest.value("programs").toObject();
    glGenVertexArrays(1, &vertexArray_);
    glBindVertexArray(vertexArray_);
    glGenBuffers(1, &vertices_);
    glGenBuffers(1, &indices_);
    glGenBuffers(1, &meshVertices_);
    glGenBuffers(1, &meshIndices_);
    glGenBuffers(1, &particleRecords_);
    glGenBuffers(1, &particleCorners_);
    // Original corner stream (renderer image 0x0209bff0), attribute 8.
    constexpr float corners[] = {0,0, 1,0, 1,1, 0,1};
    glBindBuffer(GL_ARRAY_BUFFER, particleCorners_);
    glBufferData(GL_ARRAY_BUFFER, sizeof corners, corners, GL_STATIC_DRAW);
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
            specification.value("vertex").toString(), specification.value("fragment").toString(),
            specification.value("alphaTest").toString() == "greater0");
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
    if (readFence_) glDeleteSync(readFence_);
    if (readBuffer_) glDeleteBuffers(1, &readBuffer_);
    destroyTarget(readTarget_);
    for (auto& pass : passes_) destroyTarget(pass.target);
    glDeleteTextures(static_cast<GLsizei>(ownedTextures_.size()), ownedTextures_.data());
    GLuint buffers[] = {vertices_, indices_, meshVertices_, meshIndices_, particleRecords_, particleCorners_};
    glDeleteBuffers(6, buffers);
    if (croppedQuad_) glDeleteBuffers(1, &croppedQuad_);
    if (mappedQuad_) glDeleteBuffers(1, &mappedQuad_);
    if (mappedProgram_) glDeleteProgram(mappedProgram_);
    destroyTarget(mappedTarget_);
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
    // TGA rows stay in stored order: the firmware uploads them so and ignores the origin bit
    // (measured on a running XMB: the particle iridescence matches only with stored order).
    const QImage image = decodeOriginalImage(path);
    if (image.isNull()) error("Cannot decode original texture: " + path);
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(type, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(type, 0, GL_RGBA8, image.width(), image.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, image.constBits());
    glTexParameteri(type, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(type, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    const auto addressing = type == GL_TEXTURE_RECTANGLE ? GL_CLAMP_TO_EDGE : wrap;
    glTexParameteri(type, GL_TEXTURE_WRAP_S, static_cast<GLint>(addressing));
    glTexParameteri(type, GL_TEXTURE_WRAP_T, static_cast<GLint>(addressing));
    ownedTextures_.push_back(texture);
    images_.emplace(key, texture);
    imageSizes_.emplace(texture,image.size());
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
    mesh(vertices);
}

void Renderer::mesh(const std::vector<float>& vertices) {
    if (vertices.size() != 128 * 128 * 9) error("Wrong-sized original surface mesh");
    for (float value : vertices) if (!std::isfinite(value)) error("Non-finite original mesh coordinate");
    glBindBuffer(GL_ARRAY_BUFFER, meshVertices_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(float)), vertices.data(), GL_STREAM_DRAW);
    if (indexCount_) return;
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

void Renderer::texture(const QString& name, QSize size, const void* rgba, bool floating, bool rectangle) {
    if (name.isEmpty() || size.isEmpty() || size.width() > 8192 || size.height() > 8192) error("Invalid native texture input");
    auto& texture = suppliedTextures_[name];
    const GLenum type = rectangle ? GL_TEXTURE_RECTANGLE : GL_TEXTURE_2D;
    if (!texture.texture) {
        glGenTextures(1, &texture.texture);
        texture.type = type;
        ownedTextures_.push_back(texture.texture);
    }
    if (texture.type != type) error("Original texture target changed");
    texture.size = size;
    glBindTexture(type, texture.texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(type, 0, floating ? GL_RGBA32F : GL_RGBA8, size.width(), size.height(), 0,
        GL_RGBA, floating ? GL_FLOAT : GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(type, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(type, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(type, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(type, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

void Renderer::fragmentUniform(const QString& name, const QString& parameter, const QJsonArray& values) {
    for (auto& pass : passes_) if (pass.name == name) { pass.program->set(parameter, values, 2); return; }
    error("Missing original pass for uniform update: " + name);
}

void Renderer::uniform(const QString& name, const QString& parameter, const QJsonArray& values) {
    for (auto& pass : passes_) if (pass.name == name) { pass.program->set(parameter, values); return; }
    error("Missing original pass for uniform update: " + name);
}

void Renderer::quadRect(const QString& pass, QRectF rect) {
    if (rect.isEmpty()) quadRects_.erase(pass); else quadRects_[pass] = rect;
}

void Renderer::drawFullscreen(const QRectF* rect) {
    if (rect) {
        // Same quad as vertices_ (texture v runs top to bottom) over the sub-rectangle.
        const float u0 = float(rect->left()), u1 = float(rect->right()), v0 = float(rect->top()), v1 = float(rect->bottom());
        const float quad[] = {-1,-1,0,1, u0,v1, 1,-1,0,1, u1,v1, 1,1,0,1, u1,v0, -1,1,0,1, u0,v0};
        if (!croppedQuad_) glGenBuffers(1, &croppedQuad_);
        glBindBuffer(GL_ARRAY_BUFFER, croppedQuad_);
        glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STREAM_DRAW);
    } else
        glBindBuffer(GL_ARRAY_BUFFER, vertices_);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indices_);
    for (GLuint i = 0; i < 16; ++i) glDisableVertexAttribArray(i);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(8);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(float), nullptr);
    glVertexAttribPointer(8, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), reinterpret_cast<void*>(4 * sizeof(float)));
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
}

void Renderer::particles(const std::vector<float>& records) {
    if (records.size() % 8 || records.size() / 8 > 0x4000) error("Wrong-sized original particle records");
    glBindBuffer(GL_ARRAY_BUFFER, particleRecords_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(records.size() * sizeof(float)), records.data(), GL_STREAM_DRAW);
    particleCount_ = static_cast<GLsizei>(records.size() / 8);
}

void Renderer::drawParticles(unsigned bit) {
    for (GLuint i = 0; i < 16; ++i) glDisableVertexAttribArray(i);
    if (!particleCount_ || !(particleMask_ & bit)) return;
    // RSX frequency dividers: corners per vertex, particle records per quad.
    glBindBuffer(GL_ARRAY_BUFFER, particleCorners_);
    glEnableVertexAttribArray(8);
    glVertexAttribPointer(8, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    glBindBuffer(GL_ARRAY_BUFFER, particleRecords_);
    for (GLuint i : {0u, 11u}) {
        glEnableVertexAttribArray(i);
        glVertexAttribPointer(i, 4, GL_FLOAT, GL_FALSE, 8 * sizeof(float), reinterpret_cast<void*>(i ? 4 * sizeof(float) : 0));
        glVertexAttribDivisor(i, 1);
    }
    glDrawArraysInstanced(GL_TRIANGLE_FAN, 0, 4, particleCount_);
    for (GLuint i : {0u, 11u}) glVertexAttribDivisor(i, 0);
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
    if (mappedSize_.isEmpty()) { renderGraph(destination, size); return; }
    if (mappedTarget_.size != mappedSize_) {
        destroyTarget(mappedTarget_);
        mappedTarget_ = createTarget(mappedSize_, GL_RGBA8);
    }
    renderGraph(mappedTarget_.framebuffer, mappedSize_);
    drawMapped(destination, size);
}

void Renderer::drawMapped(GLuint destination, QSize size) {
    if (!mappedProgram_) {
        const char* vertex = "#version 430 core\n"
            "layout(location=0) in vec2 position; layout(location=1) in vec2 coordinate; out vec2 uv;\n"
            "void main() { uv = coordinate; gl_Position = vec4(position, 0.0, 1.0); }\n";
        const char* fragment = "#version 430 core\n"
            "in vec2 uv; layout(binding=0) uniform sampler2D image; out vec4 colour;\n"
            "void main() { colour = texture(image, uv); }\n";
        mappedProgram_ = glCreateProgram();
        for (const auto& [type, source] : {std::pair{GL_VERTEX_SHADER, vertex}, std::pair{GL_FRAGMENT_SHADER, fragment}}) {
            const GLuint shader = glCreateShader(type);
            glShaderSource(shader, 1, &source, nullptr);
            glCompileShader(shader);
            GLint ok = 0;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
            if (!ok) error("Mapped output shader failed to compile");
            glAttachShader(mappedProgram_, shader);
            glDeleteShader(shader);
        }
        glLinkProgram(mappedProgram_);
        GLint ok = 0;
        glGetProgramiv(mappedProgram_, GL_LINK_STATUS, &ok);
        if (!ok) error("Mapped output shader failed to link");
        glGenBuffers(1, &mappedQuad_);
    }
    // Corners in fan order; the offscreen texture's row 0 is the image bottom.
    const std::array<QPointF, 4> positions{QPointF(-1, 1), QPointF(1, 1), QPointF(1, -1), QPointF(-1, -1)};
    float quad[16];
    for (unsigned i = 0; i < 4; ++i) {
        quad[i * 4 + 0] = float(positions[i].x()); quad[i * 4 + 1] = float(positions[i].y());
        quad[i * 4 + 2] = float(mappedCorners_[i].x()); quad[i * 4 + 3] = float(1 - mappedCorners_[i].y());
    }
    glBindVertexArray(vertexArray_);
    glBindFramebuffer(GL_FRAMEBUFFER, destination);
    glViewport(0, 0, size.width(), size.height());
    glDisable(GL_BLEND);
    glUseProgram(mappedProgram_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, mappedTarget_.texture);
    glBindBuffer(GL_ARRAY_BUFFER, mappedQuad_);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STREAM_DRAW);
    for (GLuint i = 0; i < 16; ++i) glDisableVertexAttribArray(i);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void*>(2 * sizeof(float)));
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glUseProgram(0);
    if (auto status = glGetError(); status != GL_NO_ERROR) error("Mapped output failed, OpenGL error " + QString::number(status, 16));
}

void Renderer::renderGraph(GLuint destination, QSize size) {
    // A host scene graph may share this context; establish the state the passes assume.
    while (glGetError() != GL_NO_ERROR) {}
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glDisable(GL_DITHER);
    glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    glBindVertexArray(vertexArray_);
    std::map<QString, Target*> previous;
    for (size_t index = 0; index < passes_.size(); ++index) {
        auto& pass = passes_[index];
        const auto& spec = pass.specification;
        QSize dimensions(spec.value("width").toInt(size.width()), spec.value("height").toInt(size.height()));
        if (dimensions.width() < 1 || dimensions.height() < 1) error("Invalid original render-target size");
        bool final = index + 1 == passes_.size();
        const bool signedBlend = final && spec.value("blend").toString() == "add";
        if (final && dimensions != size) error("Final original pass size differs from its output surface");
        if ((!final || signedBlend) && pass.target.size != dimensions) {
            destroyTarget(pass.target);
            pass.target = createTarget(dimensions, pixelFormat(spec.value("format").toString(signedBlend ? "rgba16f" : "rgba8")),
                spec.value("rectangle").toBool() ? GL_TEXTURE_RECTANGLE : GL_TEXTURE_2D);
        }
        const auto framebuffer = final && !signedBlend ? destination : pass.target.framebuffer;
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glViewport(0, 0, dimensions.width(), dimensions.height());
        glDisable(GL_BLEND);
        glClearColor(0,0,0,0);
        glClear(GL_COLOR_BUFFER_BIT);
        const auto geometry = spec.value("geometry").toString();
        if ((geometry == "sheet" || geometry == "particles") && geometryViewport_ != QRectF(0,0,1,1)) {
            const double width = dimensions.width() / geometryViewport_.width(), height = dimensions.height() / geometryViewport_.height();
            glViewport(GLint(std::lround(-geometryViewport_.left() * width)),
                GLint(std::lround(-(1 - geometryViewport_.bottom()) * height)),
                GLsizei(std::lround(width)), GLsizei(std::lround(height)));
        }
        if (spec.contains("over")) {
            auto base = previous.find(spec.value("over").toString());
            if (base == previous.end()) error("Original blend pass has no base framebuffer");
            glBindFramebuffer(GL_READ_FRAMEBUFFER,base->second->framebuffer);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER,framebuffer);
            glBlitFramebuffer(0,0,base->second->size.width(),base->second->size.height(),
                0,0,dimensions.width(),dimensions.height(),GL_COLOR_BUFFER_BIT,GL_NEAREST);
            glBindFramebuffer(GL_FRAMEBUFFER,framebuffer);
        }
        if (spec.value("blend").toString() == "add") {
            glEnable(GL_BLEND); glBlendEquation(GL_FUNC_ADD); glBlendFunc(GL_ONE,GL_ONE);
        } else if (spec.contains("blend")) error("Unadapted original blend mode");
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
            bool flip = false;
            QSize sourceSize;
            if (binding.contains("native")) {
                auto dependency = suppliedTextures_.find(binding.value("native").toString());
                if (dependency == suppliedTextures_.end() || dependency->second.type != type)
                    error("Missing or incompatible native texture in " + pass.name);
                texture = dependency->second.texture;
                sourceSize = dependency->second.size;
            } else if (binding.contains("pass")) {
                auto dependency = previous.find(binding.value("pass").toString());
                if (dependency == previous.end()) error("Original pass samples a missing predecessor");
                if (dependency->second->type != type) error("Original pass samples an incompatible texture target");
                texture = dependency->second->texture;
                sourceSize = dependency->second->size;
                flip = true;
            } else {
                texture = imageTexture(QDir(root_).filePath(binding.value("file").toString()),
                    binding.value("repeat").toBool() ? GL_REPEAT : GL_CLAMP_TO_EDGE, type);
                sourceSize = imageSizes_.at(texture);
            }
            pass.program->orientTexture(unit, binding.value("flipY").toBool(flip), sourceSize,binding.value("repeat").toBool());
            glBindTexture(type, texture);
            if (type == GL_TEXTURE_2D) {
                glTexParameterf(type,GL_TEXTURE_MIN_LOD,binding.value("level").toInt());
                glTexParameterf(type,GL_TEXTURE_MAX_LOD,binding.contains("level") ? binding.value("level").toInt() : 1000);
            }
        }
        if (spec.value("geometry").toString() == "sheet") drawMesh();
        else if (spec.value("geometry").toString() == "particles") drawParticles(unsigned(spec.value("particleBit").toInt(1)));
        else {
            const auto crop = quadRects_.find(pass.name);
            drawFullscreen(crop == quadRects_.end() ? nullptr : &crop->second);
        }
        if (spec.value("mipmaps").toBool()) {
            if (final || pass.target.type != GL_TEXTURE_2D) error("Original mipmap source needs an intermediate 2D target");
            glBindTexture(GL_TEXTURE_2D,pass.target.texture);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_NEAREST);
            glGenerateMipmap(GL_TEXTURE_2D);
        }
        if (auto status = glGetError(); status != GL_NO_ERROR)
            error("Original pass " + pass.name + " failed, OpenGL error " + QString::number(status, 16));
        previous[pass.name] = &pass.target;
        if (signedBlend) {
            // Preserve negative dither cancellation until the final display clamp.
            glBindFramebuffer(GL_READ_FRAMEBUFFER,pass.target.framebuffer);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER,destination);
            glBlitFramebuffer(0,0,size.width(),size.height(),0,0,size.width(),size.height(),GL_COLOR_BUFFER_BIT,GL_NEAREST);
        }
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

void Renderer::requestFloat(QSize size) {
    if (readFence_) error("Previous original float capture was not collected");
    if (readTarget_.size != size) { destroyTarget(readTarget_); readTarget_ = createTarget(size, GL_RGBA32F); }
    render(readTarget_.framebuffer, size);
    if (!readBuffer_) glGenBuffers(1, &readBuffer_);
    const auto bytes = static_cast<GLsizeiptr>(size.width()) * size.height() * 4 * sizeof(float);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, readBuffer_);
    glBufferData(GL_PIXEL_PACK_BUFFER, bytes, nullptr, GL_STREAM_READ);
    glBindFramebuffer(GL_FRAMEBUFFER, readTarget_.framebuffer);
    glReadPixels(0, 0, size.width(), size.height(), GL_RGBA, GL_FLOAT, nullptr);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    readFence_ = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    readSize_ = size;
}

std::vector<float> Renderer::collectFloat() {
    if (!readFence_) error("No original float capture in flight");
    glClientWaitSync(readFence_, GL_SYNC_FLUSH_COMMANDS_BIT, GLuint64(5) * 1000000000);
    glDeleteSync(readFence_);
    readFence_ = nullptr;
    std::vector<float> pixels(static_cast<size_t>(readSize_.width()) * readSize_.height() * 4);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, readBuffer_);
    glGetBufferSubData(GL_PIXEL_PACK_BUFFER, 0, static_cast<GLsizeiptr>(pixels.size() * sizeof(float)), pixels.data());
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    return pixels;
}

std::vector<float> Renderer::captureFloat(QSize size) {
    auto target = createTarget(size, GL_RGBA32F);
    render(target.framebuffer, size);
    glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
    std::vector<float> pixels(static_cast<size_t>(size.width()) * size.height() * 4);
    glReadPixels(0, 0, size.width(), size.height(), GL_RGBA, GL_FLOAT, pixels.data());
    destroyTarget(target);
    return pixels;
}
