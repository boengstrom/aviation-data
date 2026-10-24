# Aviation Data Front End — CIS 51. Two-stage build: compile the C++ service,
# then run it in a slim image. Works on any Docker host (incl. a Hostinger VPS
# with Docker, or Render/Railway-style platforms).
FROM debian:bookworm-slim AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends g++ \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY airline_service.cpp httplib.h ./
RUN g++ -std=c++17 -O2 -pthread -o airline_service airline_service.cpp

FROM debian:bookworm-slim
WORKDIR /app
COPY --from=build /src/airline_service .
COPY data ./data
# The /get-code page reads the source at runtime; keep it next to the binary.
COPY airline_service.cpp .
EXPOSE 8080
CMD ["./airline_service", "data"]
