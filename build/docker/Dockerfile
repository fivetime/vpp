# syntax=docker/dockerfile:1.7
###############################################################################
# Stage 1: builder —— 在 Debian 上从本仓库源码编译并打出 .deb 包
#
#   与 BIRD 不同，VPP 依赖 glibc / DPDK / hugepages，无法在 Alpine(musl) 上构建，
#   因此基础镜像必须是 glibc 发行版。这里用 debian:trixie-slim（Debian 13，
#   Makefile 已显式支持 debian-13）。
#
#   注意：.deb 是针对构建环境的库版本链接的，所以 builder 与 runtime 两个 stage
#   必须用同一个发行版/版本，切勿混搭（如 builder=debian、runtime=ubuntu）。
#
#   构建流程沿用 VPP 官方方式：
#     make install-dep      安装编译依赖
#     make install-ext-deps 编译外部依赖（DPDK 等，vpp-plugin-dpdk 需要）
#     make pkg-deb          产出 .deb 到 /vpp/build-root/*.deb
#
#   源码直接来自构建上下文（COPY .），所以请在 vpp 仓库根目录构建：
#     docker build -f build/docker/Dockerfile -t vpp:dev .
#   建议在仓库根放一个 .dockerignore，至少忽略 build-root/ 以免上下文过大。
###############################################################################
FROM debian:trixie-slim AS builder

ARG DEBIAN_FRONTEND=noninteractive
# release(默认优化) 或 debug
ARG BUILD_TYPE="release"
# make 并行度
ARG MAKE_PARALLEL_JOBS="16"
# 仅用于写入 OCI label
ARG VERSION="dev"

# install-dep / install-ext-deps 内部会用到这些基础工具
RUN set -eux \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
        make \
        git \
        sudo \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY . /vpp
WORKDIR /vpp

# 规范化 Windows checkout 带来的两类问题，用 git 从仓库 blob 重新落盘工作树：
#   1) 行尾：Windows 上 core.autocrlf=true → 工作树是 CRLF，会破坏靠 shebang 执行的
#      脚本（如 build/external/deb/debian/rules 的 `#!/usr/bin/make -f` 带 \r 会让 make
#      行为错乱）。强制 autocrlf=false / eol=lf 还原成 LF。
#   2) 符号链接：Windows 上 core.symlinks=false → VPP 里 86 个 symlink（如
#      src/tools/vppapigen/vppapigen -> vppapigen.py）变成内容是目标路径的文本文件，
#      执行时报 "Permission denied"。强制 symlinks=true 还原成真正的符号链接。
# 这些 -c 覆盖 COPY 进来的 .git/config 里的 Windows 设置。CI（Linux）checkout 本就
# 正确，这步等效空操作；只有 Windows 本地构建才需要。
RUN git -c core.autocrlf=false -c core.eol=lf -c core.symlinks=true checkout-index -f -a

# 为版本计算打 tag：VPP 多个组件各自用 `git describe` 推导版本号（vpp 包走
# src/scripts/version，vpp-ext-deps 包走 build/build_common.mk 的 PKG_VERSION）。
# CI 里 checkout 的是 release tag，describe 正常；本地从无 tag 的工作树构建时这些
# describe 返回空，打包会报 "version number does not start with digit"。
# 因此：当 HEAD 不可达任何 v* tag 时，用 VERSION 打一个 tag 喂给所有版本计算。
# 必须用带注解的 tag（-a）：`git describe` 默认忽略轻量 tag。注解 tag 需 git 身份，
# 用 -c 临时指定，避免改全局配置。
RUN set -eux \
    && if ! git describe --abbrev=0 --match 'v[0-9]*' >/dev/null 2>&1 ; then \
           v="${VERSION}" ; case "$v" in dev|"") v="0.0" ;; esac ; \
           echo "tagging HEAD as v${v} (annotated; no reachable git tag for version derivation)" ; \
           git -c user.email=build@vpp.local -c user.name=vpp-build tag -a "v${v}" -m "v${v}" ; \
       fi

# 以下各步拆成独立 RUN，便于 BuildKit 分层缓存：改动 pkg-deb 不会让 install-dep /
# install-ext-deps（最耗时的 DPDK 编译）失效。
RUN make UNATTENDED=yes install-dep
RUN make UNATTENDED=yes install-ext-deps
RUN set -eux \
    && if [ "$BUILD_TYPE" = "debug" ] ; then \
           make UNATTENDED=yes MAKE_PARALLEL_JOBS="${MAKE_PARALLEL_JOBS}" pkg-deb-debug ; \
       else \
           make UNATTENDED=yes MAKE_PARALLEL_JOBS="${MAKE_PARALLEL_JOBS}" pkg-deb ; \
       fi \
    # 把产出的 .deb 收拢到一个干净目录，方便 COPY 到运行镜像
    && mkdir -p /pkg \
    && find build-root -maxdepth 1 -name '*.deb' -exec cp {} /pkg/ \; \
    && ls -l /pkg

