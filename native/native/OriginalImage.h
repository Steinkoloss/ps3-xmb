// SPDX-License-Identifier: GPL-2.0-or-later
// The firmware's two texture formats, decoded without image plugins: uncompressed TGA
// (type 2, 24/32-bit; rows in stored order, as the firmware uploads them whatever the origin
// bit says) and uncompressed 32-bit BGRA DDS (top mip level).
#pragma once
#include <QFile>
#include <QImage>
#include <QString>
#include <QtEndian>
#include <cstring>

inline QImage decodeOriginalImage(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QByteArray data = file.readAll();
    const auto* bytes = reinterpret_cast<const uint8_t*>(data.constData());
    const auto size = size_t(data.size());
    const auto le16 = [&](size_t at) { return qFromLittleEndian<uint16_t>(bytes + at); };
    const auto le32 = [&](size_t at) { return qFromLittleEndian<uint32_t>(bytes + at); };
    if (path.endsWith(".tga", Qt::CaseInsensitive)) {
        if (size < 18 || bytes[1] != 0 || bytes[2] != 2) return {};
        const int width = le16(12), height = le16(14), depth = bytes[16];
        const size_t start = 18 + bytes[0], pixel = size_t(depth / 8);
        if ((depth != 24 && depth != 32) || !width || !height || start + size_t(width) * height * pixel > size) return {};
        const bool alpha = depth == 32 && (bytes[17] & 15);
        QImage image(width, height, QImage::Format_RGBA8888);
        for (int y = 0; y < height; ++y) {
            const uint8_t* source = bytes + start + size_t(y) * width * pixel;
            uint8_t* target = image.scanLine(y);
            for (int x = 0; x < width; ++x, source += pixel, target += 4) {
                target[0] = source[2]; target[1] = source[1]; target[2] = source[0];
                target[3] = alpha ? source[3] : 255;
            }
        }
        return image;
    }
    if (path.endsWith(".dds", Qt::CaseInsensitive)) {
        if (size < 128 || std::memcmp(bytes, "DDS ", 4) || le32(4) != 124) return {};
        const int height = int(le32(12)), width = int(le32(16));
        if (le32(80) & 4 || le32(88) != 32 || le32(92) != 0xff0000 || le32(96) != 0xff00 || le32(100) != 0xff ||
            !width || !height || 128 + size_t(width) * height * 4 > size) return {};   // uncompressed BGRA only
        const bool alpha = le32(80) & 1;
        QImage image(width, height, QImage::Format_RGBA8888);
        for (int y = 0; y < height; ++y) {
            const uint8_t* source = bytes + 128 + size_t(y) * width * 4;
            uint8_t* target = image.scanLine(y);
            for (int x = 0; x < width; ++x, source += 4, target += 4) {
                target[0] = source[2]; target[1] = source[1]; target[2] = source[0];
                target[3] = alpha ? source[3] : 255;
            }
        }
        return image;
    }
    return QImage(path).convertToFormat(QImage::Format_RGBA8888);
}
