#include "contracts/ImageFile.hpp"
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QVariant>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
}

int main(int argc, char** argv)
try {
    QCoreApplication app(argc, argv);
    auto file = ImageFilePtr::create();
    const auto path = file->Path();
    {
        QFile output(path);
        Check(output.open(QIODevice::WriteOnly) && output.write("shared resource") == 15,
              "Core-only consumer can populate the shared temporary file");
    }
    auto payload = QVariant::fromValue(file);
    Check(payload.value<ImageFilePtr>() == file, "shared resource survives event payload conversion");
    file.clear();
    Check(QFileInfo::exists(path), "queued payload retains the temporary file");
    payload.clear();
    Check(!QFileInfo::exists(path), "last reference releases the temporary file");
    std::cout << "Core-only shared image lifetime contract passed\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
