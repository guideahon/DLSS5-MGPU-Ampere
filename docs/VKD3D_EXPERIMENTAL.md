# VKD3D experimental para adapters con LUID duplicado

## Importación de recursos FD entre adapters

La ruta reproducible actual requiere tres cambios coordinados: Wine debe exponer
`VK_KHR_external_memory_fd`, `win32u` debe conservar
`VkImportMemoryFdInfoKHR` al reenviar `vkAllocateMemory`, y VKD3D debe ofrecer
`ID3D12DeviceExt6::CreateResourceFromExternalFd`. El build aplica esos cambios
con `scripts/build_winevulkan_experimental.sh` y
`scripts/build_vkd3d_experimental.sh`.

El smoke se ejecuta con:

```bash
./scripts/run_vkd3d_cross_adapter_resource_import_smoke.sh
```

Valida por separado la exportación, la identidad UUID/PCI, la importación y el
binding en GPU B, y el contenido leído desde B. En el host actual la primera
parte pasa, pero el contenido remoto queda en cero: `OPAQUE_FD` no debe
interpretarse como memoria P2P visible entre dos físicos NVIDIA sólo porque
`vkAllocateMemory`/`vkBindImageMemory2` devuelvan éxito. El MVP operativo usa
una copia explícita `D3D12 resource-FD → CUDA → cuMemcpyPeer → D3D12 B`, con
fences CPU y timeout; el runner del host sintético validó tres planos, tres
iteraciones persistentes y un frame-loop de 3/3 frames en ambas orientaciones.
El aliasing Vulkan directo queda como diagnóstico, no como transporte del MVP.

La sincronización GPU-native sigue marcada como pendiente: el roundtrip de fence
cross-adapter aislado pasa, pero todavía no existe un ring de imágenes de un
juego real con ownership/layout y señalización de colas integrada.

El host Proton/DXVK de esta máquina expone dos RTX 3090 con el mismo `AdapterLuid`. El comportamiento normal de VKD3D usa ese LUID para el singleton y termina reutilizando el mismo device Vulkan.

El parche [vkd3d-duplicate-luid-adapters.patch](/home/cristian/Documentos/ChatGPT/3090-DLSS5/patches/vkd3d-duplicate-luid-adapters.patch) agrega un modo opt-in:

```bash
VKD3D_DUPLICATE_LUID_ADAPTERS=1
```

En ese modo, `d3d12core` fuerza devices independientes. `VKD3D_DUPLICATE_LUID_INDEX=0|1` permite seleccionar explícitamente una entrada Vulkan por proceso. Se usa sólo para el laboratorio de esta máquina: no debe habilitarse globalmente ni en juegos sin verificar UUID/PCI.

## Compilar

```bash
VKD3D_SOURCE_DIR=/tmp/dlss5-vkd3d-proton \
VKD3D_BUILD_DIR=/tmp/dlss5-vkd3d-build \
VKD3D_INSTALL_DIR=/tmp/dlss5-vkd3d-install \
./scripts/build_vkd3d_experimental.sh
```

El resultado queda en `/tmp/dlss5-vkd3d-install/bin`. La sonda puede ejecutarse así:

```bash
OUT_DIR=/tmp/dlss5-vkd3d-probe-custom-run \
VKD3D_DLL_DIR=/tmp/dlss5-vkd3d-install/bin \
VKD3D_DUPLICATE_LUID_ADAPTERS=1 \
VKD3D_INTEROP_REQUIRE_DISTINCT=1 \
VKD3D_INTEROP_REQUIRE_HEAP=1 \
PROTON=/ruta/a/GE-Proton/proton \
./scripts/run_vkd3d_interop_probe.sh
```

### Validación aislada de fence D3D12

El checkout actual de VKD3D-Proton puede probar la SPI de fence sin aplicar los
parches de importación de recursos que pertenecen a una base anterior:

```bash
VKD3D_FENCE_ONLY=1 \
VKD3D_SOURCE_DIR=/tmp/dlss5-vkd3d-proton \
VKD3D_BUILD_DIR=/tmp/dlss5-vkd3d-build \
VKD3D_INSTALL_DIR=/tmp/dlss5-vkd3d-install \
./scripts/build_vkd3d_experimental.sh
```

El runner `scripts/run_vkd3d_fence_fd_smoke.sh` necesita un Wine completo
parcheado y compilado con X11, por ejemplo en
`/tmp/dlss5-wine-build-x`. Inicializa un prefix temporal con `wineboot`, evita
su actualización por otra build y valida: fence D3D12 compartida → FD OPAQUE →
semáforo Vulkan timeline → señal D3D12 → `vkWaitSemaphores` en valor 1.
El resultado positivo es sólo transporte intra-device D3D12↔Vulkan; no cierra
la sincronización GPU-native cross-adapter ni habilita juegos automáticamente.

