# 下载构建依赖

在仓库根目录执行（需安装 `curl`、`unzip`、`tar`，且目标目录尚不存在）：

```bash
curl -fL https://cdn.kals.top/CubismFull.zip -o CubismFull.zip
unzip -q CubismFull.zip
mv Cubism CubismSdkForNative-5-r.5

curl -fL https://cdn.kals.top/glew-2.3.1.tgz -o glew-2.3.1.tgz
tar -xzf glew-2.3.1.tgz -C thirdParty
mv thirdParty/glew-2.3.1 thirdParty/glew
```

最终目录应为 `CubismSdkForNative-5-r.5/` 和 `thirdParty/glew/`，随后即可按 README 构建。使用 Cubism SDK 须遵守其附带的许可条款。
