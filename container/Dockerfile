
FROM gcc:13 AS build

WORKDIR /src

COPY sequencial/ ./sequencial/
COPY processos/ ./processos/
COPY threads/ ./threads/

RUN g++ -O3 -std=c++17 sequencial/so_sequencial.cpp -o prog_sequencial

RUN g++ -O3 -std=c++17 processos/so_processos.cpp -o prog_processos

RUN g++ -O3 -std=c++17 -pthread threads/so_threads.cpp -o prog_threads

FROM ubuntu:24.04 AS final

WORKDIR /app

RUN apt-get update \
    && apt-get install -y --no-install-recommends libstdc++6 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/prog_sequencial .
COPY --from=build /src/prog_processos .
COPY --from=build /src/prog_threads .

ENTRYPOINT ["./prog_sequencial"]