Resultado observado: dos handles `VkPhysicalDevice`/`VkDevice` distintos en la prueba de dos objetos, pero este host reporta la misma identidad UUID/PCI para las entradas duplicadas bajo VKD3D. No se debe interpretar todavía como dos GPUs físicas distintas.

### Validación cross-adapter de fence

El fixture `tests/vkd3d_cross_adapter_fence_smoke.cpp` fuerza dos selecciones
de adapter con `VKD3D_DUPLICATE_LUID_ADAPTERS=1`, consulta la identidad
UUID/PCI expuesta por cada `ID3D12DXVKInteropDevice5` y aborta si las dos
selecciones no son físicas y lógicamente distintas. El runner es:

```bash
WINE_BUILD_DIR=/tmp/dlss5-wine-build-x \
WINEPREFIX=/tmp/dlss5-system-wine-prefix \
WINE_BOOTSTRAP_PREFIX=0 \
VKD3D_DLL_DIR=/tmp/dlss5-vkd3d-install/bin \
./scripts/run_vkd3d_cross_adapter_fence_smoke.sh
```

La prueba pasó en las dos RTX 3090: A crea y señaliza una fence D3D12,
exporta un FD OPAQUE, B lo importa como semáforo Vulkan timeline y observa
`counter=1`/`vkWaitSemaphores=VK_SUCCESS`. El resultado es
`cross_adapter_fence_roundtrip=pass`.

También puede validarse el consumidor CUDA en la segunda GPU:

```bash
MGPU_FENCE_CUDA_WAIT=1 \
MGPU_FENCE_CUDA_RELAY=1 \
MGPU_FENCE_CUDA_GPU_SIGNAL=1 \
MGPU_FENCE_CUDA_WAIT_ORDINAL=1 \
WINE_BUILD_DIR=/tmp/dlss5-wine-build-fence \
VKD3D_DLL_DIR=/tmp/dlss5-vkd3d-install-fence/bin \
./scripts/run_vkd3d_cross_adapter_fence_smoke.sh
```

El helper nativo importa el FD de A en CUDA B, espera con
`cuWaitExternalSemaphoresAsync`, señaliza una segunda fence exportada desde B
con `cuSignalExternalSemaphoresAsync` en el mismo stream y devuelve `done
rc=0`. En el host dual el resultado fue `cuda_fence_wait=pass`,
`cuda_fence_relay=pass` y `cross_adapter_fence_roundtrip=pass`. Las variables
`MGPU_FENCE_CUDA_RELAY` y `MGPU_FENCE_CUDA_GPU_SIGNAL` quedan opt-in: el MVP
automático no las activa hasta que el relay se use también para las imágenes
reales del frame-loop.

El runner copia `cryptbase.dll` y `winex11.drv` de la misma build Wine al
prefix temporal, porque `WINEDLLPATH` no busca recursivamente dentro de los
directorios de módulos. Esto es parte del harness aislado y no altera el Wine,
Proton, RandR ni Xorg del sistema.

### Frame-loop GPU-ordered de resource-FD

El smoke `scripts/run_vkd3d_resource_fd_gpu_sync_smoke.sh` valida la primera
cadena completa de recursos y fences entre D3D12 y CUDA:

```bash
WINE_BUILD_DIR=/tmp/dlss5-wine-build-fence \
VKD3D_DLL_DIR=/tmp/dlss5-vkd3d-install-resource-gpu/bin \
MGPU_GPU_SYNC_FRAMES=3 \
./scripts/run_vkd3d_resource_fd_gpu_sync_smoke.sh
```

Para verificar ambas orientaciones en una sola corrida:

```bash
MGPU_GPU_SYNC_BOTH=1 MGPU_GPU_SYNC_FRAMES=3 \
./scripts/run_vkd3d_resource_fd_gpu_sync_smoke.sh
```

El worker persistente importa una vez los dos resource-FD, crea contextos CUDA
y conserva el stream durante la corrida. En cada frame, D3D12 A copia un patrón
al recurso y señaliza el fence del slot; CUDA B espera ese fence, ejecuta
`cuMemcpyPeerAsync`, señaliza el fence de salida del mismo slot y D3D12 B espera
esa señal antes del readback. La corrida validada pasó
`gpu_sync_resource_frame_loop=pass frames=3/3 mode=persistent` en las dos RTX
3090. Se usa un pool de fences one-shot porque la reutilización de una misma
fence timeline se bloqueó en el tercer valor bajo este bridge.

