# Plan completo de implementación — Dual RTX 3090 / DLSS5 en Linux

## Auditoría de avance — 2026-09-10

- [x] Corregir el contrato del smoke sintético: `EvaluateFeature` positivo pasó a `0x00000001` después de normalizar dimensiones, jitter, motion-vector scales, subrects, exposición y reset.
- [x] Hacer reproducible el stack de parches del bridge sobre checkout limpio.
- [x] Deduplicar physical devices Vulkan por UUID/PCI y dar prioridad a la selección A/B sobre `VKD3D_VULKAN_DEVICE` en modo opt-in.
- [x] Validar identidad física distinta en el mismo proceso: A `0:1:0.0`, B `0:3:0.0`.
- [x] Validar en procesos Proton aislados que NGX/NR local funciona en A y B con la identidad UUID/PCI esperada.
- [x] Añadir SPI `ID3D12DXVKInteropDevice5` para identidad y exportación de fence FD.
- [x] Añadir probe automático de fence desde la evaluación NGX.
- [ ] Obtener exportación/importación de semáforos externos funcional en este host; el probe devuelve `E_NOTIMPL`.
- [ ] Asociar un fence a la finalización real de la cola del juego y a la cola consumidora de B.
- [ ] Importar color, motion vectors y depth en recursos del device B; el FD del heap por sí solo no es una imagen cross-adapter completa.
- [x] Validar en laboratorio la importación del heap del output privado como `VkImage` en B y acceso GPU real mediante clear/copy/readback en `GPU0 → GPU1`.
- [x] Hacer que el smoke NGX cierre/envíe el command list y espere una fence D3D12 desde CPU antes del readback; positivo y negativo completan la cola, pero comparten la misma firma de salida.
- [ ] Hacer pasar la misma importación física en `GPU1 → GPU0`; con el selector experimental correcto el driver devuelve `VK_ERROR_OUT_OF_DEVICE_MEMORY`.
- [ ] Crear y evaluar el feature NGX sobre un command list del device B con esas imágenes importadas.
- [ ] Confirmar que el output B vuelve a la cadena de presentación sin retorno innecesario a A.
- [ ] Validar estabilidad, latencia y contenido visual en un host/juego D3D12 real.
- [x] Ejecutar el sample oficial Windows D3D12 en una copia Proton instrumentada; crea el device, pero no carga NGX ni produce log del bridge dentro de 45 s.
- [ ] Mantener cerrado `READY_REMOTE` hasta completar todos los gates anteriores.
- [ ] MFG remoto permanece explícitamente fuera de alcance.

Este documento es la fuente única de verdad del experimento. Separa tres objetivos que suelen confundirse:

1. validar que las dos RTX 3090 pueden intercambiar datos eficientemente;
2. ejecutar una aplicación D3D12/DLSS bajo Linux/Proton;
3. ejecutar Neural Rendering o Frame Generation en una segunda GPU.

El primero está validado. El segundo está parcialmente validado. El tercero todavía no está desbloqueado.

## Arquitectura objetivo

```text
Juego Windows / D3D12
        │
        ▼
Proton + VKD3D-Proton
        │
        ├── RTX 3090 A: raster, RT, DLSS Super Resolution
        │                 motion vectors, depth, color
        │
        ├── recurso exportable / cross-adapter
        │                 │
        │                 ▼
        └──────────► RTX 3090 B: Neural Rendering
                              optical flow / composición
                              presentación
```

La primera versión no intenta dividir el render ni usar SLI/AFR. Tampoco activa Frame Generation. El objetivo mínimo es conservar el DLSS existente del juego y descargar únicamente un pass neuronal comprobable.

## Estado resumido

| Área | Estado | Evidencia |
|---|---|---|
| Hardware, PCIe y UUID | ✅ completo | dos RTX 3090, SM86, driver 595.71.05 |
| CUDA P2P | ✅ completo | validación bidireccional; ~10,3/12,4 GB/s |
| Sincronización CUDA nativa | ✅ completa para transporte CUDA | `cudaStreamWaitEvent` A↔B, 120/120 frames; no sustituye fence D3D12/Vulkan |
| Vulkan→CUDA→P2P | ✅ completo | ambas direcciones, checksum correcto |
| Ring de transporte | ✅ completo | 100 frames, 0 errores de validación |
| Selector automático | ✅ completo | `mgpu-auto doctor`, `selftest`, `plan`; bloquea remoto si no hay transporte |
| Lanzador directo | ✅ completo | Wine, selección de GPU, watchdog y fallback |
| Sample oficial DLSS Linux | ✅ completo | NGX/DLSS estándar inicializa |
| Bridge NGX | ✅ compilado/carga | DLLs x64 y loader smoke bajo Wine |
| NGX D3D12 real bajo Wine | ✅ validado en GE-Proton | VKD3D-Proton enumera dos RTX 3090 y crea ambos dispositivos |
| NGX en dos objetos D3D12 simultáneos | ✅ validado hasta CreateFeature | ambos objetos inicializan NGX y crean un feature; la sonda muestra que comparten el mismo device Vulkan |
| D3D12 cross-adapter nativo | ⛔ bloqueado por VKD3D | heaps/recursos se crean, pero `CreateSharedHandle(heap)=E_NOTIMPL` y el fallback de recurso es `DXGI_ERROR_INVALID_CALL` |
| Dos adapters Vulkan en un proceso Proton | ⛔ bloqueado por selección global | `VKD3D_VULKAN_DEVICE=0/1` es por proceso; la sonda devuelve `multi_adapter_distinct=no` |
| Extracción de recurso D3D12→Vulkan | 🟡 parcial | GE-Proton expone `VkBuffer` y `VkDeviceMemory`; el buffer CUDA pasa en ambas direcciones, pero la imagen física inversa devuelve `VK_ERROR_OUT_OF_DEVICE_MEMORY` |
| VKD3D experimental con LUID duplicado | 🟡 laboratorio | abre handles independientes, pero este host duplica UUID/PCI; no prueba todavía dos GPUs físicas |
| FD D3D12/Vulkan→CUDA bajo Proton | ✅ transporte MVP | FD heredado sin `CLOEXEC`, import/map/write/`cuMemcpyPeer`/checksum correctos; `vkGetMemoryFdPropertiesKHR` sigue en `VK_ERROR_UNKNOWN` |
| SPI VKD3D para exportar heap D3D12 | ✅ opt-in | `ID3D12DXVKInteropDevice4::ExportVulkanHeapFd`; heap real de 64 KiB exportado e importado por CUDA |
| Bridge `fd-probe` automático | ✅ transporte validado | output colocado de 1280x720 exportado; helper valida P2P hacia GPU1 con wrapper y shim acotado |
| Bypass lineal de imagen con CUDA P2P | ✅ laboratorio | asignaciones de imagen Vulkan equivalentes, copia GPU→GPU y readback correcto en ambas direcciones |
| Textura D3D12 → buffer lineal | ✅ laboratorio | `CopyTextureRegion` con footprint real, fence CPU y pixel readback correcto en ambas orientaciones |
| Ejecución/readback del command list NGX | ✅ smoke host | cola D3D12 + fence CPU + readback completan; hash idéntico positivo/negativo, sin evidencia visual de NR |
| NGX sobre dos devices Vulkan distintos | ⛔ estado global del runtime | ambos `Init_Ext` pasan, pero sólo el device inicializado primero crea el feature |
| Neural Rendering en GPU A | ✅ validado hasta EvaluateFeature sintético | con runtime DLSS limpio: `Init_Ext=0x1`, `CreateFeature=0x1`, `EvaluateFeature=0x1`; todavía no es un juego real |
| Neural Rendering local en GPU B aislada | ✅ smoke sintético | proceso Proton separado, UUID/PCI `0:3:0.0`, `EvaluateFeature=0x1` y chaining DLSSNR `0x1`; no es NR remoto |
| Neural Rendering remoto en GPU B | ⛔ no implementado | bridge actual encadena en el device del juego; no crea segundo device |
| Juego real con DLSS5/MFG | ⛔ no iniciado | no hay host Linux/Proton válido todavía |
| Host oficial D3D12 instrumentado | 🟡 arranque parcial | crea el device VKD3D, pero queda antes de cargar NGX; watchdog 45 s |
| Frame Generation remoto | ⏸ pospuesto | requiere NR estable y sincronización temporal |

