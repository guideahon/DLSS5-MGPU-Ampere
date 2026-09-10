# Registro técnico de cambios y pruebas

Este documento resume todo lo implementado durante el experimento Dual RTX 3090 / DLSS5 en Linux. Incluye resultados negativos: un stopper queda registrado aunque una prueba haya sido compilada correctamente.

## 2026-09-10 — identidad física, evaluación y gate de sincronización

- Se corrigió la aplicación reproducible de los parches del bridge: transporte, FD, fallback de evaluación y probe de fences se aplican en orden sobre un checkout limpio.
- Se normalizaron los parámetros públicos y compactos que recibe la evaluación DLSS. En la prueba positiva GE-Proton, el smoke sintético pasó de `0xbad00005` a `0x00000001`; el resultado no constituye validación de un juego real.
- Se añadió el probe `ProbeFenceFd` al bridge y se propagó `VKD3D_EXPORT_FENCE_FD` al entorno Proton. En este host la creación de fence funciona, pero `ExportVulkanFenceFd` devuelve `0x80004001` (`E_NOTIMPL`); por lo tanto la sincronización GPU↔GPU sigue siendo un gate cerrado.
- Se añadió `ID3D12DXVKInteropDevice5` con identidad UUID/PCI y exportación experimental de fence Vulkan. La selección VKD3D deduplica entradas físicas duplicadas y evita que `VKD3D_VULKAN_DEVICE` sobrescriba la selección A/B en el modo experimental.
- Verificación: A reporta `uuid=af6de4b3 pci=0:1:0.0`; B reporta `uuid=5b9f385f pci=0:3:0.0`; `multi_adapter_distinct=yes`.
- El transporte FD del output privado continúa validado: `cuImportExternalMemory`, mapeo, escritura, `cuMemcpyPeer` y checksum hacia GPU1 correctos.
- El bridge sigue sin declarar NR remoto: la evaluación DLSSNR positiva se ejecuta en el device principal y el helper sólo valida transporte hacia B. Falta importar las imágenes reales en B, sincronizarlas y ejecutar NGX con un command list de B.
- MFG remoto continúa fuera de alcance hasta que exista NR remoto estable y validación visual/temporal.

## Alcance y decisiones de arquitectura

- Se fijó la topología objetivo `GPU A = render del juego` y `GPU B = coprocesador neuronal/presentación`.
- Se descartó SLI/AFR como estrategia principal: no hay soporte moderno confiable para repartir DLSS/FG de esa forma.
- Se dejó Frame Generation remoto fuera del MVP hasta validar primero un pass real de Neural Rendering y su sincronización.
- Se mantuvo el fallback local como comportamiento obligatorio cuando falla cualquier gate.
- Se excluyeron juegos con anti-cheat del primer ciclo.
- No se incluyeron DLLs de NVIDIA, runtimes comunitarios, modelos, pesos, CUBINs ni archivos de juegos en el repositorio. Se consumen sólo desde rutas locales explícitas.

## Hardware e inventario

- Se identificaron dos `NVIDIA GeForce RTX 3090`, arquitectura SM86, driver `595.71.05`.
- Se registraron los BDF PCI:
  - GPU 0: `00000000:01:00.0`
  - GPU 1: `00000000:03:00.0`
- Se resolvió la correspondencia Vulkan/CUDA por UUID y PCI, sin asumir que los índices coinciden.
- Se verificó topología PCIe por `PHB` y mismo NUMA.
- Se agregaron comprobaciones de GPU con salida activa, memoria libre y procesos ocupantes.
- Las comprobaciones de monitores fueron sólo de lectura. No se modificaron RandR, Xorg ni modos de pantalla.

## Transporte CUDA P2P

- Se implementó enumeración CUDA y validación de `cudaDeviceCanAccessPeer` en ambas direcciones.
- Se implementó activación de peer access y copia `cudaMemcpyPeerAsync`.
- Se agregó validación mediante kernel CUDA en la GPU destino.
- Se agregaron benchmarks ida/vuelta y salida JSON.
- Se implementó un ring asíncrono de slots con eventos CUDA y política de finalización por frame.
- Se validó un ring de 100 frames 4K RGBA16F (`66.355.200` bytes) sin errores.
- Resultado observado: aproximadamente 10–12,7 GB/s según tamaño, dirección y carga.

## Interoperabilidad Vulkan → CUDA → P2P