Este resultado es un loop GPU-ordered sintético y acotado: el helper CUDA se
lanzó una vez y conserva los imports/contextos/stream, pero no hay todavía un
ring persistente compartido con un juego ni se ha conectado la evaluación NR
real a este contrato. Por eso la
sincronización GPU-native del MVP remoto sigue marcada como pendiente; el
fallback automático CPU-gated permanece sin cambios.

Para conservar el fixture histórico one-shot por frame se puede usar
`MGPU_GPU_SYNC_PERSISTENT=0`; el runner usa el worker persistente por defecto.

Este check valida señalización cross-adapter entre D3D12, Vulkan y CUDA, pero
sigue siendo aislado: permanecen pendientes ownership/layout de imágenes,
evaluación NR sobre un recurso de juego y el ring GPU-native completo. El MVP
continúa usando espera CPU con timeout.

## Experimento de memoria externa FD

El build incluye un segundo opt-in para probar la compatibilidad de heaps D3D12 con CUDA:

```bash
./scripts/build_cuda_external_import_helper.sh
PROTON=/ruta/a/GE-Proton/proton \
VKD3D_DLL_DIR=/tmp/dlss5-vkd3d-install/bin \
VKD3D_DUPLICATE_LUID_ADAPTERS=1 \
VKD3D_EXPORT_OPAQUE_FD_MEMORY=1 \
MGPU_CUDA_IMPORT_HELPER=/ruta/al/proyecto/build/cuda_external_import_helper \
WINEDLLOVERRIDES='d3d12=n,b;d3d12core=n,b' \
./scripts/run_vkd3d_interop_probe.sh
```

Resultado observado en este host: `vkGetMemoryFdKHR` devuelve un FD y la consulta `vkGetMemoryFdPropertiesKHR` retorna `-13` (`VK_ERROR_UNKNOWN`). El problema inicial adicional era `FD_CLOEXEC`: `__wine_unix_spawnvp` pasaba el número, pero el helper recibía `EBADF`. Con `tests/fd_inherit_shim.c`, el helper recibe un FD válido; `cuImportExternalMemory`, el mapeo, `cuMemsetD8`, `cuMemcpyPeer` y la validación de checksum pasan desde la asignación D3D12/VKD3D hacia CUDA GPU1. La selección experimental ahora deduplica UUID/PCI y confirma A `0:1:0.0` y B `0:3:0.0`. El siguiente trabajo es sincronizar un recurso real del juego; la SPI de fence compila, pero este host devuelve `E_NOTIMPL` al exportarla.

## MVP automático

```bash
PROTON=/ruta/a/GE-Proton/proton \
VKD3D_DLL_DIR=/tmp/dlss5-vkd3d-install/bin \
./scripts/run_mgpu_mvp.sh
```

El resultado `READY_REMOTE_TRANSPORT` sólo significa que el transporte de memoria de laboratorio está validado. No significa que NGX/DLSS-NR ya esté conectado ni que se lance un juego.

## Límite NGX observado

Con `MGPU_NGX_SECOND_DEVICE_TEST=1`, NGX puede recibir `Init_Ext` en ambos devices. Sin embargo, sólo el device inicializado primero puede crear el feature; el segundo devuelve `0xbad00007` (`FAIL_NotInitialized`). `MGPU_NGX_SECOND_DEVICE_FIRST=1` permite invertir el experimento y produce el resultado simétrico.

Por eso el parche VKD3D resuelve la selección de hardware, pero no habilita todavía Neural Rendering remoto. Falta aislar o adaptar el estado global del runtime/proxy NGX, importar las imágenes reales en B, obtener fences/semaphores externos y conectar una evaluación real en un command list de B.

## SPI experimental de recursos comprometidos

La cadena actual añade `ID3D12DXVKInteropDevice6::ExportVulkanResourceFd`. Con
`VKD3D_EXPORT_RESOURCE_FD=1`, VKD3D marca las asignaciones comprometidas con
`VkExportMemoryAllocateInfo` y permite obtener el FD de la asignación que respalda
un `ID3D12Resource` real.

El bridge lo prueba con:

```bash
MGPU_DLSSNR_TRANSPORT=resource-fd-probe
VKD3D_EXPORT_RESOURCE_FD=1
MGPU_VULKAN_IMAGE_IMPORT_HELPER=/ruta/al/proyecto/build/mgpu-vulkan-image-import-helper
```

