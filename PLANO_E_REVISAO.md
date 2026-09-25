# Plano e revisão — Fruit Jam Retro TV

Este arquivo tem duas partes:

1. **O fork para o Adafruit Fruit Jam** — o que falta conferir no aparelho, na
   ordem em que vale a pena conferir.
2. **O histórico do upstream** (m5-retro-tv, M5Stack Core2 + módulo RCA),
   preservado como estava. Serve de referência para os bugs que já foram achados
   lá e para os números medidos no Core2 — que **não valem** para o Fruit Jam.

---

## Parte 1 — Fruit Jam: roteiro de teste físico

**Nada do fork foi testado no aparelho.** O port compila contra as bibliotecas
do Fruit Jam e os testes nativos rodam no PC; imagem, cor, som, cartão, rede e
botões dependem de uma placa na bancada. Cada item abaixo começa como
**pendente**. Ao testar, anote a data, o resultado e o que foi observado — do
mesmo jeito que a "Bancada USB em 20/09/2026" do upstream, mais abaixo.

Material: Fruit Jam, cabo USB-C, monitor HDMI (de preferência dois: um monitor
de computador e uma TV), fone ou caixinha no P2, o alto-falante da placa, um
cartão FAT32 com um programa de vídeo em 320×240 e outro em 240×160, uma pasta de
música com MP3 e WAV, fotos convertidas, e — para o item 4 — um conversor
HDMI→AV e uma TV de tubo.

A ordem importa: os primeiros itens são pré-requisito dos seguintes, e o de maior
risco (cor) vem logo depois de haver imagem.

### 0. Gravação e console — pendente

- Segurar BOOT, apertar RESET: a placa aparece como drive `RP2350`.
- `pio run --target upload` (ou copiar o `firmware.uf2`) grava e a placa
  reinicia sozinha.
- O console serial a 115200 aparece pela USB e `diag status` responde.
- Com o console aberto, anotar a SRAM e a PSRAM livres logo depois do boot
  (`diag mem`). A conta do `PORTING.md` prevê ~300 KB de SRAM livres depois do
  framebuffer.

### 1. O DVI sobe? — pendente

- O monitor reconhece o sinal como **640×480 a 60 Hz** (a maioria mostra isso no
  OSD dele).
- A tela inicial aparece inteira, sem deslocamento, sem faixa preta lateral, sem
  linha repetida. O quadro de 320×240 deve ocupar os 640×480 exatos.
- A TV também aceita (algumas TVs HDMI recusam 640×480; se recusar, anotar o
  modelo — não é defeito do firmware, mas vai para o README).
- Sem imagem nenhuma: conferir se o `display::begin()` devolveu `true` (sem
  memória para o framebuffer ele falha) e se ele roda **antes** de qualquer
  tarefa.

### 2. As cores estão certas? — pendente, **maior risco do port**

O `LGFX_Sprite` guarda RGB565 com os bytes trocados e o DVHSTX espera a ordem
normal. Em vez de converter cada pixel, `configureSwappedRgb565()` em
`src/fj/Display.cpp` reprograma o `expand_tmds` do HSTX para ler os campos já
trocados (vermelho com rotação 0, azul com rotação 5, verde com rotação 27, que
só funciona porque o pixel vem duplicado nas duas metades da palavra). É a parte
do port que mais depende de uma leitura correta do datasheet, e nenhuma
ferramenta de PC a confere.

Como testar:

1. **PADRÃO DE TESTE**, barras SMPTE. A fileira de cima, da esquerda para a
   direita, tem de ser: **cinza, amarelo, ciano, verde, magenta, vermelho, azul**.
2. Uma imagem com degradê suave (uma foto de céu convertida pelo
   `prepare_photos.py` serve) — o degradê tem de ser liso, sem serrilhado a cada
   8 tons.
3. `diag scan` e `diag cores` leem o framebuffer **de volta**, antes do HSTX. Se
   eles mostram os valores certos e a tela mostra errado, o problema está no
   codificador TMDS, não no desenho. Se eles já mostram errado, o problema está
   antes (LovyanGFX, JPEGDEC).

Diagnóstico pelo sintoma:

| O que se vê | Causa provável | Onde mexer |
|---|---|---|
| amarelo ↔ ciano e vermelho ↔ azul trocados; verde e magenta certos | pistas 0 e 2 invertidas | `L0_ROT`/`L2_ROT` no `expand_tmds`, ou a ordem das pistas no pinout do DVHSTX |
| tudo certo, mas o verde tem degraus ou serrilhado nos degradês | a metade G2..G0 (a cópia na metade alta) não está chegando | `L1_ROT = 27`; confirmar que o modo é 320×240 com dobra horizontal |
| cores sem sentido em todo lugar, geometria certa | o `expand_tmds` não foi aplicado, ou foi sobrescrito depois | conferir que `configureSwappedRgb565()` roda **depois** do `dvi.init()` e que nada reinicia o HSTX |
| interface certa, **só o vídeo e as fotos** errados | JPEGDEC emitindo `RGB565_LITTLE_ENDIAN` | tipo de pixel do JPEGDEC tem de ser `RGB565_BIG_ENDIAN` |
| cores certas mas escuras ou lavadas | nível de faixa do monitor (limitado × completo) | ajuste do monitor, não do firmware; anotar |

Plano B, se o truque do registrador não fechar: deixar o HSTX no padrão e trocar
os bytes na origem (JPEGDEC em little-endian e o sprite sem troca). Custa tempo
por quadro; medir com `diag bench` antes e depois.

### 3. Geometria e rasgo — pendente

- A área segura aparece como margem de fundo, sem moldura preta.
- Um vídeo 320×240 enche a tela; um 240×160 aparece centralizado.
- Com o vídeo rodando, observar rasgo horizontal (o framebuffer é simples). Um
  rasgo ocasional é esperado; um rasgo parado no meio da tela, não.

### 4. TV de tubo pelo conversor HDMI→AV — pendente

- Conversor em NTSC: imagem estável e colorida numa TV PAL-M.
- Conferir no padrão de teste quanto o tubo corta, e se o texto fica dentro da
  área segura em todas as telas.
- Áudio do P2 para as entradas RCA da TV.

### 5. Áudio — fone e alto-falante — pendente

- Tom de 1 kHz do padrão de teste no **fone (TV (P2))**: limpo, sem chiado, sem
  estalo periódico.
- O mesmo no **ALTO-FALANTE**.
- Trocar de saída em CONFIGURAÇÕES → SAIDA AUDIO durante a música: troca sem
  travar a tarefa de áudio.
- Volume 0 / 50 / 100 e mudo nas duas saídas.
- Taxas: vídeo e WAV a 22.050 Hz, MP3 reamostrado. Sem mudança de tom (que
  denunciaria taxa errada no I2S ou no PLL do DAC).
- **A armadilha do GPIO 22:** o som tem de continuar **depois** que o Wi-Fi
  conecta, e depois de abrir e fechar o portal. Se o áudio morrer em silêncio no
  primeiro acesso à rede, algo reiniciou o ESP32-C6 depois de o DAC ter sido
  configurado.
- Sincronismo labial num vídeo com fala.

### 6. Cartão SD por SDIO — pendente

- Monta no boot; a detecção do soquete reconhece tirar e pôr o cartão.
- Biblioteca de vídeo, música e fotos listam tudo.
- `diag bench` num programa 240×160 e num 320×240: anotar leitura (MB/s), decode,
  desenho e FPS sustentado. **Estes são os primeiros números do Fruit Jam.**
- **A armadilha do `FILE_WRITE`:** mudar uma configuração, reiniciar, mudar de
  novo, reiniciar. O `settings.json` tem de continuar JSON válido (abrir o
  arquivo no PC). Se ele crescer a cada gravação, alguém gravou com `FILE_WRITE`,
  que no arduino-pico anexa.
- Recuperação do `.bak`: tirar a energia no meio de uma gravação de configuração
  e conferir que o boot seguinte recupera.
- Vídeo longo tocando até o fim sem underrun de áudio (`diag status`).

### 7. Wi-Fi pelo ESP32-C6 (NINA) — pendente

