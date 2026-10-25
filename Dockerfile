# syntax=docker/dockerfile:1
FROM debian:trixie-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
        g++ make pkg-config libcurl4-openssl-dev nlohmann-json3-dev \
        libavahi-client-dev python3-venv ca-certificates \
    && rm -rf /var/lib/apt/lists/*
# Debian's packaged CMake is older than this project's minimum version.
RUN python3 -m venv /opt/cmake \
    && /opt/cmake/bin/pip install --no-cache-dir cmake==4.3.4
WORKDIR /app
COPY CMakeLists.txt main.cpp *.cxx *.hxx ./
RUN /opt/cmake/bin/cmake -S . -B /build \
        -DP1SCOPE_BUILD_DESKTOP=OFF -DCMAKE_BUILD_TYPE=Release \
    && /opt/cmake/bin/cmake --build /build --parallel 2

FROM debian:trixie-slim AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends \
        libcurl4t64 libavahi-client3 ca-certificates curl tzdata \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --gid 1000 p1scope \
    && useradd --uid 1000 --gid 1000 --no-create-home p1scope
WORKDIR /app
COPY --from=build /build/p1scope-server /usr/local/bin/p1scope-server
COPY web/ /app/web/
USER p1scope
EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/p1scope-server"]
CMD ["--bind", "0.0.0.0", "--port", "8080"]
