#include "ModelLoader.hpp"
#include "LAppModel.hpp"
#include "LAppTextureManager.hpp"

#include <sys/stat.h>

std::unique_ptr<LAppModel> ModelLoader::Load(const ModelDescriptor& descriptor) const
{
    struct stat info;
    if (descriptor.id.empty() || stat(descriptor.id.c_str(), &info) != 0 ||
        !S_ISREG(info.st_mode) || info.st_size == 0)
        return nullptr;
    std::unique_ptr<LAppModel> model(new LAppModel(_textures, _width, _height, _emotionTimeout));
    return model->LoadAssets(descriptor.directory.c_str(), descriptor.jsonName.c_str())
        ? std::move(model) : nullptr;
}