- A versão do firmware NINA aparece em SISTEMA / `diag status`.
- Conecta à rede salva no boot; reconecta depois de o roteador cair e voltar.
- **Portal:** o ponto de acesso aparece, o celular abre a página pelo endereço
  mostrado na tela, salvar grava `secrets.json` válido. Anotar se a página abre
  sozinha (DNS cativo) ou só digitando o IP.
- **Previsão:** Open-Meteo responde e a tela preenche.
- **Radar:** API HTTPS com certificado de CA pública responde (TLS no C6). Uma com
  CA privada deve **falhar com mensagem**, não travar.
- **Rádio:** o stream toca por 30 minutos sem cortar e o título atualiza.
- **Transferência:** subir um vídeo de dezenas de MB, derrubar o Wi-Fi no meio e
  retomar.
- **Concorrência do SPI1:** radar ou previsão atualizando enquanto o `loop()`
  consulta o estado do Wi-Fi, por uma hora. Um NINA travado aparece como rede
  que para de responder até o reset — é o sintoma de uma chamada sem
  `net::Lock`.

### 8. Hora (NTP) — pendente

- Antes do Wi-Fi: o relógio mostra `--:--`, não uma hora inventada.
- Depois do Wi-Fi: hora de Brasília (UTC−3), não UTC.
- SISTEMA mostra a origem da hora.
- As datas da previsão de três dias batem com o calendário.

### 9. Botões — pendente

- A, B, C navegam; auto-repeat ao segurar A ou C numa lista.
- Segurar B 0,7 s volta; 1,5 s vai ao início.
- **A e C juntos** vão ao início, de qualquer tela, inclusive durante o vídeo.
- Nenhum toque duplo: um aperto, uma ação.
- Segurar o botão 1 (BOOT) e apertar RESET entra no modo de gravação —
  esperado; anotar para não confundir com defeito.

### 10. Soft-off — pendente

- DESLIGAR: tela preta (anotar se o monitor entra em espera ou só mostra preto),
  áudio mudo nas duas saídas, NeoPixels apagados.
- Qualquer botão reinicia a placa e ela volta ao início.
- **Acordar com o botão 1**: conferir que a placa volta ao firmware e **não**
  cai no modo de gravação (o botão 1 é também o BOOT e o despertar reinicia a
  placa).
- O temporizador DESLIGAR EM leva ao mesmo soft-off.
- Consumo em soft-off, se houver como medir.

### 11. Firmware autônomo `weather/` — pendente

- Grava, sobe, busca a previsão, toca a música em loop no P2 e no alto-falante.
- Sair e voltar da tela não congela (armadilha 3 do `AGENTS.md`: `stopProgram()`
  esperando `audioIdle`).

### Limites conhecidos do fork

- Sem RTC: sem Wi-Fi não há hora.
- Sem TLS configurável: CA privada não valida (o `ca.pem` é ignorado).
- DVI sem áudio: o som sai pelo P2 ou pelo alto-falante.
- `heap_caps_get_largest_free_block()` e o mínimo histórico de heap são
  aproximações (ver `AGENTS.md` 2.2); os números de memória do diagnóstico não
  são comparáveis aos do Core2.
- Nenhum número de desempenho do Fruit Jam existe ainda. Os da Parte 2 são do
  Core2.

---

## Parte 2 — histórico do upstream (m5-retro-tv, Core2)

