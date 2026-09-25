#pragma once
// Camada fina de compatibilidade ESP32 -> RP2350 (arduino-pico + FreeRTOS SMP).
//
// O firmware original foi escrito contra o ESP-IDF: heap_caps_*, ps_malloc,
// esp_timer_get_time, ESP.getFreeHeap, xTaskCreatePinnedToCore. Em vez de
// reescrever cada chamada, estas equivalências mantêm o diff contra o upstream
// pequeno — o que importa num fork que ainda quer puxar correções de lá.
//
// Diferenças que NÃO dá para esconder atrás de um nome, e que o chamador precisa
// saber:
//
// - Pilha do FreeRTOS: no ESP-IDF o tamanho é em BYTES; no FreeRTOS "de verdade"
//   é em PALAVRAS de 32 bits. O xTaskCreatePinnedToCore daqui recebe BYTES (como
//   o código original) e divide por 4.
// - Não existe DMA-capable vs. interna: toda a SRAM do RP2350 serve ao DMA.
//   MALLOC_CAP_DMA/INTERNAL caem na SRAM (malloc); MALLOC_CAP_SPIRAM na PSRAM.
// - A PSRAM do Fruit Jam (8 MB, QSPI no CS 47) é mapeada em memória atrás de um
//   cache de 16 KB. Leitura sequencial é boa; acesso aleatório é lento. Framebuffer
//   do vídeo NÃO vai para lá (ver fj/Display.h).

#include <Arduino.h>
#include <FreeRTOS.h>
#include <semphr.h>
#include <task.h>

#include <stdint.h>
#include <stdlib.h>

#ifndef FRUITJAM_RETRO_TV
#define FRUITJAM_RETRO_TV 1
#endif

// ---- memória ---------------------------------------------------------------
#ifndef MALLOC_CAP_SPIRAM
#define MALLOC_CAP_EXEC (1u << 0)
#define MALLOC_CAP_32BIT (1u << 1)
#define MALLOC_CAP_8BIT (1u << 2)
#define MALLOC_CAP_DMA (1u << 3)
#define MALLOC_CAP_SPIRAM (1u << 10)
#define MALLOC_CAP_INTERNAL (1u << 11)
#define MALLOC_CAP_DEFAULT (1u << 12)
#endif

inline void *ps_malloc(size_t n) { return pmalloc(n); }
inline void *ps_calloc(size_t count, size_t size) { return pcalloc(count, size); }

inline void *heap_caps_malloc(size_t n, uint32_t caps) {
  return (caps & MALLOC_CAP_SPIRAM) ? pmalloc(n) : malloc(n);
}
inline void *heap_caps_calloc(size_t count, size_t size, uint32_t caps) {
  return (caps & MALLOC_CAP_SPIRAM) ? pcalloc(count, size) : calloc(count, size);
}
// free() do arduino-pico reconhece ponteiros da PSRAM e devolve ao heap certo.
inline void heap_caps_free(void *p) { free(p); }

inline size_t heap_caps_get_free_size(uint32_t caps) {
  return (caps & MALLOC_CAP_SPIRAM) ? (size_t)rp2040.getFreePSRAMHeap()
                                    : (size_t)rp2040.getFreeHeap();
}
// O arduino-pico não expõe o maior bloco livre. Devolver o total livre seria
// mentir para quem decide se cabe uma alocação; metade é um palpite conservador,
// e o chamador trata a falha do malloc de qualquer forma.
inline size_t heap_caps_get_largest_free_block(uint32_t caps) {
  return heap_caps_get_free_size(caps) / 2;
}

// ---- relógio ---------------------------------------------------------------
inline int64_t esp_timer_get_time() { return (int64_t)time_us_64(); }

// ---- aleatório -------------------------------------------------------------
inline uint32_t esp_random() { return rp2040.hwrand32(); }

// ---- estatísticas estilo ESP -----------------------------------------------
struct FjEspCompat {
  uint32_t getFreeHeap() const { return (uint32_t)rp2040.getFreeHeap(); }
  uint32_t getHeapSize() const { return (uint32_t)rp2040.getTotalHeap(); }
  // Sem registro de mínimo histórico no arduino-pico: reporta o livre atual.
  uint32_t getMinFreeHeap() const { return (uint32_t)rp2040.getFreeHeap(); }
  uint32_t getFreePsram() const { return (uint32_t)rp2040.getFreePSRAMHeap(); }
  uint32_t getPsramSize() const { return (uint32_t)rp2040.getTotalPSRAMHeap(); }
  uint32_t getCpuFreqMHz() const { return (uint32_t)(rp2040.f_cpu() / 1000000u); }
  void restart() { rp2040.reboot(); }
};
extern FjEspCompat ESP;

// ---- tarefas ---------------------------------------------------------------
// Assinatura do ESP-IDF, pilha em BYTES. core < 0 = qualquer núcleo.
inline BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stackBytes,
                                          void *arg, UBaseType_t prio, TaskHandle_t *handle,
                                          BaseType_t core) {
  const configSTACK_DEPTH_TYPE words = (configSTACK_DEPTH_TYPE)((stackBytes + 3) / 4);
  if (core < 0 || core > 1)
    return xTaskCreate(fn, name, words, arg, prio, handle);
  return xTaskCreateAffinitySet(fn, name, words, arg, prio, (UBaseType_t)(1u << core), handle);
}
#ifndef tskNO_AFFINITY
#define tskNO_AFFINITY (-1)
#endif
#ifndef ESP_OK
#define ESP_OK 0
#endif
