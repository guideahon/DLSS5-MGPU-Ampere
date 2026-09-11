# DLSS5-MGPU-Ampere

Experimento Linux para usar dos RTX 3090 como:

```text
GPU A = renderizado del juego
GPU B = Neural Rendering / presentación
```

La implementación inicial contiene un transporte CUDA P2P verificable. No incluye DLLs de NVIDIA, runtimes DLSS, modelos, CUBINs ni archivos de juegos.

El plan operativo completo, con checkmarks de lo ejecutado, criterios de aceptación y stoppers encontrados está en [docs/IMPLEMENTATION_PLAN.md](docs/IMPLEMENTATION_PLAN.md).

El registro técnico consolidado de cambios, mejoras, pruebas y resultados está en [docs/CHANGELOG.md](docs/CHANGELOG.md).

## Estado actual

Implementado:

- Enumeración de GPUs CUDA.
- Detección de capacidad P2P en ambos sentidos.
- Activación de peer access cuando el driver lo permite.
- Buffers de prueba en ambas GPUs.
- Copia GPU→GPU mediante `cudaMemcpyPeerAsync`.
- Validación de integridad con kernel CUDA en la GPU destino.
- Benchmark en ambos sentidos.
- Ring asíncrono de slots con eventos CUDA y política de finalización por frame.
- Enumeración Vulkan deduplicada por UUID/PCI.
- Exportación de buffer Vulkan `DEVICE_LOCAL` mediante opaque fd.
- Importación de esa memoria desde CUDA.
- Copia de memoria Vulkan importada hacia la segunda GPU mediante CUDA P2P.
- Validación end-to-end Vulkan → CUDA → P2P.
- MVP automático Proton → FD Vulkan → CUDA → P2P con validación end-to-end.
- SPI VKD3D opt-in para exportar el heap D3D12 real como FD Vulkan.
- Hook de bridge `MGPU_DLSSNR_TRANSPORT=fd-probe` que ejecuta el smoke CUDA/P2P desde la evaluación NGX.
- Normalización del contrato de parámetros DLSS; el smoke sintético GE-Proton ya completa `EvaluateFeature=0x1`.
- SPI opt-in de identidad física y selección VKD3D deduplicada por UUID/PCI para abrir A y B en el mismo proceso.
- Probe opt-in de fence FD; queda cerrado cuando el host no expone semáforos externos (`E_NOTIMPL`).
- Transporte CPU-gated P2P con ring de slots, polling de eventos CUDA, checksum por frame y timeout de stall.
- Probe automático `mgpu-cpu-sync-p2p-probe` en ambas direcciones.
- Probe de sincronización CUDA nativa mediante `cudaStreamWaitEvent`, sin staging por RAM; el fence D3D12/Vulkan sigue pendiente.
- Probe de imagen cross-device: exporta el heap del output D3D12 de A, intenta importar una `VkImage` RGBA16F en B y valida `clear/copy/readback` cuando el driver acepta la orientación.
- Fallback de imagen lineal GPU→GPU: dos imágenes Vulkan equivalentes se copian por asignaciones CUDA mapeadas y `cudaMemcpyPeer`, con readback validado en ambas direcciones.
- Smoke D3D12 de textura→buffer lineal: `CopyTextureRegion`, fence CPU, exportación del heap y CUDA/P2P con pixel readback correcto en ambas orientaciones.
- Smoke NGX D3D12 con envío real del command list, fence CPU y readback del output; acredita ejecución/legibilidad del recurso, pero no declara NR visual porque la firma del buffer coincide entre los hosts positivo y negativo.
- Matriz de aislamiento por proceso: una instancia Proton/VKD3D por GPU verifica UUID/PCI de A y B y completa el chaining local DLSS→NR en ambas 3090.
- Contrato experimental de frame con tres planos (color, motion y depth), `frame_id` común y validación por plano.
- Salida humana y JSON.

## MVP automático

El objetivo del MVP no es soportar todos los juegos. Es ejecutar un título D3D12 bajo Proton con una configuración segura y reproducible:

```text
./scripts/mgpu-auto doctor
./scripts/mgpu-auto run --game cyberpunk2077 --dry-run
```

Para ejecutar sólo el MVP técnico, sin lanzar juegos:

```bash
PROTON=/ruta/a/GE-Proton/proton \
VKD3D_DLL_DIR=/tmp/dlss5-vkd3d-install/bin \
./scripts/run_mgpu_mvp.sh
```

