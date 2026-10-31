#include "GlProgram.h"
#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QVector4D>
#include <bit>
#include <stdexcept>

namespace {
[[noreturn]] void error(const QString& text) { throw std::runtime_error(text.toStdString()); }
float bits(const QJsonValue& value) { return std::bit_cast<float>(static_cast<uint32_t>(value.toDouble())); }
}

QByteArray readBytes(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) error("Cannot read " + path + ": " + file.errorString());
    return file.readAll();
}

QJsonObject readObject(const QString& path) {
    QJsonParseError parse;
    auto document = QJsonDocument::fromJson(readBytes(path), &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) error("Invalid JSON: " + path);
    return document.object();
}

GlProgram::GlProgram(QOpenGLFunctions_4_3_Core& gl, const QString& directory,
                     const QJsonObject& manifest, const QString& vertex, const QString& fragment) : gl_(gl) {
    vertexMetadata_ = manifest.value(vertex).toObject();
    fragmentMetadata_ = manifest.value(fragment).toObject();
    if (vertexMetadata_.isEmpty() || fragmentMetadata_.isEmpty()) error("Original shader is absent from the manifest");
    for (auto stage : {std::pair{QOpenGLShader::Vertex, vertexMetadata_},
                       std::pair{QOpenGLShader::Fragment, fragmentMetadata_}}) {
        auto source = QDir(directory).filePath(stage.second.value("glsl").toString());
        auto bytes = readBytes(source);
        auto digest = stage.second.value("glsl_sha256").toString().toLatin1();
        if (digest.size() != 64 || QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() != digest)
            error("Generated original shader was modified: " + source);
        if (!program_.addShaderFromSourceCode(stage.first, bytes)) error(source + ": " + program_.log());
    }
    if (!program_.link()) error("Original program link failed: " + program_.log());
    const auto slotCount = fragmentMetadata_.value("domain").toObject().value("instruction_slots").toInt();
    if (slotCount <= 0 || slotCount > 4096) error("Invalid original fragment constant bank");
    fragmentConstants_.resize(static_cast<size_t>(slotCount) * 4);
    const auto samplers = fragmentMetadata_.value("samplers").toObject();
    for (auto i = samplers.begin(); i != samplers.end(); ++i) {
        bool valid;
        auto unit = i.key().toUInt(&valid);
        auto type = i.value().toInt();
        if (!valid || unit >= 16 || (type != 1066 && type != 1068)) error("Original sampler needs a texture adapter");
        samplers_.emplace(unit, type == 1068 ? GL_TEXTURE_RECTANGLE : GL_TEXTURE_2D);
    }
    initializeConstants(vertexMetadata_, true);
    initializeConstants(fragmentMetadata_, false);
    gl_.glGenBuffers(2, buffers_);
}

GlProgram::~GlProgram() { gl_.glDeleteBuffers(2, buffers_); }

void GlProgram::orientTexture(GLuint unit, bool flip, QSize size) {
    const auto name = QString("textureTransform[%1]").arg(unit).toLatin1();
    const auto location = program_.uniformLocation(name.constData());
    if (location < 0) error("Original shader needs the framebuffer orientation adapter");
    const auto offset = samplers_.at(unit) == GL_TEXTURE_RECTANGLE ? float(size.height()) : 1.0f;
    program_.setUniformValue(location, QVector4D(1, flip ? -1 : 1, 0, flip ? offset : 0));
}

void GlProgram::initializeConstants(const QJsonObject& metadata, bool vertex) {
    if (vertex) {
        for (const auto& value : metadata.value("parameters").toArray()) {
            auto p = value.toObject();
            auto initial = p.value("default_bits").toArray();
            auto index = p.value("resource_index").toInt(-1);
            if (initial.size() == 4 && index >= 0 && index < 512)
                for (int i = 0; i < 4; ++i) vertexConstants_[index * 4 + i] = bits(initial[i]);
        }
    } else {
        const auto constants = metadata.value("constants").toObject();
        for (auto i = constants.begin(); i != constants.end(); ++i) {
            const auto offset = i.key().toUInt() * 4;
            const auto initial = i.value().toArray();
            if (offset + 4 > fragmentConstants_.size() || initial.size() != 4) error("Invalid inline constant slot");
            for (int lane = 0; lane < 4; ++lane) fragmentConstants_[offset + lane] = bits(initial[lane]);
        }
    }
}

void GlProgram::set(const QString& name, const QJsonArray& values) {
    if (values.isEmpty() || values.size() > 16) error("Invalid original parameter value: " + name);
    bool found = false;
    for (int stage = 0; stage < 2; ++stage) {
        const auto& metadata = stage == 0 ? vertexMetadata_ : fragmentMetadata_;
        for (const auto& value : metadata.value("parameters").toArray()) {
            auto parameter = value.toObject();
            if (parameter.value("name").toString() != name) continue;
            found = true;
            if (stage == 0) {
                auto index = parameter.value("resource_index").toInt(-1);
                if (index < 0) continue;
                if (index * 4 + values.size() > static_cast<int>(vertexConstants_.size())) error("Vertex parameter exceeds bank");
                for (int i = 0; i < values.size(); ++i) vertexConstants_[index * 4 + i] = static_cast<float>(values[i].toDouble());
            } else {
                for (const auto& patch : parameter.value("constant_offsets").toArray()) {
                    auto offset = static_cast<size_t>(patch.toInt() / 4);
                    if (values.size() > 4 || offset + values.size() > fragmentConstants_.size()) error("Fragment parameter exceeds bank");
                    for (int i = 0; i < values.size(); ++i) fragmentConstants_[offset + i] = static_cast<float>(values[i].toDouble());
                }
            }
        }
    }
    if (!found) error("Unknown original parameter: " + name);
}

void GlProgram::bind() {
    if (!program_.bind()) error("Cannot bind original shader program");
    gl_.glBindBuffer(GL_UNIFORM_BUFFER, buffers_[0]);
    gl_.glBufferData(GL_UNIFORM_BUFFER, sizeof vertexConstants_, vertexConstants_.data(), GL_DYNAMIC_DRAW);
    gl_.glBindBufferBase(GL_UNIFORM_BUFFER, 0, buffers_[0]);
    gl_.glBindBuffer(GL_UNIFORM_BUFFER, buffers_[1]);
    gl_.glBufferData(GL_UNIFORM_BUFFER, fragmentConstants_.size() * sizeof(float), fragmentConstants_.data(), GL_DYNAMIC_DRAW);
    gl_.glBindBufferBase(GL_UNIFORM_BUFFER, 1, buffers_[1]);
}
