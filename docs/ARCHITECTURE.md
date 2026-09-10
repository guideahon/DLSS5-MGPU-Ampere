# Arquitectura de implementación

## Objetivo

Implementar una ruta Linux/Proton donde el juego renderice en una RTX 3090 y el Neural Rendering se ejecute en la segunda.

## Capas

```text
Game D3D12
  ↓
Proton / VKD3D-Proton
  ↓
Vulkan image on render GPU
  ↓
exportable linear buffer
  ↓
CUDA P2P transport
  ↓
local buffer on neural GPU
  ↓
NGX / DLSS-NR
  ↓
presentation on neural GPU
```

## Backend de transporte

El transporte CUDA implementado actualmente solo trabaja con buffers lineales CUDA. Esto es deliberado: permite medir el enlace entre las GPUs sin introducir todavía la complejidad de Vulkan external memory.

Backends previstos:

```text
CudaP2P
VulkanExternalMemory
D3D12CrossAdapter
HostStagingFallback
```

La selección debe ser explícita y registrada en logs. Nunca se debe etiquetar una copia como P2P si el driver está haciendo staging por RAM.

## Ring buffer futuro

Cada slot tendrá:

- buffer de transporte;
- buffer local en GPU B;
- frame id;
- timestamp de captura;
- timestamp de presentación;
- fence de productor;
- fence de consumidor;
- checksum opcional;
- formato, tamaño y pitch.

Para Neural Rendering se puede usar una política `latest-wins`, descartando frames viejos para limitar latencia. Para Frame Generation esto no es válido sin conservar la continuidad temporal.

## Restricciones

- No incluir DLLs de NVIDIA en este repositorio.
- No incluir modelos, pesos, CUBINs ni archivos de juegos.
- No probar inicialmente con anti-cheat.
- No activar Frame Generation hasta que NR remoto sea estable.
- Registrar versión exacta de driver, Proton, VKD3D y runtime DLSS.