## TODO con estado de ejecución

### Fase 0 — Alcance y seguridad

- [x] Definir `RTX 3090 A = render` y `RTX 3090 B = neural/presentación`.
- [x] Evitar SLI/AFR como estrategia principal.
- [x] Mantener DLLs, modelos, pesos y CUBINs propietarios fuera del repositorio.
- [x] Usar prefijos temporales de Wine para las pruebas.
- [x] Mantener fallback local si falla cualquier gate.
- [x] No probar inicialmente con anti-cheat.

### Fase 1 — Inventario del sistema

- [x] Enumerar las dos RTX 3090 y sus PCI BDF:
  - GPU0: `00000000:01:00.0`
  - GPU1: `00000000:03:00.0`
- [x] Confirmar arquitectura SM86.
- [x] Registrar driver NVIDIA `595.71.05`.
- [x] Registrar topología: enlace GPU↔GPU por `PHB`, mismo NUMA.
- [x] Detectar GPU con salida activa y reservar la otra como render cuando corresponde.
- [x] Evitar asumir que índice Vulkan y CUDA coinciden; resolver por UUID/PCI.
- [x] Verificar que vLLM no siga ocupando VRAM: contenedor detenido y sin proceso `vllm serve` activo.

### Fase 2 — Transporte CUDA P2P

- [x] Implementar enumeración CUDA.
- [x] Consultar `cudaDeviceCanAccessPeer` en ambas direcciones.
- [x] Activar peer access cuando el driver lo permite.
- [x] Implementar copia GPU→GPU con `cudaMemcpyPeerAsync`.
- [x] Validar el buffer en la GPU destino mediante kernel CUDA.
- [x] Medir ida y vuelta.
- [x] Implementar ring asíncrono con slots, eventos y política de finalización.
- [x] Ejecutar ring de 100 frames: `validation_failures=0`.
- [x] Mantener explícita la diferencia entre P2P real y host staging.
- [x] Validar waits GPU→GPU con eventos CUDA en ambas dependencias del ring, dejando el CPU sólo para retiro/timeout.

Comandos principales:

```bash
./build/mgpu-p2p-probe --json
./build/mgpu-p2p-ring-probe --bytes 8294400 --slots 3 --frames 300
```

### Fase 3 — Interoperabilidad Vulkan/CUDA

- [x] Enumerar dispositivos Vulkan.
- [x] Deduplicar los dispositivos duplicados expuestos por el driver.
- [x] Leer UUID y PCI de Vulkan.
- [x] Exportar una asignación `DEVICE_LOCAL` mediante opaque fd.
- [x] Importar esa memoria desde CUDA.
- [x] Copiar desde CUDA hacia la segunda GPU por P2P.
- [x] Validar el resultado con checksum/kernel.
- [x] Probar Vulkan GPU0→CUDA GPU0→GPU1.
- [x] Probar Vulkan GPU1→CUDA GPU1→GPU0.

Comando:

```bash
./scripts/run_interop.sh
```

Resultado observado: ambas direcciones pasan; el throughput observado varía aproximadamente entre 4,4 y 6,0 GB/s según la carga y la ejecución.

### Fase 4 — MVP automático

- [x] Implementar `mgpu-auto doctor --json`.
- [x] Implementar `mgpu-auto selftest --json`.
- [x] Implementar selección automática render/neural.
- [x] Implementar estados `READY_LOCAL_ONLY`, `READY_REMOTE`, `P2P_UNAVAILABLE` y fallback.
- [x] Corregir el caso sin juego seleccionado para que no informe falsamente `READY_REMOTE`.
- [x] Detectar memoria libre y procesos ocupantes.
- [x] Descubrir juegos Steam/VDF cuando están disponibles.
- [x] Generar perfil TOML aislado por juego.
- [x] Mantener la configuración fuera del repositorio.
- [x] Añadir tests Python: 13/13 pasan.

Comandos:

```bash
./scripts/mgpu-auto doctor --json
./scripts/mgpu-auto selftest --json
./scripts/mgpu-auto plan --json
./scripts/mgpu-auto games
```

### Fase 5 — Lanzador seguro de ejecutables Windows

- [x] Añadir `mgpu-auto run --exe` para ejecutar un `.exe` aislado.
- [x] Fijar `VKD3D_VULKAN_DEVICE` para la GPU de render.
- [x] Fijar `VKD3D_FILTER_DEVICE_NAME` y `DXVK_FILTER_DEVICE_NAME`.
- [x] Usar `WINEPREFIX` explícito.
- [x] Añadir watchdog con terminación ordenada y timeout.
- [x] Declarar explícitamente `DLSS5_MGPU_MODE=local-fallback` mientras no exista bridge NGX funcional.
- [x] Verificar que el demo D3D12 libre usa la GPU seleccionada.

Demo utilizado:

```text
/home/cristian/Juegos/DLSS5-3DRenderer/3DRenderer-0.3.1-win64/3DRenderer.exe
```

Este demo valida selección D3D12/VKD3D, pero no es un host DLSS y no prueba Neural Rendering.

### Fase 6 — Dependencias de ejecución

- [x] Instalar Wine 9.0 de 64/32 bits.
- [x] Instalar VKD3D y `winetricks`.
- [x] Crear prefix aislado `/tmp/dlss5-wine64-final`.
- [x] Instalar Visual C++ Runtime en ese prefix.
- [x] Instalar `vulkan-tools` para inspección de dispositivos.
- [x] Confirmar que Vulkan ve las dos RTX 3090 con UUID distintos.
- [x] No modificar el prefix del usuario ni la configuración global de juegos.

### Fase 7 — Artefactos oficiales DLSS