###############################################################################
# （可选）packages —— 仅含 .deb 的导出阶段。BuildKit 在普通构建时会跳过它，
# 仅在显式 --target packages 时构建，用于把 .deb 导出到本地：
#   docker build -f build/docker/Dockerfile --target packages \
#     --output type=local,dest=./debs --build-arg VERSION=26.02 .
###############################################################################
FROM scratch AS packages
COPY --from=builder /pkg /

###############################################################################
# Stage 2: runtime —— 仅安装运行所需的 VPP 包
###############################################################################
FROM debian:trixie-slim AS runtime

ARG DEBIAN_FRONTEND=noninteractive
ARG VERSION="dev"

LABEL org.opencontainers.image.title="vpp" \
      org.opencontainers.image.description="FD.io VPP (Vector Packet Processing), built from source on Ubuntu" \
      org.opencontainers.image.source="https://github.com/FDio/vpp" \
      org.opencontainers.image.version="${VERSION}" \
      org.opencontainers.image.licenses="Apache-2.0"

# 安装运行时所需的包。apt 会自动解析 .deb 之间的依赖关系，
# 这里把 build-root 产出的包全装上（vpp / libvppinfra / vpp-plugin-* /
# vpp-drivers / vpp-crypto-engines 等），开发/调试包(vpp-dev/vpp-dbg)
# 会因为缺少 -dev 依赖而被忽略——它们对运行镜像本就无用。
# 用 bind mount 把 builder 的 /pkg 临时挂进来安装，.deb 不进任何镜像层（相比
# COPY 省下约 106MB——COPY 的字节即使随后 rm 也会留在历史层里）。
# VPP_INSTALL_SKIP_SYSCTL=1：vpp 包的 postinst 默认会跑 `sysctl --system`，这在容器
# 构建期既无意义又会因 slim 镜像没有 sysctl(procps) 而失败（exit 127）。postinst 专门
# 留了这个开关用于容器安装，设上即可跳过。
RUN --mount=type=bind,from=builder,source=/pkg,target=/tmp/pkg \
    set -eux \
    && apt-get update \
    && VPP_INSTALL_SKIP_SYSCTL=1 apt-get install -y --no-install-recommends \
        /tmp/pkg/libvppinfra_*.deb \
        /tmp/pkg/vpp_*.deb \
        /tmp/pkg/vpp-plugin-core_*.deb \
        /tmp/pkg/vpp-plugin-dpdk_*.deb \
        /tmp/pkg/vpp-drivers_*.deb \
        /tmp/pkg/vpp-crypto-engines_*.deb \
    && rm -rf /var/lib/apt/lists/* \
    # vpp 运行需要这些目录（startup.conf 里引用了 /var/log/vpp 与 /run/vpp）
    && mkdir -p /var/log/vpp /run/vpp

# 校验安装是否成功（构建期自检，缺库会直接让 build 失败）
RUN set -eux \
    && vpp --version

# VPP 二进制 API / CLI 监听端口非固定，常见对外端口：
#   179/tcp  -> 如果跑 BGP/控制面（一般由其它组件占用）
# VPP 本身主要靠共享内存/unix socket(/run/vpp/cli.sock) 控制，按需 EXPOSE。

# 健康检查：通过 vppctl 询问 vpp 是否在跑
HEALTHCHECK --interval=30s --timeout=5s --start-period=15s --retries=3 \
    CMD vppctl -s /run/vpp/cli.sock show version >/dev/null 2>&1 || exit 1

# 前台运行(startup.conf 里已配置 nodaemon)，挂载/覆盖 /etc/vpp/startup.conf 自定义
CMD ["/usr/bin/vpp", "-c", "/etc/vpp/startup.conf"]

###############################################################################
# Stage 3: dev —— runtime + VPP 开发头文件(vapi headers)。BUILD-ONLY，不部署。
#
#   仅供下游(如 bird-vpp)`./configure --with-vpp=/usr` 编译时取头文件用,故在
#   runtime 之上加装 vpp-dev / libvppinfra-dev(含 /usr/include/vapi 等头 + .so
#   开发链接符号)。它继承 runtime 的全部层 → 与 :master 共享底层,只多一层头。
#
#   注意:dev 是本 Dockerfile 的最后一个 stage = 默认 target,所以 CI 构建 runtime
#   镜像(:master)时必须显式 --target runtime,否则会错建成 dev。
###############################################################################
FROM runtime AS dev

RUN --mount=type=bind,from=builder,source=/pkg,target=/tmp/pkg \
    set -eux \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
        /tmp/pkg/vpp-dev_*.deb \
        /tmp/pkg/libvppinfra-dev_*.deb \
    && rm -rf /var/lib/apt/lists/* \
    # 构建期自检:vapi 头必须就位(下游 --with-vpp 靠它)
    && test -f /usr/include/vapi/vapi.h