El modo automático debe:

1. Enumerar CUDA y Vulkan.
2. Deduplicar adaptadores por UUID/PCI.
3. Detectar cuál GPU tiene salida de pantalla.
4. Elegir la otra GPU como render si está libre y tiene P2P válido.
5. Ejecutar un self-test de transporte.
6. Comprobar la presencia de los runtimes NGX proporcionados por el usuario.
7. Generar una configuración aislada por juego.
8. Preparar una política de lanzamiento por juego sin modificar el prefix.
9. Mantener fallback local explícito si falla el self-test, falta memoria o no aparecen los runtimes.
10. Dejar listo el punto de integración para vigilar device loss, importación y NGX.

El MVP no intentará modificar juegos con anti-cheat, activar Frame Generation remoto ni descargar DLLs propietarias. El primer objetivo será un juego D3D12 concreto y una versión fija de Proton.

Fuera del MVP actual:

- Captura de una imagen Vulkan/VKD3D de un juego real.
- Proxy NGX/DLSS-NR.
- Presentación de una ventana en la GPU B.
- Frame Generation remoto.

## Verificación realizada en Linux

En una máquina con dos RTX 3090, driver 595.71.05 y Wine 9.0 se verificó:

- CUDA P2P bidireccional habilitado y validado: aproximadamente 10.3 GB/s en 0→1 y 12.4 GB/s en 1→0.
- Interoperabilidad Vulkan→CUDA→P2P en las dos direcciones: validación correcta, aproximadamente 4.4–6.0 GB/s según la carga.
- Stress con un frame 4K RGBA16F (66.355.200 bytes): ring de 100 frames sin errores a 9,32 GB/s; la ruta Vulkan→CUDA→P2P también validó ambas direcciones.
- El sample oficial DLSS v310.9.1 de NVIDIA arranca nativamente en Linux, carga `libnvidia-ngx-dlss.so.310.9.1` y obtiene los requisitos NGX en las RTX 3090.
- El probe oficial D3D12 bajo GE-Proton crea el device físico A y encuentra la escena Sponza, pero en este host queda bloqueado antes de cargar `nvngx_dlss.dll` incluso con 120 s; el runner limpia el proceso-grupo completo al vencer el watchdog. Esto sigue siendo un stopper del host, no una validación negativa de NGX.
- El bridge Windows compilado carga bajo Wine y expone los exports NGX esperados.
- El demo D3D12 aislado funciona con VKD3D para renderizar. Con Wine del sistema el smoke enumera un adaptador sintético `NVIDIA GeForce GTX 470` y no alcanza feature level 12.0; con GE-Proton 11-6/VKD3D-Proton el mismo host enumera las dos RTX 3090 y crea ambos dispositivos D3D12 correctamente.
- En el prefix GE-Proton aislado, el proxy NGX inicializa el core (`0x1`), inicializa DLSS estándar (`0x1`), crea el feature DLSS y carga/crea el feature Neural Rendering con el runtime comunitario `nvngx_dlssnr.dll` 310.8.0. El runtime reporta referencias a `sm86`, y el smoke sintético con recursos/contrato normalizados completa `EvaluateFeature=0x1`. Esto no equivale todavía a validación visual en un juego real.
- El smoke NGX ahora sube color, motion vectors, depth y output deterministas a recursos D3D12 y toma baseline/post-readback con fence CPU. `MGPU_NGX_INPUT_VARIANT=0|1` varía las entradas y `MGPU_NGX_OUTPUT_VARIANT` controla por separado el seed del output (default 2), permitiendo medir sensibilidad sin confundir ambas señales; sigue sin ser validación visual de un juego.
- En VKD3D experimental, el modo B-first (`MGPU_NGX_SECOND_DEVICE_FIRST=1` y `MGPU_NGX_EVALUATE_SECOND_DEVICE=1`) también evalúa recursos locales en la segunda 3090: B (`pci=0:3:0.0`) devuelve `EvaluateFeature=0x1` y readback `fnv1a=0x3c413a88d2048413`. A inicializada después devuelve `0xbad00007`, por lo que sigue faltando estado NGX multi-device y transporte remoto desde A.
- El probe `scripts/run_d3d12_cross_adapter_frame_probe.sh` transporta tres planos sintéticos (`Color`, `MotionVectors`, `Depth`) de A a B mediante rangos lineales en heaps FD y `cuMemcpyPeer`, reconstruye las texturas en B y valida el readback sin staging de RAM. Con `MGPU_NGX_CROSS_ADAPTER=1` (por defecto), además entrega los tres recursos a NGX en B y valida `EvaluateFeature=0x1` más readback no nulo. Usa fences CPU; todavía no consume buffers auténticos de un juego.
- Para repetir la misma prueba en sentido B→A, basta añadir `MGPU_CROSS_ADAPTER_REVERSE=1`; el launcher selecciona automáticamente CUDA 1→0 y fuerza el índice físico VKD3D correspondiente durante cada creación. Ambas orientaciones pasan el MVP sintético lineal con NGX en el consumidor.
- Para probar el bypass de asignación de textura sin reconstrucción lineal, usar `MGPU_NGX_CROSS_ADAPTER=0 MGPU_CROSS_ADAPTER_RESOURCE_FD=1`: exporta la textura D3D12 de A y la equivalente de B, copia la allocation completa por CUDA P2P y valida el readback D3D12 en ambas orientaciones. Este modo sigue siendo CPU-gated y no es todavía NR remoto.
- El helper CUDA batched importa los heaps una sola vez por frame de prueba y mueve los tres rangos en una única invocación; la corrida medida registró ~0,30 s de transporte y ~17,6 ms de cola/fence B+NGX. El tiempo total del proceso no representa frametime porque incluye el arranque de Proton.

