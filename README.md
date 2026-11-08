# ocarina-ps4

Projeto de pesquisa: rodar *The Legend of Zelda: Ocarina of Time* (Nintendo Switch 2, título `0400486024DEC000`, versão 131072) **nativamente no PS4 Pro**, sem emulador. O código ARM64 do jogo é traduzido para C antes da execução (recompilação estática), compilado para a CPU do PS4 e ligado a um runtime próprio: o sistema do Switch (Horizon) e a API gráfica NVN 2 sobre Vulkan.

**Este repositório nunca contém código, dados ou chaves do jogo.** Cada build parte do dump do próprio dono, e a pasta do jogo fica fora daqui.

O plano está em [docs/PLANO.md](docs/PLANO.md).
