# The workflow supplies an immutable image digest for each baseline/architecture.
ARG BASE_IMAGE
FROM ${BASE_IMAGE}
USER root
SHELL ["/bin/bash", "-o", "pipefail", "-c"]
# manylinux supplies the compiler, make, curl, and supported Python installations.
# Do not bootstrap retired CentOS package repositories for the compatibility build.
RUN if command -v apt-get >/dev/null; then \
      apt-get update && \
      DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential binutils ca-certificates curl python3 && \
      rm -rf /var/lib/apt/lists/*; \
    else \
      test -x /opt/python/cp311-cp311/bin/python && \
      ln -sf /opt/python/cp311-cp311/bin/python /usr/local/bin/python3 && \
      command -v gcc && command -v make && command -v curl; \
    fi
# Static PCRE2 keeps the test nginx portable between the baseline and CI host.
# This library is linked into the test executable, not distributed in the module.
RUN curl --fail --show-error --location --retry 3 \
      https://github.com/PCRE2Project/pcre2/releases/download/pcre2-10.45/pcre2-10.45.tar.gz \
      -o /tmp/pcre2.tar.gz && \
    echo '0e138387df7835d7403b8351e2226c1377da804e0737db0e071b48f07c9d12ee  /tmp/pcre2.tar.gz' | sha256sum -c - && \
    tar -xzf /tmp/pcre2.tar.gz -C /tmp && \
    cd /tmp/pcre2-10.45 && \
    ./configure --prefix=/usr/local --libdir=/usr/local/lib \
      --disable-shared --enable-static --disable-pcre2grep-libz \
      --disable-pcre2grep-libbz2 && \
    make -j2 && make install && \
    rm -rf /tmp/pcre2-10.45 /tmp/pcre2.tar.gz
WORKDIR /work