El gate B-first se puede repetir automáticamente con
`scripts/run_ngx_same_process_b_probe.sh`; verifica las identidades físicas,
la evaluación/readback en B, la fence CPU y el bloqueo global esperado en A.
- La sonda aislada `tests/ngx_nr_direct_smoke.cpp` intentó además usar NR como feature independiente: el DLL directo devuelve `0xbad00002` en `Init_Ext` y el proxy devuelve `0xbad0000c` para `Reserved18`. Esto confirma que el runtime comunitario sólo está accesible en el chaining interno observado hasta `CreateFeature`.
- Con `MGPU_NGX_SECOND_DEVICE_TEST=1`, el smoke inicializa NGX y crea features en dos `ID3D12Device` simultáneos, y libera ambos correctamente. Esto valida la reentrancia básica de NGX, no que cada objeto esté respaldado por una RTX 3090 distinta ni que exista transporte cross-adapter.
- La sonda `tests/vkd3d_interop_probe.cpp` añadió una comprobación más estricta: en GE-Proton/VKD3D-Proton, ambos `ID3D12Device` del mismo proceso devuelven el mismo `VkPhysicalDevice` y `VkDevice`. `VKD3D_VULKAN_DEVICE=0/1` cambia el device Vulkan elegido para todo el proceso, pero no permite mezclar ambos adapters D3D12 en una sola instancia.
- La misma sonda confirma que GE-Proton expone `ID3D12DXVKInteropDevice3`: se obtiene el `VkBuffer` de un recurso D3D12 real y el `VkDeviceMemory` de un heap mediante `GetVulkanResourceInfo`/`GetVulkanHeapInfo`.
- Se compiló una variante experimental aislada de VKD3D-Proton con `VKD3D_DUPLICATE_LUID_ADAPTERS=1`. El parche permite abrir handles Vulkan independientes para la prueba, pero el host todavía reporta la misma identidad UUID/PCI en sus entradas duplicadas; los DLL quedan fuera del Proton instalado.
- Se añadió `VKD3D_EXPORT_OPAQUE_FD_MEMORY=1`: el heap entrega un FD con `vkGetMemoryFdKHR`. El shim de herencia corrige `FD_CLOEXEC`; CUDA importa, mapea, escribe y copia por P2P hacia la segunda 3090. `vkGetMemoryFdPropertiesKHR` aún devuelve `VK_ERROR_UNKNOWN` bajo Wine.
- La instrumentación interna de VKD3D confirma `allocation=65536`, `type=1`, `export=0`, `properties=-13`; esa consulta falla, pero no bloquea la importación CUDA una vez corregida la herencia del FD.
- Con esos devices distintos, NGX inicializa en A y B, pero el runtime mantiene estado efectivo para un solo device: A primero permite `CreateFeature` en A y B devuelve `0xbad00007`; B primero invierte el resultado. Esto confirma que el siguiente bloqueo está dentro de la gestión de estado NGX/proxy, no en la apertura de adapters ni en P2P.
- La prueba `tests/d3d12_cross_adapter_smoke.cpp` crea correctamente el heap/recurso con flags cross-adapter, pero VKD3D-Proton todavía devuelve `E_NOTIMPL` al exportar el handle del heap y `DXGI_ERROR_INVALID_CALL` al exportar el recurso. La siguiente implementación debe usar interop Vulkan/CUDA/P2P dentro del proceso.