- Se implementó enumeración Vulkan con deduplicación por UUID/PCI.
- Se implementó creación de memoria `DEVICE_LOCAL` Vulkan con exportación opaque FD.
- Se implementó importación de la asignación desde CUDA.
- Se implementó copia desde CUDA hacia la segunda RTX 3090 por P2P.
- Se agregó validación end-to-end y resolución automática del mapeo Vulkan/CUDA.
- Ambas direcciones pasan correctamente; en las corridas recientes se observaron aproximadamente 5,9–6,0 GB/s para buffers de 8 MiB.
- Se documentó que usar manualmente `Vulkan 0 → CUDA 1` falla por UUID mismatch, mientras que el selector automático usa `Vulkan 0 ↔ CUDA 0` y `Vulkan 1 ↔ CUDA 1` en este host.

## MVP automático

- Se creó `mgpu-auto doctor --json` para inventario, memoria, P2P, interop, runtimes y juegos.
- Se creó `mgpu-auto selftest --json` para ejecutar los gates técnicos.
- Se creó generación de plan con estados `READY_LOCAL_ONLY`, `READY_REMOTE`, `P2P_UNAVAILABLE` y fallback.
- Se corrigió un falso positivo: la presencia de DLLs ya no habilita `READY_REMOTE` sin `transport_available` real.
- Se agregó descubrimiento de juegos Steam/Proton y perfiles TOML aislados.
- Se agregó launcher para ejecutables Windows y Proton.
- Se fijan `VKD3D_VULKAN_DEVICE`, `VKD3D_FILTER_DEVICE_NAME` y `DXVK_FILTER_DEVICE_NAME` sólo en el proceso de prueba.
- Se agregó watchdog con finalización ordenada y fallback local.
- Se probaron 11 tests Python: todos pasan.

## NGX, DLSS y runtime comunitario

- Se creó un bridge NGX x64 sin incluir runtimes propietarios.
- Se verificó carga bajo GE-Proton 11-6.
- Se verificó inicialización del core oficial, DLSS estándar y creación del feature.
- Se verificó chaining del runtime comunitario `nvngx_dlssnr.dll` hasta `CreateFeature` en SM86.
- La evaluación sintética devuelve `0xbad00005` porque no reproduce todavía los recursos, estados y parámetros de un host real.
- La inicialización directa de NR devuelve `0xbad00002`; `Reserved18` desde el proxy devuelve `0xbad0000c`. Por ello NR no se trata como feature público independiente.
- Se verificó que dos objetos D3D12 pueden inicializar NGX y crear features sobre el mismo device efectivo.
- No se declaró éxito visual: todavía falta un juego/host real y una validación prolongada de imagen, latencia y estabilidad.

## VKD3D-Proton experimental

- Se clonó el fuente de VKD3D-Proton y se inicializaron sus submódulos.
- Se localizó que el mismo LUID DXGI para ambas 3090 hace que la selección normal reutilice un único device Vulkan.
- Se creó `vkd3d-duplicate-luid-adapters.patch` con modo opt-in `VKD3D_DUPLICATE_LUID_ADAPTERS=1`.
- El parche fuerza devices independientes y selecciona el adapter Vulkan según orden de creación cuando no se puede recuperar la identidad COM.
- Se creó `vkd3d_interop_probe.cpp` para consultar DXGI, `VkPhysicalDevice`, `VkDevice`, `VkBuffer` y `VkDeviceMemory`.
- Se añadió un gate `VKD3D_INTEROP_REQUIRE_DISTINCT=1`.
- Se comprobó que el build experimental crea dos `VkDevice` distintos y pasa `multi_adapter_distinct=yes`.
- Se comprobó que el GE-Proton instalado expone `ID3D12DXVKInteropDevice3` y `GetVulkanHeapInfo`.
- Se creó `vkd3d-export-opaque-fd-memory.patch` con el opt-in `VKD3D_EXPORT_OPAQUE_FD_MEMORY=1`.
- Ese parche habilita `VK_KHR_external_memory_fd` y añade `VkExportMemoryAllocateInfo` para probar heaps D3D12 exportables.
- Se añadió `vkd3d-fd-diagnostics.patch`, que registra desde el dispatch interno de VKD3D el tamaño, tipo, resultado de exportación y resultado de `vkGetMemoryFdPropertiesKHR`.
- Se creó un helper CUDA nativo y un launcher reproducible para pasarlo por `__wine_unix_spawnvp`.
- Se comprobó que el FD se exporta, pero que el primer launcher sólo pasaba el número: el descriptor llegaba cerrado (`fstat=EBADF`).
- Se añadió `tests/fd_inherit_shim.c`, que limpia `FD_CLOEXEC` durante el `fork/exec` controlado del probe, y `scripts/build_fd_inherit_shim.sh`.
- Se corrigieron los helpers para aceptar una GPU destino y validar escritura, importación, `cuMemcpyPeer` y checksum.
- La traza interna del heap real informó `allocation=65536`, `type=1`, `export=0`, `properties=-13` (`VK_ERROR_UNKNOWN`).
- `vkGetMemoryFdPropertiesKHR` sigue devolviendo `VK_ERROR_UNKNOWN` bajo el thunk Vulkan de Wine, pero CUDA acepta el FD cuando se hereda correctamente.
- El MVP Proton completo pasa: `cuImportExternalMemory=CUDA_SUCCESS`, `cuExternalMemoryGetMappedBuffer=CUDA_SUCCESS`, `cuMemsetD8=CUDA_SUCCESS`, `cuMemcpyPeer=CUDA_SUCCESS` y `cuda_helper_p2p_validation=ok` desde la GPU render hacia la segunda 3090.
- Se comprobó que habilitar sólo la extensión y `VkExportMemoryAllocateInfo` no elimina el fallo.

