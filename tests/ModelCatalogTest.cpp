#include "ModelCatalog.hpp"

#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

int main()
{
    char dir[] = "/tmp/deskwaifu-catalog-XXXXXX";
    if (!mkdtemp(dir)) return 1;
    const std::string root = dir;
    const std::string first = root + "/first";
    const std::string second = root + "/second";
    mkdir(first.c_str(), 0700);
    mkdir(second.c_str(), 0700);
    mkdir((first + "/Twin").c_str(), 0700);
    mkdir((second + "/Twin").c_str(), 0700);
    const std::string firstJson = first + "/Twin/alpha.model3.json";
    const std::string secondJson = second + "/Twin/beta.model3.json";
    const std::string aliasJson = second + "/Twin/alpha.model3.json";
    const std::string ignored = second + "/Twin/README.txt";
    std::ofstream(firstJson) << "{}";
    std::ofstream(secondJson) << "{}";
    symlink(firstJson.c_str(), aliasJson.c_str());
    std::ofstream(ignored) << "skip";

    const auto models = ModelCatalog::Scan({second, root + "/missing", first, second});
    const bool correct = models.size() == 3 && models[0].name == "Twin" &&
        models[1].name == "Twin" && models[2].name == "Twin" &&
        models[0].id == firstJson && models[1].id == aliasJson &&
        models[2].id == secondJson && models[0].id != models[1].id &&
        models[0].directory == first + "/Twin/";

    unlink(firstJson.c_str());
    unlink(secondJson.c_str());
    unlink(aliasJson.c_str());
    unlink(ignored.c_str());
    rmdir((first + "/Twin").c_str());
    rmdir((second + "/Twin").c_str());
    rmdir(first.c_str());
    rmdir(second.c_str());
    rmdir(dir);
    return correct ? 0 : 1;
}