Preservado como estava no commit `21aa210` do
[m5-retro-tv](https://github.com/eliel9012/m5-retro-tv), de onde o fork partiu.
Pinos, bibliotecas, números de desempenho e testes físicos abaixo são do
**M5Stack Core2 + Module13.2 RCA**, não do Fruit Jam.

### Revisão e plano executado — M5 RETRO TV

Revisão do projeto em `/Users/eliel/Documents/TV`, com prioridade adicional para o player.
As correções foram aplicadas aos arquivos locais e sincronizadas com `schematik-project.json`.
O original foi preservado em um arquivo de backup separado.

#### Etapas

| Etapa | Entrega | Situação |
|---|---|---|
| 1. Inventário e preservação | Leitura do firmware, exportação e dependências; backup integral | Concluída |
| 2. Compilação reproduzível | PlatformIO, placa Core2 e bibliotecas com versões fixas | Concluída |
| 3. Player | Leitura em blocos, descarte real, fechamento coordenado, volume e prévia | Implementada; testes de software e USB |
| 4. Navegação e configuração | Menu, toque, paginação, portal, persistência e reconexão | Implementada; testes de software e USB |
| 5. Rede/radar | Vida útil do certificado, autenticação, validação de posição e timeouts | Validado no Core2: HTTPS 200 e 22 aeronaves |
| 6. Verificação e distribuição | Testes nativos, decodificação real, build ESP32, exportação consistente | Concluída |
| 7. Bancada | Gravação no Core2, NTSC, som, toque físico e medição de desempenho | Deploy USB e testes do player executados; RCA físico pendente |

#### Falhas encontradas e correções

| Problema anterior | Correção |
|---|---|
| Vídeo roxo/verde apesar dos controles corretos | Blocos JPEG enviados explicitamente como RGB565 nativo, sem inversão de bytes |
| Player sem seleção rápida da saída de som | Botão AUDIO: M5 / RCA, com preferência persistente e CVBS sempre ativo |
| Telas sem controle de retorno visível | Seta no canto superior direito de todas as telas de navegação, exceto home |
| Consulta TLS ocupava o loop da interface | Tarefa separada com publicação atômica do resultado para a interface |
| API MeuLabApp não reconhecida | Endpoint /api/adsb/aircraft e suporte aos campos items, speed_kt e model |
| Item de menu convertido por soma de enumeração; telas erradas | Mapeamento explícito das quatro telas |
| Toque na imagem/fora da barra também produzia SELECT ao soltar | Apenas um evento; limites da barra e da área inferior separados |
| HOME durante reprodução deixava áudio e arquivos ativos | Encerramento coordenado antes da navegação |
| Fechamento de arquivos após espera fixa de 20 ms | Confirmação de inatividade da tarefa consumidora antes de fechar |
| Contador PCM de 64 bits `volatile` compartilhado entre núcleos | Contador atômico de 32 bits, suficiente para WAV/FAT, e flags atômicas |
| Leitura do MJPEG byte a byte no SD | Buffer de 4 KiB conservando bytes entre quadros |
| Quadros descartados ainda eram decodificados e desenhados | Avanço do fluxo comprimido sem JPEGDEC; trabalho limitado por passagem do loop |
| OSD redesenhado em toda passagem do loop | Atualização por quadro novo, evento ou intervalo de 250 ms |
| Primeiro quadro só aparecia depois de avançar o áudio | Decodificação e exibição antes de iniciar PCM |
| Posição/desenho fixados em 240×160 | Dimensões obtidas do JPEG e limite de 320×240 |
| Prévia cortava imagens maiores e invadia a lógica de enquadramento | Canvas em PSRAM com redução proporcional na área de 320×160 do LCD |
| Volume ignorado na saída RCA | Escala PCM com saturação do percentual a 0–100 |
| Alto-falante desabilitado sem configuração de pinos; troca I2S conflitante | Configuração pelo M5Unified, propriedade exclusiva de I2S1 e desativação do BCK anterior |
| `playRaw` podia escolher canais diferentes a cada bloco | Uso sequencial do canal zero |
| WAV truncado era aceito e tamanhos podiam transbordar | Validação de RIFF, chunks, alinhamento, formato, tamanho e limites com soma de 64 bits |
| Lista mostrava apenas quatro programas, mesmo selecionando outros | Paginação mantendo a seleção visível |
| Portal salvava sem iniciar conexão | Início explícito da conexão em AP+STA e retorno ao menu após sucesso |
| Expiração deixava a interface na tela do portal inativo | Retorno ao início e retomada do gerenciador de rede |
| Teste Wi-Fi anunciava internet disponível sem verificar | Mensagem limitada ao que foi testado; teste sem descartar formulário |
| Modo de autenticação e atualização voltavam ao primeiro valor do formulário | Seleção restaurada a partir das configurações salvas |
| Token/senha mantidos sem forma de limpar uma rede aberta | Preservação na mesma rede e opção explícita de rede sem senha |
| Formulário aceitava coordenadas inválidas e conteúdo de cabeçalho com CR/LF | Validação, respostas 400 e token de sessão nas alterações |
| Rotas registradas novamente a cada abertura do portal | Registro único e reinicialização do estado da sessão |
| Gravação apagava a configuração anterior antes de confirmar a nova | Arquivo temporário, backup, troca e recuperação de interrupção |
| Preferências do player perdidas ao salvar pelo portal | Persistência compartilhada incluindo OSD e saída de áudio |
| Configuração offline ignorada por SecretsManager | Preferências carregadas antes de verificar credenciais |
| Reconexão retinha estado da tentativa anterior; contador transbordava | Estado explícito, backoff, saturação e deadlines resistentes ao rollover |
| Certificado TLS apontava para uma String já destruída | PEM permanece vivo até o fim da requisição |
| Modo sem autenticação ainda enviava credencial; X-API-Key usava cabeçalho incorreto | Tratamento separado por modo |
| Posição sem longitude ou fora da faixa aparecia no radar | Validação de ambas as coordenadas e limite antes de projetar pixels |
| Tela de detalhes do radar não alterava o desenho | Exibe identificador, tipo e direção |
| Ausência de configuração local de compilação; referências de bibliotecas não resolvidas | Ambiente PlatformIO validado com versões disponíveis |
| Arquivo Schematik tinha cópias independentes das fontes | Sincronizador e verificação automatizada |

Também foi incluído um conversor FFmpeg/FFprobe com preservação de proporção, duração alinhada,
áudio compatível e validação de quadros, além de documentação de uso e pinagem.
As fontes C++ foram formatadas para permitir manutenção e revisão.

#### Evidências de teste

- Compilação e link reais para ESP32/Core2 com `espressif32@6.12.0`.
- M5Unified 0.2.10, M5GFX 0.2.29, ArduinoJson 7.4.2 e JPEGDEC 1.8.2.
- Testes nativos de leitura MJPEG, incluindo os 4.096 possíveis deslocamentos de fronteira,
  fim de arquivo, excesso de tamanho e truncamento.
- Testes WAV com chunk extra de tamanho ímpar, truncamento, overflow, taxa/canais/bits inválidos.
- Testes de volume PCM, falhas simuladas de gravação/rename e recuperação do backup.
- Testes do código real de InputManager, NetworkManager e ConfigurationPortal com hardware simulado.
- Casos do portal: CSRF, coordenadas, CR/LF, preservação de segredo, defaults, rede aberta,
  expiração, reabertura e status JSON do teste de conexão.
- Vídeo sintético de dois segundos: 30 quadros 240×160 e PCM estéreo de 176.400 bytes de dados.
  O leitor fez 40 leituras em blocos, incluindo EOF, em vez de uma chamada ao cartão por byte.
- Segundo vídeo: 30 quadros 320×240; áudio ausente convertido em silêncio de duração equivalente.
- Todos os 60 quadros foram decodificados com sucesso pela JPEGDEC utilizada no firmware.
- AddressSanitizer nos testes de decodificação; AddressSanitizer e UndefinedBehaviorSanitizer nos testes próprios.
- Verificação de equivalência das fontes e dependências incorporadas no pacote Schematik.

A contagem de leituras é do teste em memória; não é uma medição de FPS ou velocidade física do SD.

#### Conferências físicas ainda necessárias

1. Conectar uma TV/monitor e alto-falantes às saídas RCA; conferir NTSC, cores, som e sincronismo labial.
   O sinal saiu de `PAL_M` para `NTSC`: a tabela PAL_M do M5GFX usa 908 amostras por linha, contra as
   909,02 exigidas por 4 × 3,57561149 MHz, o que fazia a fase da burst andar e produzia uma faixa de cor
   diagonal na tela. As telas da RCA passaram a respeitar a área segura (`SAFE_*`, ~7% de cada borda),
   porque cabeçalho e ticker caíam no overscan do tubo e apareciam cortados.
2. Confirmar visualmente no LCD a correção das cores e tocar nos novos botões de áudio e voltar.
3. Medir o perfil 320×240, cartões lentos e uma biblioteca com mais de quatro programas no hardware.
4. Conferir volume 0/50/100 e configuração do portal pelo navegador de um celular.
5. Concluir o teste de fim natural do clipe e repetir a rodada final de rede quando o USB for reconectado.

#### Limites conhecidos

O Core2 foi gravado e depurado pela USB. O usuário confirmou som no alto-falante interno.
As saídas RCA não foram conectadas pelo usuário: a geração contínua está implementada, mas
TV/áudio analógicos e sincronismo labial ainda exigem teste físico. Novas consultas de radar
não começam durante a reprodução; uma consulta já iniciada pode terminar em segundo plano.

O formato suportado continua sendo MJPEG baseline + PCM; a reprodução termina quando o áudio ou o vídeo
chega ao fim. Não foram adicionados codecs de MP4, busca temporal ou aceleração de hardware.
A saída de 320×240 e taxas maiores precisam de medição antes de serem tratadas como perfil padrão.

A proteção de arquivos é individual e recuperável, não uma transação entre `settings.json` e
`secrets.json`. Falhas físicas e corrupção FAT não podem ser eliminadas por esta rotina.

O diagrama eletrônico original tem apenas um nó Core2, sem módulo ou ligações; foi preservado.
A pinagem utilizada está descrita em `PINOUT.md`. O pacote Schematik foi sincronizado e verificado
estruturalmente, mas a reimportação na interface dessa ferramenta não foi executada.

Referências de compilação: [placa Core2 no PlatformIO](https://docs.platformio.org/en/stable/boards/espressif32/m5stack-core2.html),
[M5GFX / módulo RCA](https://github.com/m5stack/M5GFX/blob/master/src/M5ModuleRCA.h),
[JPEGDEC](https://github.com/bitbank2/JPEGDEC).

#### Bancada USB em 20/09/2026

- Core2 identificado e gravado via USB, com hash da flash verificado pelo esptool.
- Cartão reconhecido; programa “Primeiro Teste”: 240×160, 15 FPS, duração 207,54 segundos.
- Teste RGB565 no aparelho aprovado; o código envia pixels nativos tipados para LCD e RCA.
- Wi-Fi configurada e reconectada após reiniciar, IP 192.168.15.23; relógio sincronizado por NTP.
- Consulta real `/api/adsb/aircraft`: HTTP 200, 22 aeronaves válidas, 9.546 ms. CA validada, sem desabilitar TLS.
- Certificado e credenciais salvos no SD, sem incorporar senha/token às fontes ou ao binário.
- Buffer de vídeo composto movido para SRAM para eliminar a tarefa contínua de cópia da PSRAM.
  Buffers TLS usam PSRAM, preservando SRAM para vídeo/I2S; a tarefa HTTPS não bloqueia o loop.
- Botão de áudio usa pausa coordenada durante a gravação da preferência no SD compartilhado.
  Há uma breve interrupção intencional na troca; posição do arquivo e relógio são preservados.
- Seta de voltar nas telas de biblioteca, reprodução, radar, configurações, informações, portal e erro.
  Em erro de inicialização, a seta reinicia o aparelho para tentar novamente.
- Na versão final, retorno da biblioteca/radar/configurações/informações, pausa/retomada,
  mudo e alternância M5/RCA passaram pelos comandos que acionam as rotinas da interface.
  A preferência M5 sobreviveu à reinicialização; os limites dos botões passaram nos testes nativos.
- Último status completo do teste longo: 144,13 segundos de PCM reproduzidos, zero erros JPEG,
  zero underruns, zero falhas de leitura, heap livre de 72.368 bytes e mínimo de 66.140 bytes.
  Foram descartados 294 de 2.162 quadros percorridos para acompanhar o áudio; 15 FPS constantes
  não são garantidos neste clipe. A prévia LCD tem cadência menor que a saída composta.
- A porta USB desapareceu antes do término do clipe. O teste de fim natural e a rodada adicional
  de rede não foram concluídos. Isso não foi registrado como teste aprovado.
- A correção visual das cores e o toque físico nos botões aguardam confirmação do usuário.

Build final: 1.344.725 bytes de aplicacao e 84.576 bytes de dados estaticos reportados pelo linker.
Os percentuais de RAM do PlatformIO incluem PSRAM e nao representam a disponibilidade de SRAM para DMA.
