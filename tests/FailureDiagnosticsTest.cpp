#include "FailureDiagnostics.hpp"

int main()
{
    if (TextureFailureMessage("/models/A/face.png", "Cannot read") !=
            "[Texture] Cannot read: /models/A/face.png" ||
        TextureFailureMessage("/models/A/face.png", "Cannot decode (bad PNG)") !=
            "[Texture] Cannot decode (bad PNG): /models/A/face.png" ||
        TextureFailureMessage("/models/A/face.png", "glGenTextures failed") !=
            "[Texture] glGenTextures failed: /models/A/face.png" ||
        ShaderLinkFailureMessage("link mismatch") != "Shader link log: link mismatch" ||
        ShaderLinkFailureMessage("") != "Shader program link failed without a driver log") return 1;
    return 0;
}