## Mejora implementada: MVP automático de transporte

- Se añadió `scripts/run_mgpu_mvp.sh`, que compila helper/shim, ejecuta Vulkan→CUDA→P2P en ambas direcciones y luego ejecuta el smoke Proton→CUDA→P2P.
- El script devuelve JSON resumido y sólo declara `READY_REMOTE_TRANSPORT` cuando la validación end-to-end pasa; nunca lanza un juego automáticamente.
- Se corrigió el selector experimental `VKD3D_DUPLICATE_LUID_INDEX` y se agregó verificación de UUID/PCI en el smoke.
- En este host, Vulkan bajo VKD3D sigue exponiendo propiedades idénticas para las dos entradas duplicadas. Por eso el selector no demuestra todavía que el segundo `ID3D12Device` sea físicamente la GPU1; la ruta validada usa GPU0 como origen y CUDA GPU1 como destino.

## Stoppers encontrados

- `CreateSharedHandle` cross-adapter D3D12 devuelve `E_NOTIMPL` para heaps y `DXGI_ERROR_INVALID_CALL` para recursos bajo VKD3D Unix.
- VKD3D stock mantiene selección Vulkan efectiva global por proceso.
- VKD3D experimental abre dos devices, pero NGX/proxy conserva estado efectivo para un solo device: el segundo `CreateFeature` devuelve `0xbad00007`. Invertir el orden invierte cuál funciona.
- El `VkDeviceMemory` extraído de un heap D3D12 exporta un FD; la consulta `vkGetMemoryFdPropertiesKHR` falla bajo Wine, pero la importación CUDA funciona cuando el FD se hereda sin `CLOEXEC`.
- La instrumentación interna reproduce el fallo sin pasar por el ABI del probe: el bloqueo está en la asignación/handle externo generado por VKD3D.
- No existe todavía un contrato de sincronización para fences/semaphores entre el juego, VKD3D, el bridge y GPU B.
- No existe todavía un host real que entregue recursos/estados de DLSS-NR a la evaluación experimental.
- No se descargó ningún juego real automáticamente: el MVP de transporte queda deliberadamente antes del lanzamiento de un título.

## Verificación final registrada

- CMake: correcto.
- Suite Python: `11/11 OK`.
- Sintaxis shell: correcta.
- P2P bidireccional: correcto.
- Ring asíncrono: correcto, 0 fallos de validación.
- Vulkan→CUDA→P2P: correcto en ambas direcciones.
- `mgpu-auto doctor`: `READY_LOCAL_ONLY` sin juego seleccionado.
- `mgpu-auto selftest`: `passed=true`.
- Build VKD3D experimental: correcto, con ambos parches detectados/aplicados de forma reproducible.
- Prueba FD VKD3D→CUDA: importación y mapeo correctos después de corregir herencia del FD; P2P y checksum correctos hacia GPU1.
- Diagnóstico interno VKD3D: confirmado `65.536 bytes / memory type 1 / export 0 / properties -13`; el `properties=-13` no impide la importación CUDA en esta ruta.
- vLLM: contenedor `vllm-qwen38-27b-dual-fast` detenido; VRAM posterior aproximada 857/66 MiB usados.
- Monitores: sólo `DP-0` y `HDMI-1-0` conectados en la última lectura; no se efectuaron cambios de configuración.