- [x] Clonar el SDK público [NVIDIA/DLSS](https://github.com/NVIDIA/DLSS).
- [x] Descargar el release oficial v310.9.1 Linux.
- [x] Descargar el release oficial v310.9.1 Windows.
- [x] Ejecutar el sample Linux oficial.
- [x] Confirmar `GetFeatureRequirements returned 0x00000000`.
- [x] Confirmar requisitos Vulkan NGX y driver mínimo 470.0.
- [x] Clonar [DLSS5-Swapper](https://github.com/rakanki911/DLSS5-Swapper) para inspección.
- [x] Clonar [dlssg_for_sm86](https://github.com/sdli1995/dlssg_for_sm86) para inspección.
- [x] Confirmar que `dlssg_for_sm86` trae una `version.dll` Windows con backend SM86.
- [ ] Obtener un runtime oficial/legalmente autorizado de Neural Rendering para Ampere.
- [x] Obtener para laboratorio un runtime comunitario 310.8.0 y mantenerlo fuera del repositorio.
- [x] Añadir una guardia que impide usar el propio proxy como `nvngx_dlss_real.dll` y evita recursión de `Init_Ext`.
- [ ] Obtener un host que invoque efectivamente Neural Rendering/MFG bajo Linux/Proton.

Ubicación local de las descargas:

```text
/home/cristian/Juegos/DLSS5-NVIDIA-DLSS-310.9.1
/home/cristian/Juegos/DLSS5-sources
```

### Fase 8 — Bridge NGX

- [x] Clonar [dlss5-linux-bridge](https://github.com/ccoredesenvolvimento/dlss5-linux-bridge).
- [x] Compilar `bridge-nvngx.dll` con MinGW-w64.
- [x] Compilar `_nvngx.dll` con MinGW-w64.
- [x] Guardar los artefactos no propietarios en `build/proton/`.
- [x] Confirmar exports mediante `objdump`.
- [x] Crear `tests/ngx_loader_smoke.c`.
- [x] Confirmar bajo Wine que cargan `nvngx_dlss.dll` proxy y `bridge-nvngx.dll`.
- [x] Confirmar exports `Init`, `Create`, `Evaluate`, `GetFeatureRequirements` y bridge DLSS/NR.
- [x] Probar `NVSDK_NGX_D3D12_Init_Ext` contra un dispositivo D3D12 hardware real bajo GE-Proton/VKD3D-Proton.
- [x] Probar chaining hasta `CreateFeature`: DLSS estándar y NR inicializan/crean handles.
- [x] Añadir hook opt-in `MGPU_DLSSNR_TRANSPORT=probe` en el bridge para inspeccionar recursos D3D12/VKD3D desde la ruta de evaluación.
- [ ] Completar chaining con `EvaluateFeature` usando recursos y estados equivalentes a un juego.

Build reproducible:

```bash
NGX_SDK_DIR=/ruta/al/DLSS \
DLSS5_BRIDGE_SOURCE=/ruta/a/dlss5-linux-bridge \
./scripts/build_bridge.sh
```

### Fase 9 — Prueba NGX reproducible

- [x] Crear `tests/ngx_d3d12_smoke.cpp`.
- [x] Crear `scripts/run_ngx_test.sh`.
- [x] Ejecutar el sample oficial Linux hasta el watchdog; el proceso queda renderizando.
- [x] Ejecutar loader smoke del proxy bajo Wine.
- [x] Ejecutar smoke D3D12 bajo Wine.
- [x] Registrar el resultado negativo de Wine 9.0: no crea dispositivo hardware NGX válido.
- [x] Conseguir y probar GE-Proton 11-6/VKD3D-Proton con las dos RTX 3090 visibles.
- [x] Repetir `Init_Ext`, `GetFeatureRequirements` y `CreateFeature` con la cadena proxy/core/DLSS real.
- [x] Probar el runtime NR directamente y el identificador `Reserved18` en un proceso aislado: el DLL directo devuelve `0xbad00002` y el proxy devuelve `0xbad0000c`.
- [x] Completar `EvaluateFeature` sintético con recursos y parámetros normalizados; el smoke devuelve `0x00000001` con runtime limpio.
- [ ] Completar `EvaluateFeature` con recursos auténticos de un host/juego.
- [x] Repetir el smoke positivo con runtime DLSS limpio después de detectar contaminación del bundle: `CreateFeature=0x1`, `EvaluateFeature=0x1`, retorno `0`.

Comando:

```bash
DLSS_DEMO_DIR=/ruta/a/DLSS_Sample_App/bin/ngx_dlss_demo \
NGX_SDK_DIR=/ruta/al/DLSS \
WINEPREFIX=/tmp/dlss5-wine64-final \
./scripts/run_ngx_test.sh
```

### Fase 10 — Primer host de juego

- [x] Descargar un demo D3D12 pequeño sin login/Steam.
- [x] Ejecutarlo con Wine/VKD3D.
- [x] Verificar selección individual de GPU.
- [x] Usar el sample oficial DLSS como host D3D12 y confirmar creación de la ventana/dispositivo bajo GE-Proton.
- [x] Ejecutar el sample oficial D3D12 bajo GE-Proton con el empaquetado experimental; el proceso arranca, pero no emitió trazas NGX del bridge.
- [x] Ejecutar el hook del bridge con el host sintético y obtener `VkInstance/VkPhysicalDevice/VkDevice` más cuatro recursos mediante `GetVulkanResourceInfo1`.
- [ ] Confirmar en un host de juego/sample que el DLL proxy se carga desde la ruta de upscalers de Proton y que se ejecutan evaluaciones reales.
- [ ] Capturar color, motion vectors y depth sin devolverlos por RAM.
- [ ] Asociar cada frame con `frame_id` y timestamps.
- [ ] Validar estados y formatos antes de enviarlos a GPU B.
- [ ] Mantener salida local si la captura no es compatible.

### Fase 11 — Neural Rendering remoto

- [x] Crear un segundo `ID3D12Device` y validar `Init_Ext`/`CreateFeature` en GPU B dentro del mismo proceso.
- [x] Consultar la interfaz `ID3D12DeviceExt` y registrar `VkInstance`/`VkPhysicalDevice`/`VkDevice` de ambos objetos.
- [x] Confirmar que los dos objetos no son dos adapters Vulkan distintos bajo GE-Proton actual.
- [x] Compilar un VKD3D-Proton aislado con selección experimental para LUID duplicados.
- [x] Obtener dos `VkPhysicalDevice`/`VkDevice` distintos en un mismo proceso con `VKD3D_DUPLICATE_LUID_ADAPTERS=1`.
- [x] Obtener `VkDeviceMemory` real mediante `GetVulkanHeapInfo` en el build instalado y en el build experimental.
- [x] Probar el orden de inicialización NGX: el primer device crea el feature; el segundo devuelve `FAIL_NotInitialized`.
- [x] Verificar el camino alternativo de aislamiento: un proceso Proton dedicado a cada índice físico ejecuta NGX/NR local correctamente en A y B.
- [ ] Conectar ese device a una evaluación NR real; la prueba actual sólo crea un feature sintético.
- [x] Conectar de forma no invasiva la entrada de `EvaluateFeature` al hook de transporte y registrar handles, offsets y layouts.
- [ ] Exportar/importar ese `VkDeviceMemory` entre los dos devices y añadir sincronización de fences/semaphores.
- [x] Exponer una SPI VKD3D opt-in para exportar el heap real: `ID3D12DXVKInteropDevice4::ExportVulkanHeapFd`.
- [x] Retener el heap del output privado en el bridge y añadir `MGPU_DLSSNR_TRANSPORT=fd-probe`.
- [x] Automatizar `VKD3D_EXPORT_OPAQUE_FD_MEMORY=1`, `VKD3D_EXPORT_HEAP_FD=1` y la herencia FD sólo en el wrapper de prueba.
- [x] Validar `heap D3D12 → FD Vulkan → helper CUDA → cuMemcpyPeer → checksum` desde el hook de evaluación.
- [x] Validar un ring CUDA-native con `cudaStreamWaitEvent` productor/consumidor: 120/120 frames y checksum correcto.
- [x] Validar representación de imagen cross-device en `GPU0 → GPU1`: heap D3D12 A → FD → `VkImage` B → clear/copy/readback, todo con `VK_SUCCESS`.
- [ ] Validar la orientación física `GPU1 → GPU0`; el buffer CUDA pasa, pero la importación como `VkImage` devuelve `VK_ERROR_OUT_OF_DEVICE_MEMORY`.
- [x] Implementar bypass lineal de asignación de imagen equivalente mediante CUDA P2P y readback Vulkan; `GPU0↔GPU1` pasa.
- [ ] Implementar recursos cross-adapter D3D12 o una ruta Vulkan/CUDA equivalente dentro del proceso.
- [x] Validar el bypass de asignación de imagen equivalente por CUDA P2P, sin staging de RAM.
- [x] Validar textura D3D12 → buffer lineal → FD → CUDA/P2P → readback con una fence CPU acotada.
- [x] Automatizar la matriz D3D12 lineal en ambos sentidos y exigir exportación FD, importación CUDA y readback correcto.
- [x] Enviar y esperar el command list del smoke NGX con una fence CPU; convertir timeout/fallo de espera en error y leer el output sólo después de la finalización.
- [x] Medir bytes no nulos y FNV-1a del output NGX; registrar que la firma coincide con el host negativo y no permite atribuirla a NR.
- [ ] Aislar/adaptar el estado global NGX para que A y B puedan evaluar features simultáneamente.
- [ ] Sustituir el aislamiento por proceso por dos contextos cooperantes dentro de la cadena del juego, sin copiar recursos por RAM.
- [ ] Evitar el viaje GPU A→CPU→GPU B.
- [ ] Ejecutar NR en GPU B con runtime compatible.
- [ ] Mantener el monitor de salida conectado a GPU B si el frame final no vuelve a A.
- [ ] Medir latencia de transferencia, inferencia y presentación.
- [ ] Comparar GPU B ocupación/VRAM contra modo local.
- [ ] Implementar device-loss y fallback local durante el arranque.
- [ ] Validar una sesión continua de 30 minutos.

Nota de estado: `fd-probe` ya confirma en laboratorio la importación del heap privado como una `VkImage` utilizable por B, incluido acceso GPU y readback, en la orientación `GPU0 → GPU1`. En la orientación física inversa el buffer CUDA es importable, pero la imagen Vulkan falla con `VK_ERROR_OUT_OF_DEVICE_MEMORY`. El FD sale con `CLOEXEC`; el wrapper utiliza el shim POSIX sólo para el proceso de prueba. Para producción aún falta resolver esa asimetría, conectar las imágenes auténticas color/motion/depth del juego, sincronizarlas con su cola, ejecutar NGX/NR sobre el device B y devolver/presentar el resultado.

### Fase 12 — Frame Generation SM86

- [ ] Integrar `dlssg_for_sm86` como etapa independiente.
- [ ] Verificar que el juego realmente usa el entry point de DLSSG.
- [ ] Ejecutar primero 2X y sólo después investigar 3X/4X.
- [ ] Medir frametime real, latencia y artefactos; no usar FPS interpolados como único criterio.
- [ ] No combinarlo con NR remoto hasta que la etapa anterior sea estable.
- [ ] Agregar una política de desactivación por juego.

## Stoppers encontrados

### S1 — Runtime NR oficial y estatus de distribución

El bridge necesita tres piezas compatibles: `_nvngx_real.dll`, `nvngx_dlss_real.dll` y `nvngx_dlssnr.dll`. El release público de NVIDIA utilizado contiene DLSS estándar (`nvngx_dlss.dll`/`libnvidia-ngx-dlss.so`), pero no publicó `nvngx_dlssnr.dll`. Para laboratorio se probó un runtime comunitario 310.8.0 distribuido en un paquete de terceros; no se lo debe presentar como binario oficial ni instalarlo automáticamente en juegos.

**Impacto:** la ruta experimental ya demuestra `Init_Ext` y `CreateFeature` en una RTX 3090, pero no hay garantía de distribución, soporte oficial, calidad visual ni estabilidad.

**Cómo se desbloquea:** runtime legal compatible, o una implementación abierta/vendor-neutral del pass neuronal.

### S2 — Host D3D12 de Wine del sistema no expone una RTX válida

El smoke creado con `D3D12CreateDevice` enumera `NVIDIA GeForce GTX 470` y devuelve `0x80070057` para Feature Level 12.0. El demo D3D12 puede abrir ventana/renderizar, pero esa prueba no alcanza un dispositivo hardware apto para NGX.

**Impacto:** el runner Wine del sistema no sirve para NGX; esto queda resuelto para el experimento mediante GE-Proton/VKD3D-Proton.

**Cómo se desbloqueó:** GE-Proton 11-6 con VKD3D-Proton enumera dos RTX 3090 y permite crear ambos `ID3D12Device`.

### S2b — Evaluación sintética devuelve parámetros inválidos

El smoke puede inicializar DLSS/NR y crear ambos features. Una corrida anterior devolvía `0xbad00005` (`FAIL_InvalidParameter`) porque el bundle había terminado usando el proxy como runtime real; con el guardia de `DLSS_RUNTIME_DLL` y el DLL limpio del SDK, la misma evaluación sintética devuelve `0x00000001`. Esto no reemplaza todavía recursos auténticos de un host ni valida calidad visual.

**Impacto:** todavía no hay prueba de que un frame real atraviese DLSS estándar y Neural Rendering, aunque la creación del runtime sí está validada.

**Cómo se desbloquea:** corregir el launcher/ruta `system32/umu`, usar recursos creados por un host DLSS real, validar formatos/estados y capturar el resultado visual antes de activar cualquier segundo GPU.

### S3 — El bridge no implementa multi-GPU por sí solo

El bridge de referencia encadena llamadas NGX y carga DLLs. No crea automáticamente un segundo adapter D3D12, no resuelve heaps cross-adapter y no selecciona GPU B para la inferencia.

**Impacto:** compilar y cargar el bridge no equivale a tener Neural Rendering remoto.

**Cómo se desbloquea:** extenderlo con selección de adapter, recursos compartidos, sincronización y presentación en GPU B.

### S4 — El demo D3D12 descargado no valida DLSS

El demo `3DRenderer` sólo sirve para probar VKD3D y selección de GPU. No llama NGX, DLSS, DLSSG ni Neural Rendering.

**Impacto:** no puede usarse como benchmark DLSS5.

**Cómo se desbloquea:** usar el sample oficial DLSS o un juego D3D12/Proton que invoque la API necesaria.

La ejecución instrumentada del sample oficial Windows con bridge y VKD3D
experimental llegó a crear el device seleccionado, pero con `WINEDEBUG=+loaddll`
no apareció ninguna carga de `nvngx_dlss.dll` ni `dlssnr-proxy.log` durante el
watchdog de 45 s. El nuevo probe deja este estado reproducible; no se concluye
si el bloqueo está en la carga de escena, el loop inicial de la aplicación o la
resolución de DLLs. Sigue pendiente obtener una evaluación real observable.

### S5 — No hay juego Steam/Proton instalado en el entorno

El descubridor automático encontró cero juegos Steam en el momento de la prueba. Se evitó descargar un título comercial que requiriera cuenta o login.

**Impacto:** no hubo prueba end-to-end sobre un juego comercial.

**Cómo se desbloquea:** proporcionar una instalación local de Proton y un juego D3D12 sin anti-cheat para pruebas.

### S6 — Incidente histórico de salida de monitor

Durante una revisión anterior, Xorg reportó dos salidas conectadas y `DP-1-3` desconectada. El trabajo realizado no ejecutó cambios de RandR ni reinició Xorg; las consultas fueron de sólo lectura. En el chequeo más reciente de esta sesión siguen apareciendo dos salidas conectadas (`DP-0` y `HDMI-1-0`) y `DP-1-3` desconectada. El incidente queda separado del plan de DLSS y no se atribuye a estos cambios.

### S7 — API pública insuficiente para NR independiente

La sonda directa confirmó que `nvngx_dlssnr.dll` exporta `Init/Create/Evaluate`, pero su inicialización independiente devuelve `0xbad00002` en este host. Si se inicializa primero el proxy, el proxy crea NR internamente, pero su `CreateFeature(Reserved18)` devuelve `0xbad0000c`. Por lo tanto, el runtime comunitario no puede tratarse como una feature pública autónoma desde este bridge.

**Impacto:** la única ruta demostrada es el chaining interno `DLSS estándar → NR` hasta `CreateFeature`; no existe todavía una llamada de evaluación NR independiente que permita transportar el pass a GPU B.

**Cómo se desbloquea:** obtener el contrato exacto del host/bridge que invoca NR, o implementar el adaptador a partir de una integración real que entregue los parámetros internos esperados. Después habrá que mover esa evaluación a un segundo device.

### S9 — Recursión por runtime DLSS reemplazado

Una prueba del launcher usó como `nvngx_dlss_real.dll` un archivo que era el propio proxy. La traza mostraba cientos de `real core Init_Ext` en el mismo milisegundo y el proceso terminaba sólo por watchdog. Se validó que no es un fallo del transporte: reemplazando el origen por el DLL limpio del SDK, el mismo host completó `CreateFeature`, `EvaluateFeature` y el helper FD.

**Estado:** resuelto en el launcher con `DLSS_RUNTIME_DLL` y detección de las firmas `_nvngx_real.dll`/`bridge-nvngx.dll`. El origen del runtime sigue siendo responsabilidad del usuario y el bridge no incorpora binarios propietarios.

### S8 — VKD3D no exporta handles cross-adapter D3D12

La prueba `tests/d3d12_cross_adapter_smoke.cpp` crea dos devices, un heap con `D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER` y un recurso con `D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER`. VKD3D-Proton acepta la creación local, pero `CreateSharedHandle(heap)` devuelve `0x80004001` (`E_NOTIMPL`) y `CreateSharedHandle(resource)` devuelve `0x887a0001` (`DXGI_ERROR_INVALID_CALL`).

La inspección del código fuente de VKD3D-Proton confirma el motivo: `libs/vkd3d/device.c` deja `CreateSharedHandle` implementado sólo bajo la rama nativa Win32 y devuelve `E_NOTIMPL` en la rama Unix; `libs/vkd3d/heap.c` condiciona `SHARED_CROSS_ADAPTER` a `VK_EXT_external_memory_host`; y `libs/vkd3d/resource.c` documenta que `ALLOW_CROSS_ADAPTER` usa memoria externa host. El build experimental sí permite abrir el device B y consultar/exportar un FD, pero ese FD no pasa la consulta de propiedades ni la importación CUDA. No alcanza con cambiar el flag D3D12.

**Impacto:** no se puede usar, por ahora, la interfaz D3D12 estándar para entregar directamente el recurso de GPU A a GPU B bajo este host.

**Cómo se desbloquea:** implementar la ruta dentro de VKD3D/interop Vulkan usando memoria externa y CUDA P2P, o encontrar una versión de VKD3D con soporte cross-adapter suficiente. No conviene simularlo con una copia por CPU.

La prueba adicional de `tests/vkd3d_interop_probe.cpp` encontró que el GE-Proton instalado expone `ID3D12DXVKInteropDevice3`: entrega el `VkBuffer` de un recurso D3D12 y un `VkDeviceMemory` real para un heap. La variante `VKD3D_EXPORT_OPAQUE_FD_MEMORY=1` también obtiene un FD, pero la consulta estándar `vkGetMemoryFdPropertiesKHR` retorna `VK_ERROR_UNKNOWN`; un helper Linux que hereda el FD recibe `CUDA_ERROR_UNKNOWN` al importarlo en cualquiera de las dos 3090. El descriptor se transmite bien: el bloqueo está en la compatibilidad de la asignación/handle, no en P2P ni en el proceso de Proton.

**Cómo se desbloquea:** inspeccionar la asignación real de VKD3D (tipo de memoria, extensión habilitada y cadena `pNext`) y crear una asignación dedicada explícitamente compatible con `VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT`; después repetir importación CUDA en A y B y agregar semáforos/fences externos. No se debe declarar `READY_REMOTE` mientras este gate falle.

### S9 — Selección Vulkan global dentro de VKD3D-Proton

La sonda `tests/vkd3d_interop_probe.cpp` consulta `ID3D12DeviceExt::GetVulkanHandles()` después de crear dos objetos D3D12 a partir de dos adaptadores DXGI. En GE-Proton 11-6, ambos devuelven el mismo `VkPhysicalDevice` y `VkDevice`. Repetir con `VKD3D_VULKAN_DEVICE=0` y `=1` sólo cambia cuál GPU usa todo el proceso.

**Impacto:** no alcanza con pedir un segundo `ID3D12Device` al bridge; no hay todavía un device Vulkan B al cual enviar el pass NR. La topología “D3D12 A + D3D12 B” queda reducida a dos objetos sobre la misma GPU.

**Cómo se desbloquea:** extender VKD3D-Proton para permitir selección por adapter por dispositivo D3D12, o sacar el transporte fuera de la abstracción D3D12 usando un contexto Vulkan/CUDA B independiente y un contrato explícito de memoria/sincronización. La segunda alternativa requiere además que el host/bridge pueda entregar el frame al contexto B.

### S10 — Estado global de NGX/proxy con devices distintos

Con el VKD3D experimental, A y B ya tienen `VkDevice` distintos. Aun así, `NVSDK_NGX_D3D12_Init_Ext` devuelve éxito para ambos, pero `CreateFeature` sólo funciona en el device inicializado primero. El segundo devuelve `0xbad00007` (`FAIL_NotInitialized`). Invertir el orden mediante `MGPU_NGX_SECOND_DEVICE_FIRST=1` invierte también cuál funciona.

**Impacto:** abrir dos adapters no es suficiente; el runtime/proxy conserva un contexto NGX efectivo por proceso. El bridge no puede enviar NR a B mientras comparta esa instancia global.

**Cómo se desbloquea:** comprobar si el contrato NGX permite una única instancia por proceso y multiplexar la evaluación, o cargar instancias aisladas del proxy/runtime con namespaces separados. La primera prueba debe ser una evaluación local real en B, sin transporte, antes de mover recursos.

### S11 — Herencia del FD entre Wine y el helper Linux

La primera prueba pasaba correctamente el número devuelto por `vkGetMemoryFdKHR`, pero `fstat()` en el helper devolvía `EBADF`. Vulkan marca los opaque FD como `FD_CLOEXEC` y `__wine_unix_spawnvp()` usa `fork()+execvp()`, por lo que el descriptor se cerraba antes de ejecutar CUDA.

**Corrección implementada:** `tests/fd_inherit_shim.c` intercepta el `execvp()` del proceso de probe y limpia `FD_CLOEXEC` para el conjunto acotado de descriptores del helper. Sólo se activa cuando se solicita `MGPU_CUDA_IMPORT_HELPER`; no se instala globalmente ni modifica el juego.

**Resultado:** el helper ve un descriptor NVIDIA válido (`fstat=char`), CUDA importa y mapea la asignación, escribe el patrón, copia con `cuMemcpyPeer` a GPU1 y valida el checksum. El script `scripts/run_mgpu_mvp.sh` automatiza este gate y mantiene el lanzamiento de juegos deshabilitado.

## Registro de ejecución — 2026-09-09

- [x] Se mantuvo vLLM detenido; no quedó proceso `vllm serve` activo.
- [x] Se mantuvieron las pruebas en prefixes/directorios aislados y no se modificaron monitores, RandR, Xorg ni la configuración global de juegos.
- [x] Se volvió a compilar el proyecto con CMake: correcto.
- [x] Se ejecutaron 11/11 tests Python con `unittest`: correcto, incluyendo launcher Proton y el gate que impide falso `READY_REMOTE`.
- [x] Se agregó el gate `transport_available`: la presencia de DLLs ya no puede declarar `READY_REMOTE` sin un backend cross-adapter real; suite final 11/11.
- [x] Se validó sintaxis de los scripts principales con `bash -n`: correcto.
- [x] Se ejecutó el ring P2P durante 300 frames de 8.294.400 bytes: 0 fallos, 11,13 GB/s en esa corrida.
- [x] `mgpu-auto selftest --json`: P2P bidireccional e interop Vulkan→CUDA→P2P en ambas direcciones, `passed=true`.
- [x] `mgpu-auto doctor`: dos RTX 3090, driver 595.71.05, 24 GiB cada una; GPU 1 casi libre.
- [x] Stress 4K RGBA16F (66.355.200 bytes): ring P2P de 100 frames sin errores a 9,32 GB/s; Vulkan→CUDA→P2P correcto en ambas direcciones (0→1: 0,713 GB/s; 1→0: 5,713 GB/s en esa corrida).
- [x] `run_ngx_test.sh` positivo con GE-Proton 11-6 y runtime NR local: dos dispositivos D3D12, `Init_Ext=0x1`, `CreateFeature=0x1`, `EvaluateFeature=0x1`; el retorno `0xbad00005` queda como resultado histórico de runtime contaminado.
- [x] `run_ngx_test.sh` con `MGPU_NGX_SECOND_DEVICE_TEST=1`: ambos objetos devuelven `Init_Ext=0x1` y `CreateFeature=0x1` simultáneamente; ambos liberan el feature y hacen shutdown correctamente.
- [x] Sonda NR directa compilada y ejecutada: `Init_Ext` directo `0xbad00002`; `Reserved18` a través del proxy `0xbad0000c`.
- [x] Verificación RandR final de esta sesión: dos salidas conectadas (`DP-0`, `HDMI-1-0`) y `DP-1-3` desconectada; no se hizo ninguna escritura de configuración.
- [x] Regresión `mgpu-auto run --exe` con GE-Proton: el proceso arrancó en GPU 1 y el watchdog lo terminó ordenadamente a los 8 s; la salida esperada fue `timed_out=true`.
- [x] Smoke cross-adapter D3D12: creación de heap/recurso local correcta; exportación de handle bloqueada por VKD3D (`E_NOTIMPL`/`DXGI_ERROR_INVALID_CALL`).
- [x] Inspección de VKD3D-Proton: localizado el punto de implementación (`device.c`, `heap.c`, `resource.c`) y confirmado que hace falta una SPI/patch del backend Unix.
- [x] Sonda `vkd3d_interop_probe`: ambos objetos D3D12 comparten el mismo device Vulkan; `VKD3D_VULKAN_DEVICE` es selección global por proceso.
- [x] La misma sonda obtuvo un `VkBuffer` y un `VkDeviceMemory` de recursos D3D12 reales mediante las interfaces base y `ID3D12DXVKInteropDevice3` del GE-Proton instalado.

## Pendientes priorizados después de esta sesión

- [ ] Conseguir un host real que invoque DLSS/NR bajo Proton y capturar los parámetros/recursos auténticos.
- [ ] Reproducir primero la evaluación local completa en GPU A y validar imagen/latencia.
- [x] Probar la inicialización de NGX en dos objetos D3D12; la selección de adapters Vulkan distintos quedó bloqueada por VKD3D.
- [ ] Implementar la sincronización cross-adapter entre esos devices.
- [ ] Integrar el transporte P2P con recursos compartidos sin staging por CPU.
- [x] Resolver la causa inmediata de importación: el FD Vulkan tenía `FD_CLOEXEC` y no llegaba abierto al helper; el shim lo limpia sólo durante el probe.
- [x] Validar asignación D3D12/VKD3D → FD → CUDA import/map → escritura → `cuMemcpyPeer` → checksum en GPU1.
- [ ] Resolver la identidad física de GPU1 dentro de la enumeración Vulkan de VKD3D; el host duplica UUID/PCI en las entradas experimentales.
- [ ] Validar la asignación dedicada con un recurso D3D12 real del juego.
- [ ] Implementar semáforos/fences externos y validar coherencia antes de copiar el frame.
- [ ] Sólo cuando NR local y remoto sean estables, investigar DLSSG SM86 2X; dejar 3X/4X para una fase posterior.

## Registro adicional — 2026-09-10

- [x] Se clonó e inspeccionó el fuente actual de VKD3D-Proton para localizar el bloqueo cross-adapter.
- [x] Se ejecutó la sonda D3D12 con heap y recurso cross-adapter: creación local correcta, exportación no implementada por VKD3D.
- [x] Se probó la variante de exportar heap y la variante de exportar recurso; ambas fallan por rutas distintas (`E_NOTIMPL` y `DXGI_ERROR_INVALID_CALL`).
- [x] Se corrigió el plan automático para exigir `transport_available`; la presencia de runtimes ya no habilita falsamente el modo remoto.
- [x] La suite automática quedó en 11/11 y `mgpu-auto doctor` informa `READY_LOCAL_ONLY` mientras el backend remoto siga cerrado.
- [x] Se añadió un gate reproducible para exigir dos adapters Vulkan distintos; en el host actual retorna código 7 con `VKD3D_INTEROP_REQUIRE_DISTINCT=1`.
- [x] Se añadieron `patches/vkd3d-duplicate-luid-adapters.patch`, `scripts/build_vkd3d_experimental.sh` y soporte `VKD3D_DLL_DIR` en los probes.
- [x] Build experimental VKD3D Win64: 208 tareas iniciales y rebuild incremental posterior correctos.
- [x] Probe con VKD3D experimental: `multi_adapter_distinct=yes`, `vkd3d_heap_memory_exported=yes`.
- [x] NGX con devices distintos y orden A→B/B→A: sólo el primer device crea feature; el segundo retorna `0xbad00007`.
- [x] Probe de exportación: `vkGetMemoryFdKHR` devuelve FD y el helper Linux lo recibe por `__wine_unix_spawnvp`.
- [x] Probe inicial de importación: `vkGetMemoryFdPropertiesKHR` devolvía `VK_ERROR_UNKNOWN` y el helper recibía `EBADF` por `FD_CLOEXEC`.
- [x] Se añadió el parche reproducible `vkd3d-export-opaque-fd-memory.patch` y el script de build del helper CUDA.
- [x] Se habilitó experimentalmente `VK_KHR_external_memory_fd` y `VkExportMemoryAllocateInfo` para heaps; el resultado no cambió, por lo que el problema no se resuelve sólo habilitando la extensión.
- [x] Diagnóstico interno VKD3D: la asignación exportable real del heap es de 65.536 bytes, memoria tipo 1; su dispatch propio devuelve `export=0` y `properties=-13`.
- [x] Corrección del launcher: `tests/fd_inherit_shim.c` limpia `FD_CLOEXEC` sólo durante el `fork/exec` del helper.
- [x] Validación Proton end-to-end: CUDA importa/mapea la asignación, escribe `0xA5`, copia con `cuMemcpyPeer` a GPU1 y valida checksum.
- [x] MVP automático: `scripts/run_mgpu_mvp.sh` devuelve `READY_REMOTE_TRANSPORT` con `game_launch=disabled`.
- [x] Regresión posterior: CMake, 11/11 tests Python, P2P, interop Vulkan→CUDA→P2P, `doctor`, `selftest` y sintaxis shell correctos.
- [x] Se detuvo el contenedor `vllm-qwen38-27b-dual-fast` a pedido del usuario; VRAM quedó aproximadamente en 857/66 MiB usados. RandR continúa con sólo `DP-0` y `HDMI-1-0` conectados; no se modificó la configuración de monitores.

## Criterios para declarar éxito

### MVP local

- [x] Dos GPUs identificadas por UUID/PCI.
- [x] P2P bidireccional validado.
- [x] Interop Vulkan/CUDA validada.
- [x] Selección automática y fallback local.
- [x] Lanzamiento aislado con watchdog.

### MVP DLSS estándar bajo Proton

- [x] D3D12 enumera las dos RTX 3090 correctas con GE-Proton/VKD3D-Proton.
- [x] El proxy NGX `Init_Ext` devuelve éxito.
- [x] El core oficial responde soporte para DLSS en `GetFeatureRequirements` antes de entrar al proxy.
- [x] `CreateFeature` completa sin device loss.
- [ ] `EvaluateFeature` completa con recursos/estados de un host real.
- [ ] El resultado visual se valida durante 30 minutos.

### MVP DLSS5 remoto

- [ ] Runtime NR compatible presente.
- [ ] Pass NR confirmado en GPU B por telemetría.
- [ ] Recursos llegan por P2P/cross-adapter, no por RAM.
- [ ] Frame final se presenta sin retorno innecesario a GPU A.
- [ ] Fallback local funciona ante cualquier error.

## Registro adicional — MVP automático de transporte

- [x] Se añadió `tests/fd_inherit_shim.c` para corregir la herencia `CLOEXEC` en el `fork/exec` controlado de Wine.
- [x] Se extendió `cuda_external_import_helper` con escritura, copia P2P y validación de checksum.
- [x] Se añadió `tests/vkd3d_vk_export_smoke.cpp` para comparar asignación Vulkan directa y asignación D3D12/VKD3D.
- [x] Se añadió `scripts/run_mgpu_mvp.sh` con salida JSON y gate `READY_REMOTE_TRANSPORT`.
- [x] Corrida verificada: `CUDA_SUCCESS`, mapeo correcto, `cuMemsetD8`, `cuMemcpyPeer` y `cuda_helper_p2p_validation=ok`.
- [ ] Conectar el transporte a un host real de DLSS/NR; el script mantiene `game_launch=disabled`.

## Registro adicional — 2026-09-10: hook de transporte en el bridge

- [x] Recuperar el fuente actual de `dlss5-linux-bridge` y conservar la modificación como `patches/dlss5-linux-bridge-transport-probe.patch`.
- [x] Añadir `scripts/build_bridge_transport_probe.sh`, que copia el fuente a un directorio temporal, verifica/aplica el patch y compila DLLs aisladas.
- [x] Hacer configurable `NGX_BRIDGE_DIR` en `scripts/run_ngx_test.sh` para probar un bridge alternativo sin sobrescribir `build/proton`.
- [x] Ejecutar el build parcheado con MinGW-w64 y headers NGX locales; compilación correcta.
- [x] Ejecutar `MGPU_DLSSNR_TRANSPORT=probe` bajo GE-Proton: el bridge consulta VKD3D y registra handles Vulkan, offsets y layouts de color, output, motion y depth.
- [x] Confirmar que el hook no cambia el resultado del smoke: con runtime limpio `EvaluateFeature=0x1`; el transporte FD sigue siendo sólo una sonda y no activa NR remoto.
- [ ] Exportar el `VkDeviceMemory` de un recurso/heap del host desde el proceso Wine sin depender de un helper externo.
- [ ] Importar la asignación en CUDA GPU B con sincronización de productor/consumidor.
- [ ] Reemplazar el modo `probe` por un backend remoto sólo después de validar identidad física de GPU B y fallback.

## Próximo orden recomendado

1. Conseguir un host que realmente invoque el proxy durante `EvaluateFeature`; el sample oficial D3D12 arrancó pero no dejó trazas del bridge.
2. Completar la evaluación local con recursos/estados auténticos y capturar una imagen antes de mover nada a GPU B.
3. Implementar una SPI de exportación/sincronización en VKD3D; no inferir un backend remoto desde handles privados solamente.
4. Conectar el hook a un transporte intra-proceso y validar una copia P2P sin NR, con identidad física de GPU B comprobada.
5. Recién después integrar `dlssg_for_sm86` y medir 2X/4X por separado.

## Registro adicional — 2026-09-10: diagnóstico del `E_NOTIMPL` de fence externo

- [x] Habilitar de forma optativa `VK_KHR_external_semaphore_fd` en el build Linux de VKD3D, sin exigirla en hosts que no la publican.
- [x] Añadir trazas de `externalSemaphoreFeatures`, `exportFromImportedHandleTypes`, `compatibleHandleTypes`, presencia de la extensión y puntero de `vkGetSemaphoreFdKHR`.
- [x] Añadir al probe un inventario de extensiones del `VkDevice` y separar `VK_KHR_external_semaphore_fd` de `VK_KHR_external_fence_fd`.
- [x] Confirmar en las RTX 3090 con driver 595.71.05: ninguna de las dos extensiones FD está habilitada; el puntero `vkGetSemaphoreFdKHR` es nulo.
- [x] Confirmar que el host sí reporta capacidades abstractas `features=0x3` y `opaque_fd=0x8`; no alcanza para invocar la API FD sin que el driver publique la extensión.
- [x] Validar una ruta alternativa de sincronización mediada por CPU: fence/cola en GPU A, espera con evento y señal posterior de una cola en GPU B; resultado `cpu_fence_sync=available`.
- [x] Mantener `ExportVulkanFenceFd` correctamente en `E_NOTIMPL` cuando falta la extensión, evitando fingir que un eventfd es un semaphore Vulkan.
- [ ] Implementar el backend remoto con gate CPU explícito, transferencia P2P y cola D3D12 B; todavía no equivale a sincronización GPU↔GPU nativa.
- [ ] Conseguir soporte real del driver para `VK_KHR_external_semaphore_fd`/`VK_KHR_external_fence_fd`, o diseñar un protocolo CUDA/host que no dependa de esas extensiones.

## Registro adicional — 2026-09-10: MVP CPU-gated P2P

- [x] Añadir `CpuSyncReport` y `benchmark_cpu_synchronized_ring()` al transporte CUDA.
- [x] Implementar ring de slots con polling CPU de eventos CUDA, validación de payload por frame y watchdog de stall configurable.
- [x] Añadir `mgpu-cpu-sync-p2p-probe` con `--frames`, `--slots`, `--timeout-ms` y salida JSON.
- [x] Validar A→B y B→A: 120/120 frames, checksum correcto, peer habilitado y sin timeout.
- [x] Integrar el probe en `run_mgpu_mvp.sh`; el estado `READY_CPU_SYNC_P2P` identifica transporte CPU-gated P2P, manteniendo `game_launch=disabled`.
- [x] Integrar el diagnóstico en `mgpu-auto doctor` como `cpu_sync_p2p_available=true`.
- [x] Marcar `gpu_native_sync=pending` en el plan automático y conservar el fallback local.
- [x] Añadir un contrato experimental de frame con color, motion vectors, depth y `frame_id` común.
- [x] Validar el transporte CPU-gated de los tres planos con checksum independiente por plano.
- [x] Integrar la validación multip plano en `run_mgpu_mvp.sh` y `mgpu-auto doctor` como `cpu_sync_frame_p2p`.
- [ ] Conectar este ring a imágenes reales de DLSS/NR y a una cola D3D12/Vulkan del device B.
- [ ] Reemplazar el gate CPU por semaphore/fence GPU nativo cuando el driver lo permita.

## Registro adicional — 2026-09-10: prueba de host y watchdog

- [x] Ejecutar el probe multip plano con tamaños equivalentes a 1080p en ambas direcciones: 120/120 frames y checksum correcto.
- [x] Ejecutar `mgpu-auto doctor` y el MVP integrado con el nuevo gate: `READY_CPU_FRAME_SYNC_P2P`.
- [x] Integrar el gate CUDA-native al MVP automático: `READY_CUDA_NATIVE_FRAME_SYNC_P2P` cuando los waits GPU→GPU pasan.
- [x] Añadir timeout también a la fase positiva Proton del smoke NGX para evitar que un proceso Wine colgado deje la iteración abierta.
- [x] Obtener una evaluación sintética utilizable del host de prueba: el proceso crea los dos devices y carga NGX, con retorno `EvaluateFeature=0x1`.
- [ ] Obtener una evaluación auténtica del host/juego: el smoke actual no sustituye la captura de un frame real.
- [x] Ejecutar y leer el output del command list del smoke sintético; el resultado sigue sin ser una validación visual ni una evaluación auténtica de juego.
- [x] Encapsular la ejecución del sample oficial D3D12 en `run_official_d3d12_host_probe.sh` con copia temporal, watchdog y estados JSON; el host aún no alcanza NGX.
- [ ] Capturar color, motion vectors y depth de esa evaluación real y conectarlos al frame ring.

## Registro adicional — 2026-09-10: aislamiento de NGX por proceso

- [x] Añadir al smoke D3D12 la consulta de `ID3D12DXVKInteropDevice5::GetVulkanPhysicalDeviceIdentity`.
- [x] Confirmar proceso aislado A: `uuid=af:6d:e4:b3`, `pci=0:1:0.0`, `EvaluateFeature=0x1`, chaining NR `0x1`, retorno 0.
- [x] Confirmar proceso aislado B: `uuid=5b:9f:38:5f`, `pci=0:3:0.0`, `EvaluateFeature=0x1`, chaining NR `0x1`, retorno 0.
- [x] Automatizar ambos sentidos y exigir PCI esperado en `run_ngx_process_isolation_matrix.sh`.
- [ ] Pasar de dos procesos sintéticos a un host de juego que entregue color, motion vectors y depth auténticos.
- [ ] Compartir esos recursos con el proceso/device B sin staging de RAM y coordinar su finalización.
- [ ] Mantener la sincronización GPU-nativa pendiente; el resultado actual sólo prueba aislamiento y ejecución local.

## Registro adicional — 2026-09-10: SPI de heap y MVP `fd-probe`

- [x] Añadir `ID3D12DXVKInteropDevice4` con `ExportVulkanHeapFd`, protegido por `VKD3D_EXPORT_HEAP_FD=1`.
- [x] Corregir el build MinGW: `fcntl.h`/`FD_CLOEXEC` no están disponibles en el DLL PE; la limpieza de herencia permanece en el shim Linux.
- [x] Hacer que `EnsurePrivateOutput` use `CreateHeap` + `CreatePlacedResource` y conserve `ID3D12Heap`/tamaño.
- [x] Añadir `MGPU_DLSSNR_TRANSPORT=fd-probe` al bridge; ejecuta el export antes del `EvaluateFeature` estándar para que el gate no dependa de `0xbad00005`.
- [x] Actualizar `build_bridge_transport_probe.sh` para aplicar los parches `transport-probe` y `fd-probe` en una copia temporal limpia.
- [x] Actualizar `run_ngx_test.sh` para activar automáticamente memoria exportable y el shim sólo en `fd-probe`.
- [x] Build completo de VKD3D mediante `scripts/build_vkd3d_experimental.sh`; los cinco parches se detectan/aplican y los DLL se instalan correctamente.
- [x] Smoke interop con build instalado: `ExportVulkanHeapFd=0x0`, helper `fstat=char`, CUDA import/map y P2P/checksum correctos.
- [x] Smoke bridge automático: output colocado `1280x720`, heap `7.864.320` bytes, export exitoso y helper con `spawn_rc=0`.
- [ ] Resolver `vkGetMemoryFdPropertiesKHR=-13` bajo Wine; CUDA funciona en esta ruta, pero el contrato Vulkan estándar sigue sin validarse.
- [ ] Reemplazar el shim por una vía de transporte de FD nativa y explícita cuando se cierre el contrato del host.
- [ ] Pasar de heap/buffer de validación a imagen/sincronización real de DLSS/NR.