Los artefactos propietarios y comunitarios se mantuvieron fuera del repositorio, en instalaciones locales de prueba. El bridge no incluye ni descarga `nvngx_dlssnr.dll`; hay que proporcionarlo explícitamente. El script reproducible conserva esa separación y ejecuta tanto el sample Linux como los smoke tests Windows:

```bash
DLSS_DEMO_DIR=/ruta/a/DLSS_Sample_App/bin/ngx_dlss_demo \
NGX_SDK_DIR=/ruta/a/DLSS \
WINEPREFIX=/tmp/dlss5-wine64-final \
./scripts/run_ngx_test.sh
```

Para repetir la prueba positiva en un prefix GE-Proton aislado, además de los headers y el sample oficial, se puede indicar un runtime NR obtenido por el usuario:

```bash
DLSS_DEMO_DIR=/ruta/a/DLSS_Sample_App/bin/ngx_dlss_demo \
NGX_SDK_DIR=/ruta/a/DLSS \
PROTON=/ruta/a/GE-Proton/proton \
DLSS_NR_DLL=/ruta/a/nvngx_dlssnr.dll \
DLSS5_PROTON_PREFIX=/tmp/dlss5-proton-ngx-positive \
MGPU_NGX_SECOND_DEVICE_TEST=1 \
./scripts/run_ngx_test.sh
```

Si el `nvngx_dlss.dll` del directorio del demo fue reemplazado por un proxy durante una prueba anterior, el launcher lo detecta y busca automáticamente el runtime limpio en `NGX_SDK_DIR/lib/Windows_x86_64/rel/nvngx_dlss.dll`. También se puede indicar explícitamente `DLSS_RUNTIME_DLL=/ruta/a/nvngx_dlss.dll`.

El bloque positivo sólo usa ese runtime dentro del prefix temporal y deja registro de `Init`, `Create` y `Evaluate`; no instala DLLs en juegos ni en el sistema.

La sonda NR independiente se puede recompilar (requiere los headers del SDK) y ejecutar dentro del mismo tipo de prefix aislado:

```bash
x86_64-w64-mingw32-g++ -std=c++17 -O2 -I/ruta/a/DLSS/include \
  -o build/ngx_nr_direct_smoke.exe tests/ngx_nr_direct_smoke.cpp \
  -ld3d12 -ldxgi -static-libgcc -static-libstdc++
```

La sonda de interop VKD3D consulta los handles Vulkan asociados a dos objetos D3D12:

```bash
PROTON=/ruta/a/GE-Proton/proton \
WINEPREFIX=/tmp/dlss5-vkd3d-interop-probe \
./scripts/run_vkd3d_interop_probe.sh
```

En el VKD3D experimental con `VKD3D_DUPLICATE_LUID_ADAPTERS=1`, la sonda ahora imprime identidades distintas: A `uuid=af6de4b3 pci=0:1:0.0`, B `uuid=5b9f385f pci=0:3:0.0`, y `multi_adapter_distinct=yes`. El modo es opt-in y no reemplaza el VKD3D de Proton.

