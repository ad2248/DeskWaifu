---
name: local-ci-tests
description: 在本机通过 DeskWaifu 固定 CI 容器运行 CI 自动化测试、构建和门禁；用于本地复现 CI、验证改动或询问如何运行 CI 测试。
---

# 本地执行 DeskWaifu CI

## 范围

- 权威步骤以 `.gitea/workflows/ci.yml` 为准；主要测试工作流是 `test` job。`package` job（发行包构建、四个已安装包测试）是独立工作流，不要把只跑 `test` job 描述成完整 CI。
- 优先使用与 runner 相同的、按 image ID 固定的 `deskwaifu-ci` 镜像。先查 `~/.config/deskwaifu-ci-runner/config.yaml` 中 `deskwaifu-arch` 映射的 `docker://sha256:...`，再用 `podman image exists <ID>` 确认本机有该镜像。不要仅凭 `latest` 或旧 tag 认为它与 CI 一致；缺少镜像时说明情况，除非用户要求，否则不自动重建/拉取镜像。
- 普通自动化测试不访问真实剪贴板、云凭据、音频设备或私有模型。测试输出中的 `UNVERIFIED` 必须保留并解释，不能伪报已覆盖。

## 工作流程

1. 先查看 `git status --short`，保留用户已有更改。确认 `podman`、CI 镜像和 SDK/GLEW 依赖可用；镜像内依赖应由 `scripts/find-deps-dir.sh` 找到。缺项时停下说明，不静默换宿主机构建。
2. 默认运行完整 `test` job，而非只跑受影响测试。只在用户明确要求定向测试或先做诊断时，才使用 `ctest -R` / `run-test-gate.py --test`，并将结果标为子集。
3. 在仓库外建立临时持久 build 目录（例如 `/tmp/opencode/deskwaifu-ci-<任务名>`），避免把构建产物放进源码树。用可写 bind mount 挂载源码，因为若干测试会写入源码树；测试结束核对 `git status --short`，只清理由本次测试产生且确认无用的文件，绝不覆盖原有更改。
4. 用 rootless Podman、`--userns=keep-id --user "$(id -u):$(id -g)"` 执行，避免 `makepkg` 因 root 身份拒绝运行。目录和挂载权限不合时先查明原因，不要直接改仓库文件属主或以 root 重跑掩盖问题。
5. 在容器内按 CI 次序执行。基础门禁示例（`$IMAGE` 必须替换为核实过的 image ID，宿主先 `mkdir -p "$BUILD_ROOT"`）：

   ```bash
   podman run --rm --security-opt label=disable \
     --userns=keep-id --user "$(id -u):$(id -g)" \
     -v "$PWD:/workspace" -v "$BUILD_ROOT:/build" \
     -w /workspace "$IMAGE" bash -lc '
       python3 -B -m unittest discover -s .gitea/scripts -p "test_*.py" -v &&
       cmake -S . -B /build/build-ci -G Ninja \
         -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
         -DDESKWAIFU_BUILD_OCR=OFF -DDESKWAIFU_BUILD_CONTROL=ON \
         -DDESKWAIFU_TEST_GATE=ON &&
       cmake --build /build/build-ci --parallel 24 &&
       python3 scripts/run-test-gate.py /build/build-ci
     '
   ```

6. 若要求完整复刻 `test` job，还需按 workflow 继续执行：
   - 两个 Release 渲染宏变体（`USE_RENDER_TARGET`、`USE_MODEL_RENDER_TARGET`）的 configure、compile-command 宏检查和完整 build；
   - `python3 scripts/test-environment.py python3 scripts/check-test-mutations.py /build/build-ci`；PR 改动范围检查须先确认可解析真实 merge-base，不能缺少基线时假称选择完成；
   - `bash scripts/headless-smoke-test.sh /build/build-ci`；
   - 两条 `qt_headless_smoke.py` 命令（普通和 `--scale 1.5 --installed`），SDK 参数取容器内 `scripts/find-deps-dir.sh` 返回路径下的 `CubismSdkForNative-5-r.5`；
   - 单独配置 Debug ASan+UBSan build，按 workflow 编译指定目标，并用 workflow 相同正则执行 `run-test-gate.py`。

   全量 build/gate 命令以 workflow 实际内容为准，尤其不要漏掉渲染变体、mutation、Wayland/KWin smoke 或 sanitizer 阶段。长任务可用后台 shell 执行，等待完成通知再汇总；不要轮询或只凭部分日志报成功。

## 报告

- 分别报告镜像标识、执行的阶段、构建结果、门禁通过/失败/跳过与 `UNVERIFIED` 项；区分完整工作流和子集。
- 首次失败应保留日志并先判定是代码失败、环境/权限限制还是测试超时。修复运行条件后可按原命令重跑失败项，但不得把定向重跑包装成全套门禁通过。
- 不自动修改 CI 镜像、workflow、容器 runner 配置，不提交、不推送，也不擅自运行依赖云凭据、真实设备或私有模型的测试。
