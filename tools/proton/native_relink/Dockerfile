FROM ubuntu@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates curl gnupg build-essential cmake ninja-build python3 ruby perl git npm nasm pkg-config libicu-dev libssl-dev libxml2-dev zlib1g-dev libedit-dev libncurses-dev xz-utils unzip patch && rm -rf /var/lib/apt/lists/*
COPY llvm.asc /etc/apt/trusted.gpg.d/llvm.asc
RUN printf 'deb https://apt.llvm.org/noble/ llvm-toolchain-noble-23 main\n' > /etc/apt/sources.list.d/llvm.list && apt-get update && apt-get install -y --no-install-recommends clang-23=1:23.1.5~++20261028083854+a738f2737189-1~exp1~20261028203912.92 lld-23=1:23.1.5~++20261028083854+a738f2737189-1~exp1~20261028203912.92 llvm-23=1:23.1.5~++20261028083854+a738f2737189-1~exp1~20261028203912.92 llvm-23-dev=1:23.1.5~++20261028083854+a738f2737189-1~exp1~20261028203912.92 libclang-rt-23-dev=1:23.1.5~++20261028083854+a738f2737189-1~exp1~20261028203912.92 && rm -rf /var/lib/apt/lists/*
COPY rust-components.json /tmp/rust-components.json
COPY install_rust.py /tmp/install_rust.py
RUN python3 /tmp/install_rust.py
RUN /usr/lib/llvm-23/bin/clang --version && /opt/rust/bin/rustc -vV && /opt/rust/bin/cargo -vV && dpkg-query -W > /opt/dpkg-packages.txt
RUN apt-get update && apt-get install -y --no-install-recommends gperf bison flex python3-packaging && rm -rf /var/lib/apt/lists/*
RUN mkdir -p /p3/tmp /p3/home && chown -R 1000:1000 /p3
ENV PATH=/opt/rust/bin:/usr/lib/llvm-23/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
ENV HOME=/p3/home TMPDIR=/p3/tmp CMAKE_BUILD_PARALLEL_LEVEL=2 CARGO_BUILD_JOBS=2 RUSTUP_AUTO_INSTALL=0
USER 1000:1000
WORKDIR /p3
CMD ["sleep", "infinity"]
