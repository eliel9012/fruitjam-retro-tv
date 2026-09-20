# Ligações usadas pelo firmware

Alvo: M5Stack Core2 ESP32, com Module13.2 RCA M125 empilhado no M5-Bus.

| Uso | GPIO | Observação |
|---|---:|---|
| Vídeo composto PAL-M | 26 | Seletor físico do M125 em GPIO26; periférico I2S0 |
| RCA PCM5102 BCK | 19 | Reservado ao áudio do módulo |
| RCA PCM5102 DATA | 2 | Compartilhado com áudio interno |
| RCA PCM5102 LRCK | 0 | Compartilhado com áudio interno |
| Alto-falante interno BCK | 12 | Configurado pelo M5Unified |
| microSD CS | 4 | Slot integrado do Core2 |
| microSD SCK | 18 | Barramento compartilhado com LCD |
| microSD MISO | 38 | Não usar GPIO19 como MISO |
| microSD MOSI | 23 | Barramento compartilhado com LCD |

RCA e alto-falante interno usam I2S1 alternadamente. O firmware desinstala/encerra o dono anterior
antes de trocar a saída; I2S0 permanece reservado ao vídeo. Não são duas saídas simultâneas.

O diagrama exportado originalmente contém apenas o Core2, sem o M125 ou fios. Esta tabela documenta
o circuito usado no código, mas não substitui uma revisão elétrica do conjunto físico.

Fontes: [Core2](https://docs.m5stack.com/en/core/Core2),
[M5ModuleRCA](https://github.com/m5stack/M5GFX/blob/master/src/M5ModuleRCA.h)
e implementação de Core2 na versão M5Unified 0.2.10 utilizada pela compilação.
