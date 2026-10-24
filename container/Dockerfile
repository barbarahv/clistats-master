# 1. Etapa de Compilação (Ambiente com o compilador G++)
FROM gcc:latest AS build
WORKDIR /src

# Copia todo o projeto para dentro do Docker
COPY . .

# Compila gerando apenas um único executável unificado para cada pasta
RUN g++ -O3 -o prog_sequencial $(find ./sequencial -name "*.cpp")
RUN g++ -O3 -o prog_processos $(find ./processos -name "*.cpp")
RUN g++ -O3 -o prog_threads $(find ./threads -name "*.cpp") -pthread

# 2. Etapa Final (Gera o pacote fechado limpo e super leve)
FROM ubuntu:latest AS final
WORKDIR /app

# Instala dependências básicas de runtime do C++
RUN apt-get update && apt-get install -y libstdc++6 && rm -rf /var/lib/apt/lists/*

# Copia os 3 executáveis oficiais e dinâmicos
COPY --from=build /src/prog_sequencial .
COPY --from=build /src/prog_processos .
COPY --from=build /src/prog_threads .

# Por padrão, inicia o sequencial se não especificar nada
ENTRYPOINT ["./prog_sequencial"]
