/**
 * Copyright(c) Live2D Inc. All rights reserved.
 *
 * Use of this source code is governed by the Live2D Open Software license
 * that can be found at https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */

#include "LAppSpriteShader.hpp"

#include "LAppDefine.hpp"
#include "LAppLive2DManager.hpp"
#include "LAppPal.hpp"
#include "FailureDiagnostics.hpp"
#include <vector>

LAppSpriteShader::LAppSpriteShader()
{
    _programId = CreateShader();
}

LAppSpriteShader::~LAppSpriteShader()
{
    if (_programId) glDeleteProgram(_programId);
}

GLuint LAppSpriteShader::GetShaderId() const
{
    return _programId;
}

GLuint LAppSpriteShader::CreateShader()
{
    // 构建着色器路径。
    Csm::csmString vertShaderFile(LAppDefine::ShaderPath);
    vertShaderFile += LAppDefine::VertShaderName;
    Csm::csmString fragShaderFile(LAppDefine::ShaderPath);
    fragShaderFile += LAppDefine::FragShaderName;

    // 编译着色器。
    GLuint vertexShaderId = CompileShader(vertShaderFile, GL_VERTEX_SHADER);
    GLuint fragmentShaderId = CompileShader(fragShaderFile, GL_FRAGMENT_SHADER);

    if (!vertexShaderId || !fragmentShaderId)
    {
        if (vertexShaderId) glDeleteShader(vertexShaderId);
        if (fragmentShaderId) glDeleteShader(fragmentShaderId);
        return 0;
    }

    // 创建着色器程序对象。
    GLuint programId = glCreateProgram();
    glAttachShader(programId, vertexShaderId);
    glAttachShader(programId, fragmentShaderId);

    // 链接。
    glLinkProgram(programId);
    GLint linked = GL_FALSE;
    glGetProgramiv(programId, GL_LINK_STATUS, &linked);

    if (linked != GL_TRUE)
    {
        GLint logLength = 0;
        glGetProgramiv(programId, GL_INFO_LOG_LENGTH, &logLength);
        if (logLength > 0) {
            std::vector<GLchar> log(static_cast<std::size_t>(logLength));
            glGetProgramInfoLog(programId, logLength, nullptr, log.data());
            CubismLogError("%s", ShaderLinkFailureMessage(log.data()).c_str());
        } else {
            CubismLogError("%s", ShaderLinkFailureMessage("").c_str());
        }
        glDeleteShader(vertexShaderId);
        glDeleteShader(fragmentShaderId);
        glDeleteProgram(programId);
        return 0;
    }

    glUseProgram(programId);

    // 删除不再需要的着色器对象。
    glDeleteShader(vertexShaderId);
    glDeleteShader(fragmentShaderId);

    return programId;
}

bool LAppSpriteShader::CheckShader(GLuint shaderId)
{
    GLint status;
    GLint logLength;
    glGetShaderiv(shaderId, GL_INFO_LOG_LENGTH, &logLength);
    if (logLength > 0)
    {
        GLchar* log = reinterpret_cast<GLchar*>(CSM_MALLOC(logLength));
        glGetShaderInfoLog(shaderId, logLength, &logLength, log);
        CubismLogError("Shader compile log: %s", log);
        CSM_FREE(log);
    }

    glGetShaderiv(shaderId, GL_COMPILE_STATUS, &status);
    if (status == GL_FALSE)
    {
        glDeleteShader(shaderId);
        return false;
    }

    return true;
}

GLuint LAppSpriteShader::CompileShader(Csm::csmString filename, GLenum shaderType)
{
    // 读取文件。
    Csm::csmSizeInt bufferSize = 0;
    const char* shaderString = reinterpret_cast<const char*>(LAppPal::LoadFileAsBytes(filename.GetRawString(), &bufferSize));
    const GLint glSize = (GLint)bufferSize;

    // 编译。
    GLuint shaderId = glCreateShader(shaderType);
    glShaderSource(shaderId, 1, &shaderString, &glSize);
    glCompileShader(shaderId);

    // 释放读取的着色器字符串。
    LAppPal::ReleaseBytes(reinterpret_cast<Csm::csmByte*>(const_cast<char*>(shaderString)));

    if (!CheckShader(shaderId))
    {
        return 0;
    }

    return shaderId;
}
