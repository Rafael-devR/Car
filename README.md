# Car — DX12 Open City

Projeto de jogo 3D em C++/DirectX 12 inspirado na câmera e no ritmo dos primeiros jogos urbanos top-down, reinterpretado como uma cidade totalmente 3D.

A base do projeto inclui:
- renderer DirectX 12;
- câmera alta com perspectiva;
- cidade 3D gerada em grade com ruas, calçadas, prédios e árvores;
- materiais texturizados;
- iluminação direcional;
- shadow map em tempo real;
- personagem controlável;
- carro dirigível;
- build por CMake/Visual Studio 2022;
- GitHub Actions para gerar artefato Windows.

## Controles

- **WASD** — mover personagem / dirigir
- **Shift** — correr
- **E** — entrar/sair do carro
- **Q / R** — girar câmera
- **Mouse wheel** — zoom
- **Esc** — sair

## Compilar

Requer Windows 10/11, Visual Studio 2022 com Desktop development with C++ e Windows SDK.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

Executável: `build/Release/Car.exe`.