La prueba oficial utilizada fue descargada desde el release público de [NVIDIA/DLSS](https://github.com/NVIDIA/DLSS/releases/tag/v310.9.1). También se descargaron localmente [DLSS5-Swapper](https://github.com/rakanki911/DLSS5-Swapper) y [dlssg_for_sm86](https://github.com/sdli1995/dlssg_for_sm86) para inspección; ambos son rutas Windows y no agregan por sí mismos un backend multi-GPU Linux.

Esas partes necesitan runtimes y contratos propietarios o inestables que deben permanecer en la instalación local del usuario.

## Compilar

Requiere CUDA Toolkit, el driver NVIDIA y headers Vulkan. En Ubuntu:

```bash
sudo apt install libvulkan-dev vulkan-tools
```

El CMakefile también busca headers en `third_party/vulkan-sdk/root`, que se usa en este workspace para no modificar el sistema.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

## Ejecutar

```bash
./build/mgpu-p2p-probe
```

Para un frame 4K RGBA16F:

```bash
./build/mgpu-p2p-probe \
  --bytes 66355200 \
  --warmup 20 \
  --iterations 200
```

Probe Vulkan → CUDA → P2P:

```bash
./build/mgpu-vulkan-cuda-probe --vulkan-gpu 0 \
  --cuda-source 0 --cuda-destination 1 \
  --bytes 8294400
```

El índice Vulkan se deduplica por UUID/PCI. En esta máquina las dos RTX 3090 aparecen como Vulkan GPU 0 y 1 después de eliminar duplicados del driver.

Probe del ring asíncrono:

```bash
./build/mgpu-p2p-ring-probe \
  --bytes 8294400 \
  --slots 3 \
  --frames 300
```

Probe del MVP CPU-gated P2P:

```bash
./build/mgpu-cpu-sync-p2p-probe \
  --bytes 8294400 \
  --slots 3 \
  --frames 120 \
  --timeout-ms 5000 \
  --json
```

`READY_CUDA_NATIVE_FRAME_SYNC_P2P` agrega waits CUDA GPU→GPU a la validación
multip plano. `READY_CPU_SYNC_P2P` significa que la transferencia entre GPUs y su ordenamiento
mediado por CPU pasaron. `READY_CPU_FRAME_SYNC_P2P` agrega la validación conjunta
de color/motion/depth. Ninguno significa que DLSS/NR remoto esté conectado. El
semaphore/fence D3D12/Vulkan GPU-nativo permanece como trabajo pendiente. La
ruta CUDA tiene un probe separado:

```bash
./build/mgpu-cuda-native-sync-probe --source 0 --destination 1 \
  --bytes 8294400 --slots 3 --frames 120 --timeout-ms 5000 --json
```

`gpu_native_waits=true` sólo significa que las dependencias del transporte
CUDA se resolvieron con eventos en GPU; no habilita todavía NR remoto.

La prueba positiva de imagen se habilita junto con `fd-probe`:

```bash
MGPU_VULKAN_IMAGE_IMPORT_HELPER=/ruta/al/build/mgpu-vulkan-image-import-helper \
MGPU_CUDA_IMPORT_HELPER=/ruta/al/build/cuda_external_import_helper \
MGPU_DLSSNR_TRANSPORT=fd-probe \
./scripts/run_ngx_test.sh
```

El helper actual valida una imagen privada sintética de 1280×720; la orientación
`GPU0 → GPU1` pasa con acceso GPU real. La orientación física inversa, con el
selector experimental correctamente activado, todavía devuelve
`VK_ERROR_OUT_OF_DEVICE_MEMORY`; el buffer CUDA inverso sí pasa. Esto no afirma
que color, motion vectors y depth de un juego ya estén importados en B.

El bypass lineal se puede ejecutar automáticamente en ambas direcciones:

```bash
./build/mgpu-vulkan-image-cuda-p2p-probe 0 1
./build/mgpu-vulkan-image-cuda-p2p-probe 1 0
```

Este probe demuestra que una asignación de imagen equivalente puede viajar por
CUDA P2P. La conversión de una textura D3D12 real a buffer lineal se valida en
el smoke siguiente; todavía falta conectar ese buffer con una evaluación NGX en B.

El smoke D3D12 que valida esa conversión se ejecuta con:

```bash
VKD3D_DUPLICATE_LUID_ADAPTERS=1 \
VKD3D_DUPLICATE_LUID_INDEX=0 \
MGPU_CUDA_SOURCE_ORDINAL=0 MGPU_CUDA_DESTINATION_ORDINAL=1 \
PROTON=/ruta/a/GE-Proton/proton \
VKD3D_DLL_DIR=/ruta/al/proton-patched \
./scripts/run_d3d12_texture_linear_smoke.sh
```

Para probar la orientación inversa se usan `VKD3D_DUPLICATE_LUID_INDEX=1`,
`MGPU_CUDA_SOURCE_ORDINAL=1` y `MGPU_CUDA_DESTINATION_ORDINAL=0`.
La espera del fence es CPU explícita y sigue siendo un fallback de laboratorio;
el fence GPU-nativo D3D12/Vulkan continúa pendiente.

El smoke NGX también cierra y envía el command list, espera una fence D3D12 desde
CPU y copia el output a un readback. Con `MGPU_NGX_OUTPUT_VARIANT=2` fijo, la
variante de entrada 0 produjo `fnv1a=0x3a300cd59e971a6f` y la variante 1
`fnv1a=0xe5da35ab3b4b797b`, partiendo ambas del mismo baseline
`0x096af4a380b90383`; las dos terminaron con `EvaluateFeature=0x1` y
`DLSSNR Evaluate=0x1`. Esto demuestra sensibilidad byte-level del smoke, pero no
permite atribuir el cambio exclusivamente a NR ni equivale a calidad visual en
un juego real.

El mismo smoke acepta `MGPU_NGX_INPUT_VARIANT=0|1` y registra cuatro uploads
GPU-side más un baseline previo a `EvaluateFeature`. `MGPU_NGX_OUTPUT_VARIANT=2`
mantiene el output inicial fijo para que la comparación entre variantes mida la
respuesta a color/motion/depth. La fence usada en esta prueba es CPU explícita:
no debe confundirse con el semaphore/fence GPU-nativo D3D12/Vulkan, que continúa
pendiente.

La matriz de aislamiento por proceso ejecuta el mismo host sintético en procesos
Proton separados, con `VKD3D_DUPLICATE_LUID_INDEX=0` y `=1`. Cada proceso exige
la identidad física esperada (`0:1:0.0` para A y `0:3:0.0` para B),
`EvaluateFeature=0x1`, `DLSSNR Evaluate result=0x1` y retorno positivo:

```bash
PROTON=/ruta/a/GE-Proton/proton \
VKD3D_DLL_DIR=/ruta/al/vkd3d-experimental/bin \
DLSS_DEMO_DIR=/ruta/a/DLSS_Sample_App/bin/ngx_dlss_demo \
NGX_SDK_DIR=/ruta/a/DLSS_SDK \
DLSS_RUNTIME_DLL=/ruta/a/nvngx_dlss.dll \
NGX_BRIDGE_DIR=/ruta/al/bridge \
DLSS_NR_DLL=/ruta/a/nvngx_dlssnr.dll \
./scripts/run_ngx_process_isolation_matrix.sh
```

Este resultado demuestra NR local aislado en B, no NR remoto: todavía no hay
intercambio de color/motion/depth entre ambos procesos ni presentación desde B.

Para diagnosticar el host oficial D3D12 sin modificar su instalación:

```bash
DLSS_DEMO_DIR=/ruta/a/DLSS_Sample_App/bin/ngx_dlss_demo \
NGX_BRIDGE_DIR=/ruta/al/bridge \
DLSS_RUNTIME_DLL=/ruta/a/nvngx_dlss.dll \
DLSS_NR_DLL=/ruta/a/nvngx_dlssnr.dll \
PROTON=/ruta/a/GE-Proton/proton \
VKD3D_DLL_DIR=/ruta/al/vkd3d-experimental/bin \
./scripts/run_official_d3d12_host_probe.sh
```

El probe copia todo a un directorio temporal, conserva el prefix aislado y
devuelve JSON con `device_created`, `ngx_loaded` y `bridge_evaluated`. En este
host el resultado actual es `device_created=true`, `ngx_loaded=false`,
`bridge_evaluated=false` y timeout 124: el sample no llegó a cargar NGX antes
del watchdog. No se considera una validación de juego.

Para separar un bloqueo de assets del backend D3D12 se puede usar la escena
mínima incluida, sin contarla como prueba de juego:

```bash
MGPU_OFFICIAL_HOST_SCENE="$PWD/tests/fixtures/ngx_empty_scene.json" \
./scripts/run_official_d3d12_host_probe.sh
```

La escena mínima reproduce el mismo bloqueo después de `CreateDevice`; el
runner aísla y limpia todo el proceso-grupo Proton/Wine al vencer el watchdog.

La matriz automatizada ejecuta ambos sentidos y exige exportación, importación
CUDA y readback válidos:

```bash
PROTON=/ruta/a/GE-Proton/proton \
VKD3D_DLL_DIR=/ruta/al/proton-patched \
./scripts/run_d3d12_texture_linear_matrix.sh
```

`run_ngx_test.sh` limpia sus copias temporales de bridge y logs al salir. Para
inspeccionar esos archivos después de una corrida se puede usar
`MGPU_NGX_KEEP_TEMP=1`; los prefixes y runtimes proporcionados por el usuario
no son eliminados por el launcher.

Probe experimental de frame multip plano:

```bash
./build/mgpu-cpu-sync-frame-probe \
  --color-bytes 8294400 \
  --motion-bytes 8294400 \
  --depth-bytes 4147200 \
  --slots 3 --frames 120 --timeout-ms 5000 --json
```

Este probe transporta tres buffers CUDA asociados al mismo `frame_id`; simula
la forma del paquete color/motion/depth, pero todavía no consume imágenes
producidas por un juego ni ejecuta NGX en GPU B.

MVP automático de diagnóstico y selección:

```bash
./scripts/mgpu-auto doctor
./scripts/mgpu-auto selftest --json
./scripts/mgpu-auto plan --json
./scripts/mgpu-auto games
./scripts/mgpu-auto plan --game <appid-o-nombre> --write-profile --json
./scripts/mgpu-auto run --game <appid-o-nombre> --dry-run --json
./scripts/mgpu-auto run --exe /ruta/al/Test.exe --prefix /tmp/test-prefix \
  --timeout-seconds 30 --json
```

`run --dry-run` todavía no inicia juegos: genera la política y conserva fallback local. El modo remoto sólo podrá quedar en `READY_REMOTE` cuando, además de P2P, interop Vulkan/CUDA, memoria y runtimes, exista un backend de transporte cross-adapter explícitamente habilitado. Actualmente ese gate permanece cerrado, por lo que no se promete una ruta remota aunque estén presentes las DLLs.

El último chequeo de RandR de esta sesión se hizo sólo en modo lectura y reportó dos salidas conectadas (`DP-0` y `HDMI-1-0`); `DP-1-3` apareció desconectada. No se ejecutaron comandos de configuración de monitores ni se reinició Xorg.

El controlador automático ya está implementado para diagnóstico, descubrimiento Steam/Proton, selección de GPUs, self-tests, perfiles TOML y fallback. Consume JSON de los probes y no parsea texto humano de `nvidia-smi` como fuente principal de verdad. La ejecución automática de un juego sigue en modo seguro: requiere convertir primero esta prueba de host en un launcher por juego con rollback.

`--exe` permite probar un ejecutable Windows aislado cuando Steam no está instalado. Fija `VKD3D_VULKAN_DEVICE`, `VKD3D_FILTER_DEVICE_NAME` y `DXVK_FILTER_DEVICE_NAME` para la GPU de render elegida; el watchdog termina el proceso si supera `--timeout-seconds`. Este camino todavía usa fallback local: no pretende ejecutar NGX remoto sin bridge.

Cuando `--runner` apunta a un binario llamado `proton`, el lanzador usa automáticamente `proton run <exe>` y configura `STEAM_COMPAT_DATA_PATH`, `STEAM_COMPAT_CLIENT_INSTALL_PATH`, `UMU_ID` y `UMU_USE_STEAM=0`. Esta ruta fue probada con GE-Proton 11-6; una ejecución de 8 s arrancó correctamente y fue terminada por el watchdog.

El bridge NGX opcional se construye sin incluir runtimes propietarios:

```bash
export DLSS5_BRIDGE_SOURCE=/ruta/a/dlss5-linux-bridge
export NGX_SDK_DIR=/ruta/a/headers-del-DLSS-SDK
./scripts/build_bridge.sh
```

Para inspeccionar desde el propio bridge los recursos D3D12 que llegan a una
evaluación, existe un hook opt-in. No mueve memoria ni activa un modo remoto;
registra los handles Vulkan, offsets y layouts expuestos por VKD3D-Proton:

```bash
NGX_SDK_DIR=/ruta/a/headers-del-DLSS-SDK \
DLSS5_BRIDGE_SOURCE=/ruta/a/dlss5-linux-bridge \
OUT_DIR=/tmp/dlss5-bridge-transport-probe \
./scripts/build_bridge_transport_probe.sh

NGX_BRIDGE_DIR=/tmp/dlss5-bridge-transport-probe \
MGPU_DLSSNR_TRANSPORT=probe \
DLSS_DEMO_DIR=/ruta/a/DLSS_Sample_App/bin/ngx_dlss_demo \
NGX_SDK_DIR=/ruta/a/DLSS \
PROTON=/ruta/a/GE-Proton/proton \
DLSS_NR_DLL=/ruta/a/nvngx_dlssnr.dll \
./scripts/run_ngx_test.sh
```

El log `dlssnr-proxy.log` debe mostrar `transport_probe handles` y una línea
`transport_probe resource=...` por cada recurso disponible. Esto confirma el
punto de integración del host, pero no implica todavía que el recurso llegue a
la segunda 3090: faltan exportación/importación dentro del proceso y
sincronización.

Para probar el nuevo MVP automático de transporte desde el bridge:

```bash
NGX_SDK_DIR=/ruta/a/DLSS \
DLSS5_BRIDGE_SOURCE=/ruta/a/dlss5-linux-bridge \
OUT_DIR=/tmp/dlss5-bridge-fd-probe \
./scripts/build_bridge_transport_probe.sh

NGX_BRIDGE_DIR=/tmp/dlss5-bridge-fd-probe \
MGPU_DLSSNR_TRANSPORT=fd-probe \
MGPU_CUDA_IMPORT_HELPER=/ruta/al/build/cuda_external_import_helper \
DLSS_DEMO_DIR=/ruta/a/DLSS_Sample_App/bin/ngx_dlss_demo \
NGX_SDK_DIR=/ruta/a/DLSS \
PROTON=/ruta/a/GE-Proton/proton \
DLSS_NR_DLL=/ruta/a/nvngx_dlssnr.dll \
VKD3D_DLL_DIR=/tmp/dlss5-vkd3d-install/bin \
./scripts/run_ngx_test.sh
```

El modo `fd-probe` crea un heap colocado para el output, exporta el FD desde
VKD3D y valida importación/mapeo/escritura/copia P2P hacia GPU B. Es un gate de
transporte: no activa NR remoto, no presenta en la segunda GPU y no habilita
MFG. `vkGetMemoryFdPropertiesKHR` todavía puede informar `VK_ERROR_UNKNOWN`
bajo el thunk Vulkan de Wine; el helper se valida mediante el FD heredado y el
shim POSIX acotado al proceso de prueba.

El resultado queda en `build/proton/`. Para pasar a `READY_REMOTE` todavía deben existir, dentro del prefix/juego, las DLLs NGX compatibles proporcionadas por el usuario: `_nvngx_real.dll`, `nvngx_dlss_real.dll` y `nvngx_dlssnr.dll`.

Probe combinado CPU-gated A→B→NGX(B):

```bash
NGX_SDK_DIR=/ruta/a/DLSS \
DLSS_DEMO_DIR=/ruta/a/ngx_dlss_demo \
DLSS_RUNTIME_DLL=/ruta/a/nvngx_dlss.dll \
DLSS_NR_DLL=/ruta/a/nvngx_dlssnr.dll \
NGX_BRIDGE_DIR=/ruta/al/bridge \
PROTON=/ruta/a/GE-Proton/proton \
VKD3D_DLL_DIR=/ruta/a/vkd3d \
./scripts/run_d3d12_cross_adapter_frame_probe.sh
```

Para validar sólo el transporte, usar `MGPU_NGX_CROSS_ADAPTER=0`. El modo combinado sigue siendo sintético y no habilita `READY_REMOTE` ni MFG.

Una vez configuradas esas variables, el mismo gate puede ejecutarse automáticamente:

```bash
./scripts/mgpu-auto remote-selftest --json
```

Por defecto prueba A→B. Para exigir automáticamente ambas orientaciones, usar `MGPU_REMOTE_DIRECTIONS=both`; no inicia un juego ni cambia `READY_REMOTE`.

Salida JSON:

```bash
./build/mgpu-p2p-probe --json > p2p-report.json
```

## Diagnóstico del sistema

Antes del benchmark:

```bash
nvidia-smi
nvidia-smi topo -m
```

El resultado importante es que ambas GPU tengan `cudaDeviceCanAccessPeer == true` y que el ancho de banda medido sea superior al camino host-staging.

## Próxima integración

El transporte está aislado en `include/mgpu/p2p_transport.hpp`. La siguiente capa debe:

1. Exportar un buffer lineal desde Vulkan/VKD3D.
2. Importarlo o conectarlo al transporte CUDA P2P.
3. Crear un recurso local en la GPU B.
4. Ejecutar el bridge NGX/DLSS-NR provisto localmente por el usuario.
5. Presentar el resultado en la GPU B.

La integración se debe probar primero en un juego D3D12 bajo Proton, sin Frame Generation.
