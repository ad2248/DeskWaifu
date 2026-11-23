# Private Rust CI / Arch packaging image with Cubism Native test assets.
FROM archlinux:base-devel AS toolchain
ARG PACMAN_MIRROR=https://mirrors.tuna.tsinghua.edu.cn/archlinux
# The virtual KWin backend runs without elevated scheduling privileges.
RUN printf 'Server = %s/$repo/os/$arch\nServer = https://geo.mirror.pkgbuild.com/$repo/os/$arch\n' "$PACMAN_MIRROR" > /etc/pacman.d/mirrorlist \
    && pacman -Syu --noconfirm rust git python nodejs libarchive libpipewire clang wl-clipboard sccache \
        cmake libglvnd mesa systemd-libs \
        wayland freetype2 fontconfig libxkbcommon libxkbcommon-x11 libxcb libx11 vulkan-icd-loader \
        dbus kwin plasma-workspace plasma-desktop noto-fonts vulkan-swrast \
    && setcap -r /usr/bin/kwin_wayland \
    && pacman -Scc --noconfirm
RUN printf '\nPACKAGER="DeskWaifu reproducible build"\n' >> /etc/makepkg.conf
ENV LANG=C.UTF-8
WORKDIR /workspace

FROM toolchain AS cargo-dependencies
ARG CARGO_HTTPS_PROXY
COPY Cargo.lock /tmp/workspace-Cargo.lock
RUN --mount=type=bind,source=.,target=/source \
    mkdir -p /opt/deskwaifu-cargo && \
    cd /source && cmp Cargo.lock /tmp/workspace-Cargo.lock && \
    HTTPS_PROXY="$CARGO_HTTPS_PROXY" cargo vendor --locked /opt/deskwaifu-cargo/vendor > /opt/deskwaifu-cargo/config.toml

FROM toolchain AS gui-ci
COPY --from=cargo-dependencies /opt/deskwaifu-cargo /opt/deskwaifu-cargo
RUN pacman -S --noconfirm --needed python-gobject ffmpeg noto-fonts-cjk zstd \
    && pacman -Scc --noconfirm

FROM toolchain AS ci
COPY --from=cargo-dependencies /opt/deskwaifu-cargo /opt/deskwaifu-cargo
ENV CUBISM_SDK_DIR=/opt/deskwaifu-deps/CubismSdkForNative-5-r.5 \
    CUBISM_GLEW_DIR=/opt/deskwaifu-deps/glew
COPY CubismSdkForNative-5-r.5/Core/ ${CUBISM_SDK_DIR}/Core/
COPY CubismSdkForNative-5-r.5/Framework/ ${CUBISM_SDK_DIR}/Framework/
COPY CubismSdkForNative-5-r.5/LICENSE.md CubismSdkForNative-5-r.5/NOTICE*.md ${CUBISM_SDK_DIR}/
COPY CubismSdkForNative-5-r.5/Samples/Resources/Haru/ ${CUBISM_SDK_DIR}/Samples/Resources/Haru/
COPY CubismSdkForNative-5-r.5/Samples/Resources/Hiyori/ ${CUBISM_SDK_DIR}/Samples/Resources/Hiyori/
COPY CubismSdkForNative-5-r.5/Samples/Resources/Mao/ ${CUBISM_SDK_DIR}/Samples/Resources/Mao/
COPY thirdParty/glew/include/ ${CUBISM_GLEW_DIR}/include/
COPY thirdParty/glew/src/glew.c ${CUBISM_GLEW_DIR}/src/glew.c
COPY thirdParty/glew/LICENSE.txt ${CUBISM_GLEW_DIR}/LICENSE.txt
