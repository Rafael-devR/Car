# Car — DirectX 12 Top-Down Crime City

Reconstrução do projeto inspirada na leitura visual e no ritmo dos jogos urbanos top-down clássicos, mas com mundo 3D em DirectX 12.

## O que já existe nesta reconstrução

- câmera ortográfica quase vertical, sempre centralizada no jogador ou no carro;
- cidade 3D com ruas, calçadas, parques, prédios variados, árvores, postes e telhados;
- materiais mais claros e legíveis, sem a exposição escura da versão antiga;
- personagens e veículos com visual top-down por sprites/cards dentro do mundo 3D;
- shadow map 2048×2048 para prédios e objetos sólidos;
- jogador a pé com mira pelo mouse, corrida e tiro;
- tráfego com múltiplos carros, desaceleração e mudanças de direção em cruzamentos;
- NPCs com estados de passeio, fuga, reação agressiva e morte/respawn;
- polícia e nível de procurado de 0 a 5;
- policiais perseguem e atiram;
- carros dirigíveis e entrada/saída com E;
- projéteis, dano, vida, colete, munição e dinheiro;
- HUD em DirectX com barras, procurado, dinheiro e munição;
- objetivo/missão simples com ponto de coleta e entrega;
- GitHub Actions gera um ZIP Windows x64 automaticamente.

## Controles

- **WASD** — andar / dirigir
- **Shift** — correr
- **Mouse** — direção da mira
- **Clique esquerdo** ou **Space** — atirar
- **E** — entrar/sair de veículo
- **Roda do mouse** — zoom
- **Esc** — sair

## Compilar

Requer Windows 10/11, Visual Studio 2022 com Desktop development with C++ e Windows SDK.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

Executável: `build/Release/Car.exe`.

A versão antiga foi preservada na branch `prototype-v1`.