## Próximas mejoras recomendadas

- Añadir trazas VKD3D de tipo de memoria, tamaño real, extensiones habilitadas y cadena `pNext` por asignación.
- Crear una asignación dedicada de buffer Vulkan explícitamente exportable/importable y comprobarla dentro del mismo proceso Unix antes de conectarla a un recurso D3D12 real de un juego.
- Resolver la enumeración/identidad física de GPU1 bajo VKD3D; el índice experimental actual sólo garantiza un device Vulkan distinto, no una GPU distinta.
- Exponer una SPI experimental de memoria/sincronización desde VKD3D, en vez de inferir el contrato a partir de `VkDeviceMemory` privado.
- Implementar semáforos/fences externos y medición de latencia end-to-end.
- Capturar primero una evaluación DLSS estándar real en GPU A; recién después intentar mover un pass neuronal a GPU B.
- Mantener `READY_REMOTE` cerrado hasta que memoria, sincronización, NGX y validación visual pasen sus gates.

## 2026-09-10 — hook de transporte desde la evaluación NGX

- Se recuperó el fuente actual de `dlss5-linux-bridge` y se añadió el patch reproducible `patches/dlss5-linux-bridge-transport-probe.patch`.
- El bridge ahora tiene un modo opt-in `MGPU_DLSSNR_TRANSPORT=probe` que consulta la interfaz VKD3D-Proton desde el mismo proceso y registra `VkInstance`, `VkPhysicalDevice`, `VkDevice`, handles de recursos, offsets y layouts.
- Se añadió `scripts/build_bridge_transport_probe.sh`; trabaja sobre una copia temporal, verifica el patch y no modifica el checkout del bridge del usuario.
- `scripts/run_ngx_test.sh` acepta `NGX_BRIDGE_DIR`, permitiendo probar DLLs alternativas sin sobrescribir el build base.
- La ejecución real del hook observó `color`, `output`, `motion` y `depth` con `GetVulkanResourceInfo1` y HRESULT exitoso.
- El hook es deliberadamente sólo diagnóstico: no exporta memoria, no llama CUDA y no declara `READY_REMOTE`.
- Se repitió la evaluación sintética con el contrato DLSS extendido; el runtime comunitario continúa devolviendo `0xbad00005`, por lo que sigue faltando un host auténtico y una evaluación válida.

## 2026-09-10 — SPI de heap y transporte automático desde el bridge

- Se añadió `patches/vkd3d-export-heap-fd-spi.patch`, con la interfaz experimental `ID3D12DXVKInteropDevice4::ExportVulkanHeapFd`.
- La SPI valida que el heap pertenezca al device, exporta `VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT` y queda protegida por `VKD3D_EXPORT_HEAP_FD=1`.
- Se corrigió el primer build: `fcntl.h` y `FD_CLOEXEC` no forman parte del entorno MinGW del DLL PE. La herencia POSIX queda encapsulada en `fd_inherit_shim.c`.
- `EnsurePrivateOutput` del bridge ahora conserva el `ID3D12Heap` y crea el output mediante `CreateHeap` + `CreatePlacedResource`.
- Se añadió `patches/dlss5-linux-bridge-fd-probe.patch` y el modo opt-in `MGPU_DLSSNR_TRANSPORT=fd-probe`.
- `scripts/build_bridge_transport_probe.sh` aplica ambos parches del bridge sobre una copia temporal limpia.
- `scripts/run_ngx_test.sh` activa automáticamente la memoria exportable y el shim sólo para `fd-probe`.
- Build completo reproducible de VKD3D: los cuatro parches se aplican/detectan y `d3d12.dll`/`d3d12core.dll` se instalan correctamente.
- Smoke de la SPI: heap de 65.536 bytes exportado; el helper ve un FD NVIDIA, importa/mapea, escribe, ejecuta `cuMemcpyPeer` y valida checksum en GPU1.
- Smoke automático del bridge: output 1280x720, heap de 7.864.320 bytes, export exitoso y helper con `spawn_rc=0`.
- El runtime NGX de la prueba sigue devolviendo `0xbad00005`; el resultado no prueba NR remoto ni MFG remoto.
- `vkGetMemoryFdPropertiesKHR` continúa en `-13` (`VK_ERROR_UNKNOWN`) bajo Wine; CUDA acepta el FD en la ruta heredada, pero el contrato Vulkan estándar sigue pendiente.