Para probar el camino CUDA directo desde los recursos que recibe NGX, usar el
runner oficial con `MGPU_DLSSNR_TRANSPORT=resource-fd-probe` y
`MGPU_CUDA_IMPORT_HELPER=/ruta/al/proyecto/build/cuda_external_import_helper`.
El runner activa `VKD3D_EXPORT_RESOURCE_FD=1` antes de crear las asignaciones.
Como NGX corre en el device consumidor, el bridge invierte automáticamente los
ordinales del transporte; se pueden sobrescribir con
`MGPU_CUDA_BRIDGE_SOURCE_ORDINAL` y `MGPU_CUDA_BRIDGE_DESTINATION_ORDINAL`.
La prueba de laboratorio actual importa los cuatro allocations reales
(`color`, `output`, `motion`, `depth`) en la GPU propietaria, hace P2P a la otra
3090 y valida el contenido en ambas direcciones.

Para repetir los tres planos del smoke sin recrear imports/contextos en cada
iteración:

```bash
MGPU_CROSS_ADAPTER_RESOURCE_FD=1 \
MGPU_CROSS_ADAPTER_PERSISTENT_FRAMES=8 \
./scripts/run_d3d12_cross_adapter_frame_probe.sh
```

Esto es un worker persistente dentro de una corrida de prueba, no todavía un
daemon conectado al frame loop de un juego.

El helper reconstruye una imagen en GPU B y usa `bind-only` para aislar
exportación/importación/binding de cualquier acceso o layout que todavía no pueda
inferirse con seguridad desde el descriptor D3D12. En el host Donut actual,
`color`, `output`, `motion` y `depth` obtienen `vkBindImageMemory=VK_SUCCESS`.

Esto no implica que el runtime NGX pueda ejecutar NR sobre esas imágenes: falta
crear/usar el command list de B, coordinar la finalización del productor y resolver
la presentación. La sincronización GPU-native sigue pendiente; el fallback del MVP
es CPU-gated con timeout.

## Daemon resource-FD del bridge

El modo `MGPU_DLSSNR_TRANSPORT=resource-fd-worker` mantiene un proceso nativo
CUDA conectado al bridge por loopback. `MGPU_CUDA_IMPORT_HELPER` queda reservado
para el importador individual; el daemon persistente se indica con
`MGPU_CUDA_WORKER_HELPER`:

```bash
MGPU_DLSSNR_TRANSPORT=resource-fd-worker \
MGPU_CUDA_IMPORT_HELPER=/ruta/al/build/cuda_external_import_helper \
MGPU_CUDA_WORKER_HELPER=/ruta/al/build/mgpu-cuda-external-p2p-copy-helper \
MGPU_CUDA_P2P_COPY_HELPER=/ruta/al/build/mgpu-cuda-external-p2p-copy-helper \
MGPU_CROSS_ADAPTER_RESOURCE_FD=1 \
./scripts/run_d3d12_cross_adapter_frame_probe.sh
```

El bridge exporta los cuatro recursos, hereda únicamente sus FDs al daemon y
envía `c` por cada evaluación; el daemon responde `OK <microsegundos>` y se
cierra con `q`. Es una prueba CPU-gated y sintética: todavía no ejecuta el
runtime NR sobre recursos remotos de un juego ni presenta el resultado desde B.
La sincronización GPU-native sigue siendo un pendiente explícito porque VKD3D
devuelve `E_NOTIMPL` para la exportación de fences.

El hook `MGPU_DLSSNR_WORKER_TEST_REPEAT=N` permite validar el protocolo sin
relanzar el daemon: con `N=8` se observaron ocho respuestas `OK` A→B y ocho
B→A. La variable es sólo diagnóstica y no reemplaza la señalización de colas
ni un frame loop real.

Para probar el destino D3D12 directamente, el smoke acepta
`MGPU_CROSS_ADAPTER_RESOURCE_DAEMON=1`. El daemon importa el FD de cada recurso
en A y el FD de su allocation equivalente en B; `cuMemcpyPeer` escribe en B y
el mismo proceso ejecuta después el command list/NGX sobre esas texturas. El
resultado positivo en ambas orientaciones no elimina la limitación del bridge:
la creación de los recursos equivalentes todavía está en el host sintético,
no en un juego real.

En el mismo modo, el smoke crea una allocation de output en A, exporta el
output generado por NGX en B y ejecuta un segundo daemon en sentido inverso.
El readback en A valida el round-trip completo (`remote_output_returned=true`);
la espera entre ambos command lists continúa siendo CPU-gated.
