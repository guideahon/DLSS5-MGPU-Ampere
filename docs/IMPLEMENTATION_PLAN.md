# Plan completo de implementación — Dual RTX 3090 / DLSS5 en Linux

## Auditoría de avance — 2026-09-11 — wiring DXVK del host NGX

- [x] Añadir `MGPU_DXVK_DIR` y `MGPU_DXVK_NVAPI_DIR` a
  `run_ngx_test.sh`, validando y copiando `dxgi.dll`/NVAPI cuando el usuario
  proporciona un build compatible.
- [x] Forzar los overrides nativos coherentes para `dxgi`, `d3d12` y
  `d3d12core`, y registrar una regresión del wiring; la suite queda en 30/30.
- [x] Ejecutar una prueba directa acotada con el conjunto experimental:
  `dxgi.dll` cargó y enumeró cuatro entradas RTX 3090, confirmando que el
  selector PCI ya no cae en el adapter falso GTX 470.
- [ ] Completar la inicialización DXVK y la corrida host→bridge: la prueba
  directa termina en `Failed to initialize DXVK`, porque no incluye la pila
  completa de `winevulkan`/Proton; no se declara evaluación NGX válida.
- [ ] La copia GE-Proton recuperada sólo como fuente de prueba tampoco es
  ejecutable en este sistema: aborta en funciones Win32U no implementadas
  (`NtUserInitializeTouchInjection`, `SHGetFolderPathW`, `CoInitialize`). Se
  limpió su prefix temporal y no se toma como runtime válido.

## Auditoría de avance — 2026-09-11 — propagación del selector PCI

- [x] Propagar `MGPU_NGX_PRIMARY_PCI` desde `run_ngx_test.sh` y
  `run_official_d3d12_host_probe.sh` hasta el proceso Wine/Proton que ejecuta
  `ngx_d3d12_smoke.exe`.
- [x] Añadir regresión estática para ambos runners; la suite queda en 29/29 y
  todos los scripts pasan `bash -n`.
- [ ] Repetir la ejecución host→bridge con `0:3:0.0`: sigue pendiente porque
  falta una corrida completa del demo/Proton en este entorno y la ejecución
  previa se bloqueó antes del primer log.

## Auditoría de mantenimiento — 2026-09-11 — limpieza de artefactos

- [x] Eliminar los directorios temporales exactos `/tmp/dlss5-*` generados por
  las compilaciones y harnesses de esta investigación; no se tocaron otros
  temporales del sistema.
- [x] Verificar que `/home/cristian/Juegos` quedó vacío y que no hay directorios
  `dlss5-*` restantes en `/tmp`; el espacio libre pasó aproximadamente de
  80 GiB a 84 GiB.

## Auditoría de avance — 2026-09-11 — regresión de transporte y CUDA nativo

- [x] Repetir el probe Vulkan→CUDA→P2P en ambas orientaciones físicas:
  A→B y B→A devuelven `validation=ok`, con las UUID/PCI de las dos RTX 3090
  correctamente emparejadas.
- [x] Repetir el frame-loop CPU-gated de tres planos durante 120 frames:
  `completed=120/120`, `validation_passed=true`, `peer_enabled=true`.
- [x] Repetir la sincronización GPU-nativa CUDA↔CUDA en ambas orientaciones:
  A→B `120/120`, `gpu_native_waits=true`, ~4.98 GB/s; B→A `120/120`,
  `gpu_native_waits=true`, ~11.58 GB/s.
- [x] Ejecutar `./scripts/mgpu-auto selftest --json`: `passed=true`, P2P
  bidireccional (~11.9/12.7 GB/s), interop Vulkan↔CUDA en ambas direcciones,
  semáforos externos Vulkan↔CUDA, imágenes CUDA P2P y sincronización CUDA
  nativa 120/120.
- [ ] No promover estos resultados al gate GPU-native D3D12: el productor
  D3D12/VKD3D todavía no exporta la fence externa en el host GE-Proton
  (`E_NOTIMPL`). El éxito CUDA↔CUDA sólo valida el transporte y la espera
  nativa dentro de CUDA.

## Auditoría de avance — 2026-09-11 — selección física PCI en el host NGX

- [x] Añadir `MGPU_NGX_PRIMARY_PCI=dominio:bus:device.function` al host
  `tests/ngx_d3d12_smoke.cpp`. Durante la enumeración DXGI, el host crea una
  sonda D3D12, consulta `GetVulkanPhysicalDeviceIdentity` y sólo selecciona el
  adapter cuyo PCI coincide; no depende del orden DXGI ni del LUID duplicado.
- [x] Mantener el fallback existente cuando la variable no está definida y
  rechazar de forma explícita una especificación PCI inválida.
- [x] Compilar el host modificado con MinGW y verificar sintaxis/regresión del
  repositorio.
- [ ] Validar aún la ejecución host→bridge con `MGPU_NGX_PRIMARY_PCI=0:3:0.0`:
  el harness Wine experimental se bloqueó antes de crear
  `ngx_d3d12_smoke.result.txt` o `dlssnr-proxy.log`, incluso copiando
  `winex11.drv`; se detuvo por timeout y no se toma como evidencia positiva.
- [ ] No confundir este selector con integración de juego: el host sigue siendo
  sintético y GPU-native continúa pendiente explícitamente.

## Auditoría de avance — 2026-09-11 — MVP remoto automático CPU-gated en ambas orientaciones

- [x] Corregir la propagación del helper Linux en los runners: el bridge
  consume `MGPU_CUDA_WORKER_HELPER`, mientras que el runner sólo exponía
  `MGPU_CUDA_P2P_COPY_HELPER`. Ambos runners ahora rellenan el primero con el
  helper P2P si el usuario no lo define; antes el pair-worker retornaba sin
  siquiera registrar diagnóstico.
- [x] Hacer que `mgpu-auto` fuerce `MGPU_NGX_PRIME_SOURCE=0` para los perfiles
  `remote-ngx`: NGX conserva estado global y el orden A-first hace que el
  segundo `CreateFeature` devuelva `0xbad00007`.
- [x] Ejecutar `mgpu-auto remote-selftest` real con
  `MGPU_REMOTE_TRANSPORT=resource-fd-pair-worker-remote-ngx`,
  `MGPU_REMOTE_DIRECTIONS=both` y `MGPU_CROSS_ADAPTER_GPU_NATIVE=0` sobre las
  dos RTX 3090. A→B y B→A devolvieron `returncode=0`, resource-FD y readback
  de planos correctos, `remote_ngx_evaluate=true`, submit/fence CPU completo,
  `output_returned=true` y `output_validation=true`.
- [x] Confirmar en el log autoritativo del bridge, por corrida, la secuencia
  `remote_ngx_init=0x1`, `remote_ngx_create=0x1`,
  `remote_ngx_evaluate=0x1`, `remote_ngx_submit=0x0` con
  `device_removed=0`, y retorno FNV no nulo. Esto cierra el MVP remoto
  sintético CPU-gated, no una integración con un juego.
- [x] Repetir el perfil persistente automático con tres frames en A→B y B→A:
  `ngx_b_frames_completed=3`, retorno y validación de output positivos en las
  dos orientaciones.
- [x] Repetir el perfil `resource-fd-pair-worker-sequential-dual` en A→B y
  B→A: además del pass remoto, `local_after_remote_init/create/evaluate=true`
  en ambas direcciones. Esto es secuencial, no simultáneo.
- [x] Probar el sink de presentación sintético con tres frames. El intento
  A→B reintentó B→A automáticamente y obtuvo `Present=0x0` y
  `presentation_frames_presented=3/3`; `xrandr` mantuvo `DP-0` y `HDMI-1-0`
  conectados, sin cambios de configuración.
- [x] Añadir regresión unitaria para el orden B-first y conservar la suite en
  27/27; `bash -n` de los runners también pasa.
- [x] Hacer que el runner Wine cree y limpie su propio prefix/salida temporal
  cuando no se pasan `WINEPREFIX`/`OUT_DIR`; una prueba de fallo temprano dejó
  cero entradas nuevas en `/tmp`. Los paths explícitos del usuario no se
  eliminan.
- [x] Permitir que `mgpu-auto remote-selftest` use un `MGPU_NGX_CORE_DLL`
  explícito sin exigir `DLSS_DEMO_DIR`; el demo sólo se necesita para bootstrap
  automático del core. La suite queda en 28/28.
- [ ] Mantener GPU-native explícitamente pendiente: el mismo host GE-Proton
  devuelve `ExportVulkanFenceFd=E_NOTIMPL` cuando se solicita el modo nativo;
  el MVP validado usa timeout y coordinación CPU.
- [ ] Conectar este camino a inputs auténticos de un juego/host DLSS y medir
  presentación visual, latencia y frametime; el smoke actual sigue siendo
  sintético.

## Auditoría de avance — 2026-09-11 — importación cross-adapter de recursos D3D12

- [x] Exponer `VK_KHR_external_memory_fd` desde el `winevulkan.dll` experimental
  y aceptar `VkImportMemoryFdInfoKHR` en `win32u`; el build reproducible aplica
  `patches/winevulkan-expose-external-memory-fd.patch` y
  `patches/wine-win32u-import-memory-fd.patch`.
- [x] Adaptar la SPI actual de VKD3D-Proton con
  `ID3D12DeviceExt6::CreateResourceFromExternalFd` y hacer que el camino PE de
  Wine use FD OPAQUE aunque compile con `_WIN32`; el parche reproducible es
  `patches/vkd3d-import-resource-fd-current.patch`.
- [x] Exportar un recurso D3D12 real de GPU A: FD válido, `offset=0`,
  `size=16384`; el readback de control en A confirma datos no nulos.
- [x] Crear/importar el recurso en GPU B y copiarlo a un readback de B; la
  creación estructural y el binding Vulkan terminan en éxito.
- [ ] Obtener contenido visible en B: el readback remoto termina con
  `nonzero=0`. El FD OPAQUE del driver NVIDIA permite importar/bindear, pero no
  demuestra aliasing/visibilidad P2P entre las dos físicas.
- [x] Validar el transporte explícito operativo
  `D3D12 resource-FD → CUDA → cuMemcpyPeer → D3D12 B`: tres planos, tres
  iteraciones persistentes y frame-loop CPU-gated de 3/3 frames pasaron en
  A→B y B→A; `EvaluateFeature=0x1`, readback NGX no nulo y
  `frame_loop_payload_varied=true`.
- [ ] Resolver el aliasing Vulkan directo entre físicos; queda sólo como
  experimento de bajo nivel, no como dependencia del MVP.
- [ ] Reemplazar la espera CPU del MVP remoto por sincronización GPU-native
  integrada; sigue pendiente de forma explícita aunque los smokes aislados y el
  frame-loop sintético persistente ya pasen.
- [x] Implementar el probe opcional `D3D12 fence A → CUDA external semaphore B`:
  crea una fence dedicada, arranca un helper CUDA bloqueado en
  `cuWaitExternalSemaphoresAsync`, señaliza desde D3D12 y exige la finalización
  sin staging por CPU. El helper usa un gate/timeout y el launcher es asíncrono.
- [x] Validar ese contrato con Wine/VKD3D completo que expone
  `VK_KHR_external_semaphore_fd`: pasaron `cuImportExternalSemaphore`,
  `cuWaitExternalSemaphoresAsync` y `cuStreamSynchronize` tanto con
  `ID3D12Fence::Signal` como con `ID3D12CommandQueue::Signal` en GPU A hacia
  CUDA ordinal 1 (GPU B). El mismo smoke mantiene el roundtrip D3D12→Vulkan.
- [x] Añadir relay opt-in `D3D12 A → CUDA wait/signal en B → D3D12 B`: el
  helper importa dos fences, ordena wait y signal en el mismo CUDA stream y el
  smoke observa `GetCompletedValue(B)>=1`.
- [x] Validar un frame-loop sintético persistente de recursos reales: D3D12 A
  produce un buffer, CUDA B mantiene contextos/imports/stream vivos, espera la
  fence del slot, ejecuta `cuMemcpyPeerAsync` sobre los resource-FD exportados,
  señaliza la fence del slot B y D3D12 B hace el wait más readback. Pasaron
  3/3 frames consecutivos en el harness Wine/VKD3D experimental.
- [x] Repetir ese worker persistente en las dos orientaciones físicas: A→B y
  B→A pasaron 3/3 frames, con `VKD3D_DUPLICATE_LUID_INDEX` y los ordinals CUDA
  invertidos de forma consistente.
- [x] Integrar el worker GPU-native opt-in en el smoke real de tres planos
  (`color/motion/depth`): `D3D12 A → fences por slot → CUDA B/P2P → fence B →
  D3D12 B`. El runner Wine experimental pasó 3/3 en A→B y B→A con readback
  válido y `gpu_native_sync_success=true`.
- [x] Hacer explícita la composición del runner NGX: valida y copia proxy,
  bridge, core NGX, runtime DLSS y runtime NR por separado; permite indicar
  cada artefacto con variables de entorno y registra un fallo de inicialización
  sin confundirlo con un fallo del transporte.
- [x] Reproducir el orden de inicialización del bridge: el smoke admite
  `MGPU_NGX_PRIME_SOURCE=1` y registra por separado el resultado de `Init_Ext`
  en A antes de intentar B. En el stack experimental ambos retornan
  `0xbad00002`, así que el orden no es suficiente.
- [x] Añadir al runner un mecanismo aislado para copiar DLLs de compatibilidad
  NVIDIA al prefix temporal (`MGPU_NGX_COMPAT_DLL_DIR`); `nvapi64.dll`,
  `nvml.dll` y `nvofapi64.dll` no cambiaron el resultado.
- [x] Añadir preload opt-in (`MGPU_NGX_PRELOAD_COMPAT=1`) para distinguir
  “DLL visible” de “DLL cargada antes de NGX”. `nvapi64`/`nvofapi64` cargan,
  `nvml` falla con `ERROR_DLL_INIT_FAILED`, y `Init_Ext` sigue en
  `0xbad00002`.
- [x] Añadir `MGPU_NGX_COMPAT_UNIX_DIR` para suministrar el `nvml.so` Unix del
  mismo paquete junto al wrapper PE. La carga sigue fallando con
  `ERROR_DLL_INIT_FAILED`, evidencia de incompatibilidad ABI del wrapper con
  el Wine experimental; no se toca el sistema.
- [x] Añadir `tests/nvapi_ngx_compat_stub.c` y su builder reproducible como
  diagnóstico aislado del ABI NVAPI. El stub completa las versiones reales de
  `NV_GPU_ARCH_INFO`/`NV_LOGICAL_GPU_DATA`, arquitectura GA102, handle lógico,
  versión del driver y handles DRS; con él NGX supera las consultas iniciales,
  pero `Init_Ext` continúa en `0xbad00001` (`FeatureNotSupported`). La variante
  opt-in AD100 tampoco habilita el core, por lo que esto no es un bypass ni un
  soporte DLSS5 para Ampere.
- [x] Integrar opcionalmente DXVK x64 y DXVK-NVAPI real en el runner. Con
  `MGPU_DXVK_DIR`, `MGPU_DXVK_NVAPI_DIR` y `DXVK_CONFIG` NVIDIA, NVAPI real
  inicializa (`0x1`) y B-first completa DLSS estándar→DLSSNR con
  `Init/Create/Evaluate=0x1` y readback no nulo después de un transporte A→B
  de 3/3 frames. A-first sigue dejando el segundo device en
  `CreateFeature=0xbad00007` por estado global NGX.
- [ ] Completar la combinación multi-device GPU-native + NGX en orden A-first:
  con DXVK-NVAPI real el transporte de tres planos pasa 3/3 y `Init_Ext=0x1`,
  pero el segundo device devuelve `CreateFeature=0xbad00007`. El camino
  B-first sí evalúa localmente DLSS→DLSSNR; aún no es NR remoto ni un frame de
  juego real.
- [ ] Probar la misma combinación dentro de una distribución GE-Proton
  ejecutable y coherente. La instalación disponible en Trash no pudo completar
  el bootstrap: aborta en funciones `win32u` no implementadas, por lo que no
  se la usa como evidencia positiva.
- [x] Verificar el experimento de identidad sin LUID duplicado: no es una
  solución; VKD3D termina seleccionando la misma física para ambos devices y el
  import CUDA/P2P falla con `CUDA_ERROR_UNKNOWN`.
- [ ] Integrar esa sincronización en el ring de imágenes/recursos persistentes
  del MVP remoto y con un productor/consumidor real de NR. El contrato aislado
  de fence y el loop persistente de tres planos ya pasan, pero todavía no
  reemplazan el gate CPU del MVP remoto ni prueban un frame producido por un
  juego/NR real.
- [ ] Repetirlo dentro de GE-Proton/Proton distribuido: su `winevulkan` builtin
  sigue devolviendo `ExportVulkanFenceFd=E_NOTIMPL`; el runtime experimental
  completo queda como harness de laboratorio y no se activa globalmente.

## Auditoría de avance — 2026-09-11 — fence cross-adapter D3D12→Vulkan

- [x] Añadir `tests/vkd3d_cross_adapter_fence_smoke.cpp` para crear dos
  `ID3D12Device` bajo `VKD3D_DUPLICATE_LUID_ADAPTERS=1`, inspeccionar sus
  `VkPhysicalDevice`/UUID/PCI y rechazar una selección físicamente duplicada.
- [x] Validar en dos RTX 3090 físicas distintas que la fence compartida creada
  en A se exporta como FD OPAQUE, se importa como semáforo timeline Vulkan en B,
  observa `counter 0→1` después de `ID3D12Fence::Signal(1)` y completa
  `vkWaitSemaphores` en B. Resultado autoritativo:
  `cross_adapter_fence_roundtrip=pass`.
- [x] Hacer reproducible el runner aislado con un prefix temporal y módulos
  Wine PE compatibles (`cryptbase.dll` y `winex11.drv`), evitando el falso
  fallo previo de `SystemFunction036`/`nodrv_CreateWindow`.
- [ ] Extender el contrato a colas, recursos/imágenes y ownership/layout entre
  adapters; esta prueba sólo cierra la señalización de fence.
- [ ] Conectar el transporte a un recurso producido por un juego real y
  reemplazar el gate CPU por un ring GPU-native; el MVP remoto sigue
  deliberadamente CPU-gated.

## Auditoría de avance — 2026-09-11 — Wine Vulkan exporta semáforos FD

- [x] Identificar que Wine genera los entry points de
  `VK_KHR_external_semaphore_fd`, pero los excluye de la lista de extensiones
  expuestas a aplicaciones Win32.
- [x] Añadir el parche reproducible
  `patches/winevulkan-expose-external-semaphore-fd.patch` y compilar una copia
  experimental de `winevulkan.dll`/`winevulkan.so` fuera de Proton.
- [x] Verificar que GE-Proton continúa cargando su `winevulkan` como
  `builtin`; la prueba autoritativa sigue mostrando
  `external_semaphore_fd_spec=0`, `proc=null` y `ExportVulkanFenceFd=0x80004001`.
- [x] Compilar un Wine completo coherente con el parche, incluyendo loader,
  wineserver y módulos PE/Unix, en `/tmp/dlss5-wine-build2`; el prefix de
  validación se mantuvo separado del Wine/Proton normal.
- [x] Añadir `tests/wine_vulkan_external_semaphore_probe.cpp` y su runner para
  verificar el contrato real: `VkDevice` con la extensión habilitada,
  `vkGetSemaphoreFdKHR`, semáforo exportable y FD válido.
- [x] Validar en el host dual: cinco dispositivos Vulkan publican
  `VK_KHR_external_semaphore_fd`; `vkCreateSemaphore=0`,
  `vkGetSemaphoreFdKHR=0` y FD OPAQUE válido. El sexto dispositivo virtual no
  anuncia la extensión y se descarta.
- [x] Compilar VKD3D-Proton 3.1 con la SPI de fence y el diagnóstico de
  capacidad, usando un checkout coherente y submódulos inicializados.
- [x] Validar una fence D3D12 real con ese runtime: `CreateDevice`, fence
  compartida, `ID3D12DXVKInteropDevice5`, exportación FD `S_OK`, importación
  como semáforo Vulkan timeline, señal D3D12 a 1 y `vkWaitSemaphores(1)` con
  `VK_SUCCESS`. El runner reproducible es
  `scripts/run_vkd3d_fence_fd_smoke.sh`.
- [x] Repetir el contrato de fence entre dos adapters físicos mediante
  `scripts/run_vkd3d_cross_adapter_fence_smoke.sh`; la identidad UUID/PCI
  verificada es distinta y el roundtrip A→B pasa.
- [ ] Repetirlo con semáforos/fences asociados a colas y recursos de ambos
  adapters físicos; la corrida actual prueba señalización, no el ring remoto
  completo.
- [ ] Integrar el cambio en un Proton completo que el juego realmente use;
  GE-Proton sigue resolviendo `winevulkan` como `builtin` y no se modifica la
  instalación normal.
- [ ] Mantener pendiente la sincronización GPU-native del MVP remoto hasta
  completar el check cross-adapter de colas/recursos y un juego real; el MVP
  CPU-gated sigue siendo el camino operativo.

## Auditoría de avance — 2026-09-11

### Semáforos externos nativos Vulkan↔CUDA

- [x] Añadir un probe nativo que exporta/importa semáforos Vulkan mediante
  `VK_KHR_external_semaphore_fd` y CUDA external semaphores.
- [x] Validar `Vulkan → CUDA` y `CUDA → Vulkan` en las dos orientaciones
  físicas (`Vulkan 0/CUDA 1` y `Vulkan 1/CUDA 0`): ambas pasan en las RTX 3090.
- [x] Integrar el resultado en `mgpu-auto selftest` como capacidad independiente
  de la sincronización D3D12.
- [ ] Conectar este mecanismo con una allocation D3D12/VKD3D real: el host
  sigue devolviendo `E_NOTIMPL` para fence/semaphore externo.
- [ ] Reemplazar el gate CPU del MVP remoto por sincronización GPU-native; se
  mantiene explícitamente como pendiente hasta que VKD3D exponga ese contrato.

### MVP remoto NGX experimental — GPU B primero, CPU-gated

- [x] Inicializar el runtime NR en un segundo `ID3D12Device` con identidad
  física UUID/PCI distinta, usando el mismo runtime legalmente proporcionado
  por el usuario. La variante de DLL copiada a otro nombre/path fue probada y
  falló con `0xbad00002`; no se la usa como solución.
- [x] Crear `NVSDK_NGX_Feature_Reserved18` en el device remoto después de
  normalizar dimensiones, masks, preset, calidad y callback; la creación
  remota devuelve `0x00000001`.
- [x] Evaluar NGX en el device remoto con `DLSSNR.Color/Output/MVec/Depth`
  apuntando a allocations creadas en B. La evaluación devuelve `0x00000001`.
- [x] Ejecutar la cola remota y esperar una fence D3D12 desde CPU con timeout
  de 5 s; en A→B y B→A la fence llegó a `completed=1`, `wait=0` y
  `device_removed=0x00000000`.
- [x] Añadir un segundo worker CUDA P2P para devolver el output remoto a la
  allocation de presentación del device del juego. En ambas orientaciones el
  log autoritativo registra `output_return_copy=ok` y una respuesta `OK` para
  el buffer RGBA16F de 1280×720.
- [x] Hacer que el worker de salida de un solo par valide siempre la allocation
  destino con `cuMemcpyDtoH`, FNV-1a y conteo de bytes no nulos; el gate
  automático exige ahora `output_return_validation=ok`, no sólo una respuesta
  de socket. El modo multi-plano conserva el protocolo copy-only.
- [x] Mantener todo el camino detrás de
  `MGPU_DLSSNR_REMOTE_NGX_INIT_PROBE=1`,
  `MGPU_DLSSNR_REMOTE_NGX_FEATURE=1` y
  `MGPU_DLSSNR_SKIP_LOCAL_NGX=1`; el perfil normal no cambia.
- [x] Exponer el gate en `mgpu-auto remote-selftest` como
  `MGPU_REMOTE_TRANSPORT=resource-fd-pair-worker-remote-ngx`; el verificador
  exige JSON válido y las marcas nuevas del log sólo desde el comienzo de la
  corrida.
- [x] Añadir un modo opt-in `resource-fd-pair-worker-sequential-dual` que,
  después de completar y devolver el pass remoto, libera/apaga su feature,
  re-inicializa NR local, crea el feature local de forma diferida y verifica
  `Init/Create/Evaluate` local con cola completada. Es una secuencia dual
  CPU-gated, no simultaneidad.
- [x] Convertir el pass remoto en un ciclo multi-frame CPU-gated opt-in:
  `MGPU_DLSSNR_REMOTE_NGX_PERSISTENT=1` reutiliza el feature, resetea
  allocator/command-list después de cada fence y conserva los estados de
  recursos. El smoke ejecutó 3/3 frames con `Evaluate=0x00000001`, fences
  1/1, 2/2 y 3/3, `device_removed=0` y readback válido.
- [x] Añadir el perfil automático
  `MGPU_REMOTE_TRANSPORT=resource-fd-pair-worker-remote-ngx-persistent`;
  exige por defecto tres frames completados (`MGPU_REMOTE_NGX_FRAMES` puede
  cambiarlo) además de los gates existentes de output/FNV.
- [x] Registrar telemetría por frame en el bridge mediante QPC:
  `remote_ngx_frame_timing` informa número de frame, tiempo de evaluación más
  submit/wait CPU y resultado. Esto prepara la medición de frametime del host
  sin confundirla con un Present real.
- [x] Añadir un sink de presentación D3D12 opt-in al host sintético:
  `MGPU_CROSS_ADAPTER_PRESENT=1` crea un swapchain en el consumer, copia el
  output remoto y registra frames presentados y tiempo QPC de `Present`.
- [x] Validar tres frames remotos persistentes con presentación: el intento
  A→B reproduce `CreateSwapChainForHwnd=0x80070057` cuando el segundo device
  no puede presentar bajo VKD3D; el runner automático reintenta B→A y obtiene
  `Evaluate=0x00000001`, fences completas y `presentation_frames_presented=3/3`
  con `Present=0x00000000`. `MGPU_CROSS_ADAPTER_PRESENT_AUTO=0` conserva la
  orientación manual para diagnóstico.
- [x] Integrar ese gate en `mgpu-auto remote-selftest` de forma opt-in mediante
  `MGPU_REMOTE_PRESENT=1` y `MGPU_REMOTE_PRESENT_FRAMES=N`. La corrida real
  devuelve `available=true`, `ngx_b_frames_completed=3` y
  `presentation_frames_presented=3`, aceptando en el reporte la orientación
  efectiva elegida por el retry automático.
- [x] Añadir un fixture D3D12 de rasterización opt-in:
  `MGPU_CROSS_ADAPTER_RASTER=1` compila `VSMain`/`PSMain` con DXC, crea un
  root signature y un PSO, dibuja un triángulo en `Color` de A y sólo después
  ejecuta el transporte P2P/NGX/presentación. El resultado se mantiene como
  host de laboratorio; no se lo presenta como captura de un juego.
- [x] Exponer el fixture como gate automático opt-in mediante
  `MGPU_REMOTE_RASTER=1`; `mgpu-auto` exige que el payload confirme shader
  listo, draw enviado y readback no nulo. El modo no se activa por defecto.
- [x] Ejecutar el gate automático completo con 3 frames: raster A, transporte
  P2P, `Evaluate=0x00000001` en B, retorno validado y `Present=3/3` tras el
  retry automático de orientación. Esto no convierte el fixture en un juego.
- [x] Añadir un producer frame loop opt-in mediante
  `MGPU_CROSS_ADAPTER_FRAME_LOOP=1`: regraba color/motion/depth, espera la
  fence CPU y repite el transporte FD/P2P por frame. En A→B y B→A se validan
  3/3 frames con payload cambiante; sigue siendo un productor sintético.
- [x] Exponerlo en `mgpu-auto` con `MGPU_REMOTE_FRAME_LOOP=1` y exigir los
  contadores/payload del JSON. No se promociona a frame loop de juego.
- [x] Ejecutar la combinación completa raster + producer loop + resource-FD +
  NGX persistente + presentación automática: `available=true`, 3/3 en cada
  gate y orientación efectiva B→A. La sincronización sigue siendo CPU-gated.
- [ ] Ejecutar simultáneamente NR local y remoto. El orden local-first todavía
  provoca `device_removed=0x887a0005`; el modo secuencial evita el device loss
  liberando el estado remoto antes de iniciar A, pero no satisface este check.
- [x] Aislar el orden remoto-first sin `Shutdown`: remote
  `Evaluate/submit=0x00000001` y local `Init/Create/Evaluate=0x00000001`
  llegan a ejecutarse, pero el cierre posterior del device remoto termina en
  `device_removed=0x887a0005`. También se probó
  `MGPU_DLSSNR_SHUTDOWN_REMOTE_BEFORE_LOCAL=0` (liberar sólo el handle): falla
  igual, por lo que el conflicto no es únicamente el feature handle.
- [ ] Resolver el estado global del runtime NGX para permitir ambos devices
  vivos; la variante release-only queda disponible como diagnóstico, no como
  perfil de producción.
- [ ] Validar que el output devuelto sea el frame presentado por un juego real
  y medir frametime/latencia; el smoke actual sólo prueba un host sintético y
  el registro `dlssnr-proxy.log` del bridge.
- [ ] Reemplazar la espera CPU y el segundo worker por sincronización
  GPU-native; continúa pendiente explícitamente mientras VKD3D devuelve
  `E_NOTIMPL` para fences/semaphores externos.
- [ ] Integrar MFG/Frame Generation remoto; permanece fuera de este MVP.
- [ ] Conectar el ciclo multi-frame a recursos/presentación auténticos de un
  juego; la prueba de 3 frames sigue siendo un host sintético.
- [ ] Demostrar que el swapchain y los recursos presentados pertenecen a un
  juego real; el sink actual es deliberadamente un host de laboratorio y no
  convierte el perfil en `READY_REMOTE`.

### Bridge pair-worker con selección física por UUID/PCI

- [x] Crear desde el bridge un segundo `ID3D12Device` y allocations D3D12
  equivalentes para `color`, `output`, `motion` y `depth`.
- [x] Exportar los ocho FDs y conectarlos al daemon CUDA
  `--resource-pair-daemon` mediante `MGPU_DLSSNR_TRANSPORT=resource-fd-pair-worker`.
- [x] Detectar el caso de identidad duplicada de VKD3D: el adapter solicitado
  podía resolver al mismo UUID/PCI que el device del juego.
- [x] Probar candidatos D3D12 y seleccionar el primer UUID/PCI físico distinto;
  A→B y B→A pasaron con `resource_pair_daemon_imports_ready`, `copy copied=1`
  y cierre limpio.
- [x] Hacer reproducible el modo en el patch chain y en `mgpu-auto` como perfil
  opt-in `MGPU_REMOTE_TRANSPORT=resource-fd-pair-worker`.
- [x] Ejecutar, de forma opt-in y B-first, Init/Create/Evaluate de NGX sobre
  esas allocations remotas; la ruta se documenta con más detalle en el bloque
  `MVP remoto NGX experimental` anterior.
- [ ] Integrar recursos auténticos de un juego, presentación desde B y medición
  de frametime; no se habilita `READY_REMOTE` automáticamente.
- [ ] Sustituir la coordinación CPU por fence/semaphore GPU-native; continúa
  pendiente explícitamente mientras VKD3D devuelve `E_NOTIMPL`.

### Worker resource-FD conectado al bridge, completado como MVP CPU-gated

- [x] Añadir `--source-daemon` al helper CUDA: importa y mapea una vez los
  cuatro allocations (`color`, `output`, `motion`, `depth`), conserva los
  contextos/mappings y atiende comandos `c` (copiar) y `q` (cerrar) por
  loopback TCP.
- [x] Integrar el daemon en el bridge con el modo opt-in
  `MGPU_DLSSNR_TRANSPORT=resource-fd-worker` y separar
  `MGPU_CUDA_WORKER_HELPER` del importador individual
  `MGPU_CUDA_IMPORT_HELPER`.
- [x] Corregir la herencia de FDs: el shim acepta listas y argumentos numéricos
  del `execvp`, limpia sólo los FDs exportados y conserva `FD_CLOEXEC` en la
  tubería interna de `__wine_unix_spawnvp`.
- [x] Hacer reproducible el patch chain: checkout limpio del bridge, parche
  `resource-fd` corregido, parche `resource-fd-worker` y link explícito con
  `-lws2_32`; build de `bridge-nvngx.dll` y `_nvngx.dll` exitoso.
- [x] Ejecutar el smoke sintético A→B y B→A con el daemon: cuatro imports
  `CUDA_SUCCESS`, conexión persistente, comando `c`, respuesta `OK`, NGX en
  el consumidor `Evaluate=0x00000001`, readback no nulo y cierre sin procesos
  huérfanos.
- [x] Repetir el transporte de tres planos ocho veces en ambas direcciones
  con mappings persistentes (`persistent_worker_iterations=8`); el helper
  reportó `copy_us=10308` A→B y `10524` B→A en estas corridas. Esta repetición
  corresponde al helper de pares del smoke, no a ocho comandos del socket del
  daemon.
- [x] Añadir `MGPU_DLSSNR_WORKER_TEST_REPEAT=N` como hook de prueba del bridge:
  una sola conexión persistente atiende ocho comandos `c` y responde `OK` en
  A→B y B→A; el log registró 16/16 respuestas válidas en la corrida cruzada.
- [x] Añadir el daemon sintético `MGPU_CROSS_ADAPTER_RESOURCE_DAEMON=1`:
  importa los FDs source/destination en CUDA, escribe directamente en las
  allocations D3D12 de B y deja que NGX evalúe esas texturas; ocho comandos
  A→B y B→A pasaron con readback `fnv1a=0xf0e542b22c97a119`.
- [x] Completar el round-trip CPU-gated: exportar el output producido por NGX
  en B, copiarlo por un segundo daemon B→A a una allocation D3D12 de A y
  validarlo con readback; ambas orientaciones devolvieron `nonzero=6216988` y
  el mismo FNV `0xf0e542b22c97a119`.
- [x] Integrar el round-trip en `mgpu-auto remote-selftest` mediante
  `MGPU_REMOTE_TRANSPORT=resource-pair-daemon`; el gate exige daemon, output
  devuelto y readback no nulo, con test unitario del perfil.
- [ ] Conectar el daemon a los recursos auténticos y al frame loop de un juego;
  la prueba actual usa el host sintético/laboratorio y una evaluación por
  feature.
- [ ] Ejecutar NR remoto real sobre los recursos de juego en GPU B, devolver
  el resultado a la presentación y medir frametime de juego.
- [ ] Sustituir la coordinación CPU por semaphore/fence GPU-native; queda
  pendiente explícitamente mientras VKD3D devuelve `E_NOTIMPL`.

## Auditoría de avance — 2026-09-10

### Última iteración — bridge resource-FD real → CUDA P2P, completada en ambas direcciones

- [x] Reparar el parche del bridge para que sus hunks sean válidos y se
  apliquen desde un checkout limpio.
- [x] Añadir al helper CUDA un modo sólo lectura con `offset`, importación del
  allocation real, `cuMemcpyPeer` y comparación FNV origen/destino.
- [x] Confirmar que el bridge puede exportar `color`, `output`, `motion` y
  `depth` reales del host D3D12 y lanzar el helper nativo desde Proton.
- [x] Detectar la inversión de ordinales: el bridge se ejecuta en B, de modo
  que importa con `source=GPU_B` y copia hacia `destination=GPU_A`.
- [x] Añadir ordinales específicos opcionales
  `MGPU_CUDA_BRIDGE_SOURCE_ORDINAL`/`MGPU_CUDA_BRIDGE_DESTINATION_ORDINAL`,
  con fallback automático invertido respecto del transporte A→B.
- [x] Ejecutar A→B y B→A: los cuatro imports reales devolvieron
  `CUDA_SUCCESS`, las copias P2P y validaciones pasaron, y NGX en el
  consumidor completó `Init/Create/Evaluate=0x00000001` con readback no nulo.
- [x] Hacer que el runner oficial active `VKD3D_EXPORT_RESOURCE_FD=1` antes
  de crear recursos cuando el modo `resource-fd-probe` está seleccionado.
- [ ] Capturar recursos de un juego real sin el registro de compatibilidad del
  host de laboratorio.
- [x] Añadir modo `--pairs-repeat` al helper CUDA: importa/mapea las seis
  allocations una sola vez y repite los tres `cuMemcpyPeer` sin recrear
  contextos ni relanzar el helper por iteración.
- [x] Integrar `MGPU_CROSS_ADAPTER_PERSISTENT_FRAMES` al smoke resource-FD y
  validar ocho iteraciones A→B y B→A; ambas conservaron readback y NGX
  correctos.
- [ ] Convertir este worker de una corrida persistente en un daemon/ring
  entre frames del juego y medir frametime real; la corrida actual todavía
  incluye el coste de Proton y no recibe señales de una cola D3D12 real.
- [ ] Encadenar productor/consumidor con semaphore/fence GPU-native; este
  check queda pendiente explícitamente porque VKD3D retorna `E_NOTIMPL`.

### Iteración actual — separar el bloqueo del driver del thunk Wine/GE-Proton

- [x] Añadir `mgpu-vulkan-cross-device-fd-probe` para probar la ruta nativa sin Wine.
- [x] Deduplicar los handles Vulkan por UUID: el loader expone cuatro handles,
  pero sólo dos UUIDs físicos (`af:6d:e4:b3` y `5b:9f:38:5f`).
- [x] Confirmar con los dos UUIDs físicos reales: exportación FD `VK_SUCCESS`,
  pero importación en el device destino devuelve `VK_ERROR_OUT_OF_DEVICE_MEMORY`.
- [x] Confirmar que `vkGetMemoryFdPropertiesKHR` devuelve `VK_ERROR_UNKNOWN`;
  el driver tampoco permite completar la asignación cross-device directa.
- [x] Identificar en `win32u_vkAllocateMemory` el `Unhandled sType` para
  `VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR`.
- [x] Añadir `patches/wine-win32u-import-memory-fd.patch`, conservando el nodo
  `pNext` para que llegue al Vulkan host.
- [x] Compilar un `win32u.so` parcial con el caso FD y confirmar que no puede
  sustituir directamente al módulo GE-Proton: la ABI completa requiere sus
  patches staging/Wayland. El módulo experimental fue retirado y se restauró
  el Proton original.
- [ ] Resolver primero la importación nativa FD cross-device o mantener el
  transporte lineal CUDA/P2P como ruta operativa; luego aplicar el parche al
  árbol Wine de GE-Proton después de ejecutar su pipeline
  completo de staging y recompilar los módulos ABI relacionados.
- [ ] Repetir el probe D3D12 y obtener `vkd3d_resource_fd_imported_same_process=yes`.
- [ ] Importar/bindear en B `Color`, `MotionVectors` y `Depth` auténticos y
  ejecutar NR en B.
- [ ] Asociar sincronización real productor/consumidor; GPU-native sigue pendiente
  explícitamente mientras el host continúe devolviendo `E_NOTIMPL`.

### Iteración actual — puente D3D12 resource-FD → CUDA P2P → tres texturas D3D12

- [x] Añadir un modo opt-in `MGPU_CROSS_ADAPTER_RESOURCE_FD=1` al smoke D3D12.
- [x] Exportar las asignaciones de `Color`, `MotionVectors` y `Depth` en A y sus
  texturas equivalentes en B mediante `ID3D12DXVKInteropDevice6::ExportVulkanResourceFd`.
- [x] Importar las seis FDs en el helper CUDA y copiar cada allocation completo
  con `cuMemcpyPeer`, sin staging en RAM.
- [x] Validar A→B con readback D3D12: color `5.767.168` bytes, motion/depth
  `983.040` bytes cada uno, y pixel color `00340038003a003c` correcto.
- [x] Validar B→A con los tres planos, UUID/PCI físicos invertidos y readback
  de color, motion y depth correcto.
- [ ] Reemplazar la textura sintética por `Color`, `MotionVectors` y `Depth`
  auténticos del host/juego, respetando layout, offsets y estados de cola.
- [ ] Encadenar la copia con una fence/semaphore del productor; esta iteración
  sigue usando espera CPU y deja GPU-native explícitamente pendiente.

### Iteración actual — NGX en B sobre los tres resource-FD trasladados

- [x] Mantener `Color`, `MotionVectors` y `Depth` como texturas D3D12 nativas
  del consumidor B después de la copia P2P.
- [x] Ejecutar `Init=0x00000001`, `Create=0x00000001` y
  `Evaluate=0x00000001` en NGX sobre B usando esos tres recursos.
- [x] Validar readback NGX no nulo: `nonzero=6216988`,
  `fnv1a=0xf0e542b22c97a119`, en A→B y B→A.
- [x] Integrar el perfil automático `MGPU_REMOTE_TRANSPORT=resource-fd` en
  `mgpu-auto remote-selftest`; agrega gates de modo y readback de los tres
  planos sin cambiar el perfil lineal por defecto.
- [x] Compactar los tres `cuMemcpyPeer` en una sola invocación `--pairs` del
  helper CUDA; la transferencia medida baja a aproximadamente `0,31 s` desde
  el smoke, sin cambiar la validación byte-level.
- [x] Reutilizar imports, mappings y contextos durante múltiples copias en el
  helper persistente; el smoke midió ~10,4 ms A→B y ~11,8 ms B→A para ocho
  iteraciones de los tres planos.
- [x] Añadir un daemon CPU-gated al bridge: mantiene cuatro imports CUDA por
  feature, atiende `c/q` por loopback y se desmonta en `ReleaseFeature`/
  `Shutdown`; no se considera todavía un frame loop de juego.
- [ ] Convertirlo en worker persistente/ring conectado al productor real y
  eliminar también el spawn inicial del ciclo de juego.
- [ ] Sustituir los recursos sintéticos por los recursos auténticos capturados
  de un juego y asociar la copia a su finalización real.
- [ ] Sustituir la coordinación CPU por fence/semaphore GPU-native; continúa
  pendiente y no se promociona a `READY_REMOTE`.

### Iteración actual — exportación directa de recursos D3D12 y bind en GPU B

- [x] Añadir `ID3D12DXVKInteropDevice6::ExportVulkanResourceFd` como SPI Linux experimental y opt-in mediante `VKD3D_EXPORT_RESOURCE_FD=1`.
- [x] Marcar las asignaciones reales de recursos comprometidos con `VK_EXPORT_MEMORY_ALLOCATE_INFO`, no sólo heaps privados.
- [x] Exportar desde el host oficial los cuatro recursos nativos registrados por el shim de prueba: `color`, `output`, `motion` y `depth`.
- [x] Extender el helper Vulkan a formatos usados por el host (`R16G16B16A16_FLOAT`, `R16G16_FLOAT`, `D32/D32S8`) y modo explícito `bind-only` por argumento.
- [x] Validar en GPU B `vkBindImageMemory=VK_SUCCESS` para los cuatro recursos reales; el depth conserva `requirements_size > allocation_size`, pero el bind positivo queda registrado sin falsear acceso GPU.
- [x] Hacer reproducibles los parches VKD3D y bridge desde checkouts limpios; ambos builds cruzados completan correctamente.
- [x] Repetir el host oficial con la build limpia del patch chain: `EvaluateFeature=0x00000001`, `DLSSNR Evaluate=0x00000001` y cuatro probes de recurso exitosos.
- [ ] Ejecutar el pass NR usando esas imágenes importadas en un `ID3D12Device`/command list de GPU B; esta iteración sólo valida exportación y binding.
- [ ] Implementar sincronización GPU-native; continúa pendiente porque el host VKD3D devuelve `E_NOTIMPL` para fence/semaphore externo.
- [ ] Devolver el output producido en B a la presentación evitando retorno innecesario a A.

### Iteración actual — host oficial Donut, ABI de recursos y evaluación mínima

- [x] Hacer que el runner propague al proceso Proton los flags de traza, evaluación mínima y probes del bridge, evitando resultados que dependan de variables heredadas manualmente.
- [x] Hacer que el runner propague también la configuración completa del `fd-probe`: helper CUDA, ordinales A/B, flags de exportación, `LD_PRELOAD` acotado y ruta opcional de log del helper.
- [x] Hacer que el runner copie automáticamente los blobs `donut/shaders` desde el layout del build o desde `MGPU_OFFICIAL_HOST_SHADER_DIR`.
- [x] Detectar y copiar automáticamente `libgcc_s_seh-1.dll`, `libstdc++-6.dll` y `libwinpthread-1.dll` cuando el host fue cross-compilado con MinGW.
- [x] Corregir el runner para separar los roles de DLL: `_nvngx_real.dll` es el core generado por GE-Proton y `nvngx_dlss_real.dll` es el runtime DLSS limpio; antes se mezclaban y podían producir recursión/stack overflow.
- [x] Añadir bootstrap automático del core GE-Proton en `run_official_d3d12_host_probe.sh`, con watchdog y copia aislada del prefix.
- [x] Recompilar el sample oficial Donut como PE x86-64 desde Linux: `101/101` objetivos, incluyendo shaders, NVRHI, escena y ejecutable.
- [x] Instrumentar etapas del host y de `NGXWrapper`: `Init`, `GetCapabilityParameters`, lecturas de parámetros y creación/evaluación mínima.
- [x] Confirmar en el host oficial que `NVSDK_NGX_D3D12_Init`, `GetCapabilityParameters`, DLSS init y DLSSNR init retornan éxito; la lectura pública `SuperSampling_Available` devuelve éxito con valor `0` y el getter de fallback se bloquea, por lo que se agregó un bypass sólo diagnóstico para continuar la traza.
- [x] Hacer que el host mínimo cree cuatro recursos D3D12 NVRHI distintos y llegue a `InitializeDLSSFeatures`/`minimal_eval_begin` bajo Proton.
- [x] Identificar el límite de la ABI compacta de parámetros: el bridge no recuperaba recursos D3D12 auténticos desde el getter público.
- [x] Añadir un registro opt-in sólo al shim de compatibilidad del host de prueba (`NVSDK_NGX_Compat_GetD3D12Resource`) y el patch correspondiente del bridge; la última traza recupera cuatro punteros nativos distintos (`HDR`, `output`, `motion`, `depth`).
- [x] Hacer retornar `EvaluateFeature` en el host oficial mínimo: `DLSS standard EvaluateFeature=0x00000001` y `DLSSNR Evaluate=0x00000001`, con `minimal_eval_end` y cierre limpio.
- [x] Validar ese registro con el flujo completo de `CommonRenderPasses`/escena: el runner copia `donut/shaders`, alcanza `common_passes_ready`, carga la escena y observa `DLSSNR Evaluate=0x00000001` en frames sucesivos; el watchdog termina el host persistente de forma controlada.
- [x] Ejecutar el `fd-probe` desde el host oficial y validar el heap de output real: `ExportVulkanHeapFd hr=0x00000000`, importación CUDA, escritura, `cuMemcpyPeer` A→B y readback con `cuda_helper_p2p_validation=ok`; esto prueba transporte del output, no NR remoto.
- [x] Añadir log persistente opcional al helper CUDA (`MGPU_CUDA_HELPER_LOG`) para conservar el resultado de importación, escritura, copia P2P y checksum aunque Proton no herede el `stderr` al log del host.
- [ ] Sustituir el registro de prueba por una recuperación de recursos válida para un juego real, sin depender del shim de compatibilidad.
- [ ] Mantener GPU-native semaphore/fence como pendiente: estas pruebas siguen usando coordinación CPU y no habilitan remoto automático.

- [x] Añadir payload sintético determinista de color/motion/depth y dos variantes seleccionables al smoke NGX.
- [x] Medir baseline y output posterior con readback D3D12 y fence CPU: baseline cero, output posterior no nulo, `EvaluateFeature=0x1` en ambas variantes.
- [x] Demostrar sensibilidad sintética del output al input manteniendo fijo el seed de output: variante 0 y 1 producen hashes finales distintos.
- [x] Evitar acumulación de temporales del launcher: limpiar bridge/logs propios al salir y permitir conservarlos sólo con `MGPU_NGX_KEEP_TEMP=1`.
- [x] Aislar el host oficial D3D12 en un proceso-grupo y limpiar descendientes Wine/Proton al vencer el watchdog; la prueba devuelve `124` sin dejar procesos ni VRAM ocupada.
- [x] Añadir una escena mínima opt-in (`MGPU_OFFICIAL_HOST_SCENE`) para separar bloqueo de assets Sponza de bloqueo del host/render; no se cuenta como evaluación de juego.
- [x] Corregir el estado inicial de los buffers D3D12 `UPLOAD` a `GENERIC_READ` y parametrizar también el output de baseline por variante.
- [x] Separar la variante del payload de entrada de la variante del seed de output (`MGPU_NGX_OUTPUT_VARIANT`) para aislar sensibilidad de la evaluación.
- [x] Agregar un check opt-in de evaluación en B (`MGPU_NGX_SECOND_DEVICE_FIRST=1` + `MGPU_NGX_EVALUATE_SECOND_DEVICE=1`) con recursos D3D12 y fence CPU propios.
- [x] Validar readback del output producido en B: `nonzero=4594848`, `fnv1a=0x3c413a88d2048413`; los recursos de esta prueba son locales a B, no importados desde A.
- [x] Automatizar ese gate como `scripts/run_ngx_same_process_b_probe.sh`, incluyendo limpieza del probe y comprobación del bloqueo global esperado en A.
- [x] Implementar transporte de textura sintética A→B dentro del mismo proceso: textura/linear buffer A, dos heaps FD, `cuMemcpyPeer`, linear buffer/textura B y readback D3D12.
- [x] Añadir `mgpu-cuda-external-p2p-copy-helper` y `scripts/run_d3d12_cross_adapter_frame_probe.sh` con comparación byte-level/FNV y limpieza del prefix temporal.
- [x] Conectar la textura reconstruida en B al `Color` de un feature NGX/DLSSNR creado y evaluado sobre el device B; el smoke combinado obtuvo `EvaluateFeature=0x1` y readback no nulo.
- [x] Registrar hash FNV-1a del output NGX B y hacer que el launcher separe automáticamente core GE-Proton, runtime DLSS real y runtime NR.
- [x] Integrar el MVP combinado en `mgpu-auto remote-selftest --json`, con siete gates estrictos y fallo cerrado; no habilita lanzamiento de juegos.
- [x] Añadir `MGPU_REMOTE_DIRECTIONS=both` a `remote-selftest` para repetir y exigir automáticamente A→B y B→A, incluyendo metadatos de ordinales/dirección.
- [x] Medir el MVP combinado en A→B y B→A: transporte `~0,30–0,35 s`, cola/fence del consumidor+NGX `~17,6–18,7 ms`; el overhead restante es arranque de Proton/prefix.
- [x] Corregir el contrato del smoke sintético: `EvaluateFeature` positivo pasó a `0x00000001` después de normalizar dimensiones, jitter, motion-vector scales, subrects, exposición y reset.
- [x] Hacer reproducible el stack de parches del bridge sobre checkout limpio.
- [x] Deduplicar physical devices Vulkan por UUID/PCI y dar prioridad a la selección A/B sobre `VKD3D_VULKAN_DEVICE` en modo opt-in.
- [x] Validar identidad física distinta en el mismo proceso: A `0:1:0.0`, B `0:3:0.0`.
- [x] Validar en procesos Proton aislados que NGX/NR local funciona en A y B con la identidad UUID/PCI esperada.
- [x] Añadir SPI `ID3D12DXVKInteropDevice5` para identidad y exportación de fence FD.
- [x] Añadir probe automático de fence desde la evaluación NGX.
- [x] Auditar el stopper contra Vulkan nativo: `vulkaninfo` expone `VK_KHR_external_semaphore_fd`/`VK_KHR_external_fence_fd` en las 3090, pero la lista de extensiones visible para VKD3D bajo Proton no enumera ninguna (`external_semaphore_fd=no`, proc `vkGetSemaphoreFdKHR=null`); el intento opt-in de seleccionar `OPAQUE_FD` compila, pero no puede habilitar una extensión ausente.
- [ ] Obtener exportación/importación de semáforos externos funcional en este host; el probe devuelve `E_NOTIMPL`.
- [ ] Asociar un fence a la finalización real de la cola del juego y a la cola consumidora de B.
- [x] Transportar tres planos sintéticos (`Color`, `MotionVectors`, `Depth`) A↔B a buffers lineales del device consumidor y reconstruir sus texturas D3D12 con una fence CPU; faltan los recursos auténticos del juego.
- [x] Validar en laboratorio la importación del heap del output privado como `VkImage` en B y acceso GPU real mediante clear/copy/readback en `GPU0 → GPU1`.
- [x] Hacer que el smoke NGX cierre/envíe el command list y espere una fence D3D12 desde CPU antes del readback; positivo y negativo completan la cola, pero comparten la misma firma de salida.
- [ ] Hacer pasar la misma importación física directa como `VkImage` en `GPU1 → GPU0`; con el selector experimental correcto el driver devuelve `VK_ERROR_OUT_OF_DEVICE_MEMORY`. La ruta lineal equivalente ya pasa en ambos sentidos.
- [x] Probar `VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT` como alternativa
  Linux y una asignación importada dedicada: `vkGetMemoryFdPropertiesKHR`
  devuelve `VK_SUCCESS` pero `memoryTypeBits=0`, y ambas variantes terminan en
  `VK_ERROR_OUT_OF_DEVICE_MEMORY`; no se habilita como transporte productivo.
- [ ] Crear y evaluar el feature NGX sobre color, motion y depth auténticos importados desde el juego; el MVP actual transporta los tres planos, pero todos son sintéticos.
- [ ] Confirmar que el output B vuelve a la cadena de presentación sin retorno innecesario a A.
- [ ] Validar estabilidad, latencia y contenido visual en un host/juego D3D12 real.
- [x] Ejecutar el sample oficial Windows D3D12 en una copia Proton instrumentada durante 120 s; crea el device físico A, pero no carga NGX ni produce log del bridge y termina por watchdog.
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
| Dos adapters Vulkan en un proceso Proton | 🟡 experimental opt-in | `VKD3D_DUPLICATE_LUID_ADAPTERS=1` selecciona A/B por adapter; el modo normal sigue siendo global |
| Extracción de recurso D3D12→Vulkan | 🟡 parcial | GE-Proton expone `VkBuffer` y `VkDeviceMemory`; el buffer CUDA pasa en ambas direcciones, pero la imagen física inversa devuelve `VK_ERROR_OUT_OF_DEVICE_MEMORY` |
| VKD3D experimental con LUID duplicado | ✅ laboratorio físico | abre A=`0:1:0.0` y B=`0:3:0.0` en el mismo proceso; no es todavía una integración production-ready |
| FD D3D12/Vulkan→CUDA bajo Proton | ✅ transporte MVP | FD heredado sin `CLOEXEC`, import/map/write/`cuMemcpyPeer`/checksum correctos; `vkGetMemoryFdPropertiesKHR` sigue en `VK_ERROR_UNKNOWN` |
| SPI VKD3D para exportar heap D3D12 | ✅ opt-in | `ID3D12DXVKInteropDevice4::ExportVulkanHeapFd`; heap real de 64 KiB exportado e importado por CUDA |
| Bridge `fd-probe` automático | ✅ transporte de output validado | output colocado de 1280x720 exportado; helper CUDA importa, escribe, copia A→B y valida checksum; todavía no mueve inputs ni ejecuta NR en B |
| Bypass lineal de imagen con CUDA P2P | ✅ laboratorio | asignaciones de imagen Vulkan equivalentes, copia GPU→GPU y readback correcto en ambas direcciones |
| Textura D3D12 → buffer lineal | ✅ laboratorio | `CopyTextureRegion` con footprint real, fence CPU y pixel readback correcto en ambas orientaciones |
| Textura cross-adapter A↔B | ✅ laboratorio CPU-gated | heap FD A/B + `cuMemcpyPeer` sin staging de RAM + reconstrucción/readback D3D12 en ambos consumidores |
| Textura A↔B + NGX en consumidor | 🟡 MVP sintético | `EvaluateFeature=0x1`, output no nulo, FNV y timings registrados en ambas orientaciones; los tres planos aún son sintéticos |
| Resource-FD D3D12 → CUDA P2P → texturas D3D12 | ✅ laboratorio CPU-gated | allocations reales de color/motion/depth exportados en A/B, copia P2P y tres readbacks correctos en ambas orientaciones |
| Resource-FD + NGX en consumidor B | 🟡 MVP sintético CPU-gated | tres recursos D3D12 trasladados por FD/P2P, `EvaluateFeature=0x1` y readback NGX correcto en ambas orientaciones |
| Ejecución/readback del command list NGX | ✅ smoke host | cola/fence/readback completan en A y B-first; hay sensibilidad sintética, pero no evidencia visual de un juego |
| Payload NGX sintético y baseline | ✅ sensibilidad sintética | baseline idéntico con output fijo; variantes 0/1 producen hashes finales distintos tras `EvaluateFeature=0x1` |
| NGX sobre dos devices Vulkan distintos | 🟡 B-first únicamente | B puede evaluar localmente y leer output; A después devuelve `0xbad00007` por estado global |
| Neural Rendering en GPU A | ✅ validado hasta EvaluateFeature sintético | con runtime DLSS limpio: `Init_Ext=0x1`, `CreateFeature=0x1`, `EvaluateFeature=0x1`; todavía no es un juego real |
| Neural Rendering local en GPU B aislada | ✅ smoke sintético | proceso Proton separado, UUID/PCI `0:3:0.0`, `EvaluateFeature=0x1` y chaining DLSSNR `0x1`; no es NR remoto |
| Neural Rendering remoto en GPU B | 🟡 MVP sintético CPU-gated | color/motion/depth cruzan A↔B y NGX evalúa en el consumidor; faltan inputs reales, simultaneidad y GPU-native sync |
| Juego real con DLSS5/MFG | ⛔ no iniciado | no hay host Linux/Proton válido todavía |
| Host oficial D3D12 instrumentado | ✅ evaluación mínima + flujo alto | crea device, carga shaders/escena/CommonRenderPasses, recupera cuatro recursos nativos distintos y completa `EvaluateFeature` estándar/NR con `0x00000001`; además exporta el heap de output y pasa por CUDA/P2P; el watchdog sólo detiene el loop persistente |
| Build cruzado del host Donut desde Linux | ✅ 101/101 objetivos | Donut/NVRHI/shaders/app y `ngx_dlss_demo.exe` compilan con MinGW usando el shim opt-in; el runtime propietario no se incorpora al repositorio |
| Frame Generation remoto | ⏸ pospuesto | requiere NR estable y sincronización temporal |

## TODO con estado de ejecución

### Iteración 2026-09-10 — build cruzado de Donut y cierre del diagnóstico de alto nivel

- [x] Configurar el sample para `CMAKE_SYSTEM_NAME=Windows` con MinGW y DXC local.
- [x] Generar un import library temporal sólo para los exports de la DLL NGX disponible; no se incorpora ningún binario propietario al repositorio.
- [x] Compilar `donut_core`, `donut_engine`, `donut_render`, `donut_app`, NVRHI D3D12 y los 60 shaders DXIL.
- [x] Verificar que las capas altas `CommonRenderPasses`, `ShaderFactory`, `TextureCache`, escena y app son compilables en el entorno cruzado.
- [x] Aislar el enlace final: faltan los wrappers del SDK (`NVSDK_NGX_Parameter_*`, destroy/update y conversión de resultados), no los exports de runtime de `nvngx_dlss.dll`.
- [x] Registrar los parches reproducibles en `patches/`, sin editar el snapshot externo de `Juegos`.
- [ ] Obtener el import library oficial completo del SDK NGX o una distribución de headers+libs compatible.
- [x] Repetir el enlace y ejecutar el host recompilado bajo Proton: el runner automático devuelve `return_code=0`, `bridge_evaluated=true` y captura `EvaluateFeature` estándar/NR positivo.
- [ ] Mantener GPU-native semaphore/fence como pendiente; el host de diagnóstico sigue usando sincronización CPU y la implementación remota no se habilita automáticamente.

### Iteración 2026-09-10 — aislar la fase de ventana/swapchain

- [x] Añadir un host D3D12 mínimo que registre cada fase previa a NGX.
- [x] Ejecutarlo con Proton en proceso-grupo aislado y watchdog.
- [x] Ejecutar el smoke en este host: `CreateSwapChainForHwnd`, fence local y
  `Present` completan correctamente en GPU A; esa fase mínima no reproduce el
  bloqueo del sample oficial.
- [x] Ejecutar el perfil de swapchain equivalente al sample: factory 2,
  ventana visible, tres buffers y `DXGI_SWAP_CHAIN_FULLSCREEN_DESC`.
- [x] Reproducir la envoltura mínima de backbuffers de NVRHI (`CreateRenderTargetView`
  y clear/fence); también completa en GPU A.
- [ ] Repetir el perfil ampliado en GPU B mediante selección explícita de adapter.
- [x] Repetir el perfil ampliado en GPU B (`MGPU_D3D12_ADAPTER_INDEX=1` y
  `VKD3D_VULKAN_DEVICE=1`): factory, swapchain de 3 buffers, RTV, clear,
  fence, `Present` y carga de NGX pasan; VKD3D informa el mismo LUID lógico,
  por lo que la identidad física sigue siendo un check separado.
- [x] Repetir GPU A con ventana Win32 visible durante 5 s: completó el mismo
  smoke y `xrandr` conservó exactamente `DP-0` y `HDMI-1-0` conectados.
- [x] Reproducir la ruta GLFW/DXGI previa al device: `EnumOutputs`,
  `GetDesc` y reposicionamiento de ventana. En ambos adapters, VKD3D devuelve
  `DXGI_ERROR_NOT_FOUND` sin output; el fallback del sample (no mover la
  ventana) continúa correctamente.
- [x] Aislar la diferencia restante entre el smoke nativo y el sample:
  inicialización interna `nvrhi::d3d12::Device`, wrapping de backbuffers
  mediante `createHandleForNativeTexture` y la secuencia GLFW completa.
- [x] Reproducir `createFramebuffer` de los backbuffers y la textura de shadow
  map 2048×2048×4 creada por `CascadedShadowMap`.
- [x] Repetir la secuencia de visibilidad del sample: mostrar la ventana sólo
  después de crear NVRHI y los recursos iniciales.
- [x] Verificar por build cruzado que `CommonRenderPasses`,
  `ShaderFactory`/shaders, `TextureCache` y la app compilan; el bloqueo que
  queda no es de compilación de esas capas.
- [ ] Ejecutar esas capas dentro de un host D3D12 recompilado: el enlace aún
  requiere los wrappers oficiales `nvsdk_ngx*.lib`.
- [ ] Si el smoke pasa, instrumentar el siguiente punto del sample oficial
  entre swapchain y `LoadLibrary(nvngx_dlss.dll)`.
- [ ] Si el smoke se bloquea, corregir/aislar VKD3D-DXGI antes de seguir con
  NGX remoto.
- [ ] Mantener GPU-native semaphore/fence como pendiente; este smoke usa sólo
  fence D3D12 local para diagnóstico.

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
- [x] Evaluar sintéticamente en B-first con recursos creados en B y verificar queue/fence/readback; `EvaluateFeature=0x1`, `fnv1a=0x3c413a88d2048413`.
- [x] Verificar el camino alternativo de aislamiento: un proceso Proton dedicado a cada índice físico ejecuta NGX/NR local correctamente en A y B.
- [ ] Conectar ese device a una evaluación NR real; la prueba actual sólo crea un feature sintético.
- [x] Conectar de forma no invasiva la entrada de `EvaluateFeature` al hook de transporte y registrar handles, offsets y layouts.
- [ ] Exportar/importar ese `VkDeviceMemory` entre los dos devices y añadir sincronización de fences/semaphores.
- [x] Exponer una SPI VKD3D opt-in para exportar el heap real: `ID3D12DXVKInteropDevice4::ExportVulkanHeapFd`.
- [x] Retener el heap del output privado en el bridge y añadir `MGPU_DLSSNR_TRANSPORT=fd-probe`.
- [x] Automatizar `VKD3D_EXPORT_OPAQUE_FD_MEMORY=1`, `VKD3D_EXPORT_HEAP_FD=1` y la herencia FD sólo en el wrapper de prueba.
- [x] Validar `heap D3D12 → FD Vulkan → helper CUDA → cuMemcpyPeer → checksum` desde el hook de evaluación.
- [x] Transferir una textura sintética completa A→B usando buffers lineales y dos heaps D3D12 exportados; comparar todos los bytes antes/después.
- [x] Validar un ring CUDA-native con `cudaStreamWaitEvent` productor/consumidor: 120/120 frames y checksum correcto.
- [x] Validar representación de imagen cross-device en `GPU0 → GPU1`: heap D3D12 A → FD → `VkImage` B → clear/copy/readback, todo con `VK_SUCCESS`.
- [ ] Validar la orientación física `GPU1 → GPU0`; el buffer CUDA pasa, pero la importación como `VkImage` devuelve `VK_ERROR_OUT_OF_DEVICE_MEMORY`.
- [x] Implementar bypass lineal de asignación de imagen equivalente mediante CUDA P2P y readback Vulkan; `GPU0↔GPU1` pasa.
- [x] Implementar una ruta Vulkan/CUDA equivalente dentro del proceso para recursos cross-adapter; la API D3D12 nativa sigue bloqueada por `E_NOTIMPL`.
- [x] Validar el bypass de asignación de imagen equivalente por CUDA P2P, sin staging de RAM.
- [x] Validar textura D3D12 → buffer lineal → FD → CUDA/P2P → readback con una fence CPU acotada.
- [x] Automatizar la matriz D3D12 lineal en ambos sentidos y exigir exportación FD, importación CUDA y readback correcto.
- [x] Evitar staging de RAM en el transporte sintético A→B: el helper usa memoria externa CUDA y `cuMemcpyPeer`; las copias a host sólo son validación posterior.
- [x] Enviar y esperar el command list del smoke NGX con una fence CPU; convertir timeout/fallo de espera en error y leer el output sólo después de la finalización.
- [x] Medir bytes no nulos y FNV-1a del output NGX; registrar que la firma coincide con el host negativo y no permite atribuirla a NR.
- [x] Cargar cuatro recursos sintéticos (color, motion, depth y output) mediante upload GPU y registrar baseline/post-output con `MGPU_NGX_INPUT_VARIANT=0|1`.
- [x] Probar sensibilidad al input con `MGPU_NGX_OUTPUT_VARIANT=2` fijo: baseline `0x096af4a380b90383`; outputs `0x3a300cd59e971a6f` y `0xe5da35ab3b4b797b`.
- [ ] Guardar imágenes comparables y validar calidad visual/temporal en un host real; la firma sintética no basta para atribuir el cambio a NR.
- [ ] Permitir evaluación simultánea A+B: el orden B-first funciona sólo para B y deja A en `0xbad00007`.
- [x] Hacer que el segundo `ID3D12Device` tenga identidad física `0:3:0.0` dentro del mismo proceso, usarlo como consumidor y conectar allí los tres recursos producidos por A.
- [x] Conectar una textura reconstruida en B a un readback D3D12 posterior al transporte y al `Color` de un feature NGX/NR evaluado en B; el MVP sigue siendo sintético y CPU-gated.
- [ ] Aislar/adaptar el estado global NGX para que A y B puedan evaluar features simultáneamente.
- [ ] Sustituir el aislamiento por proceso por dos contextos cooperantes dentro de la cadena del juego, sin copiar recursos por RAM.
- [x] Evitar staging de datos por CPU en el MVP de tres planos: `cuMemcpyPeer` mueve color/motion/depth directamente entre asignaciones GPU; el CPU sólo coordina el helper y las fences.
- [x] Repetir el MVP lineal completo en la orientación inversa B→A y automatizar sus ordinales CUDA con `MGPU_CROSS_ADAPTER_REVERSE=1`; el selector fuerza el índice físico VKD3D por cada creación.
- [x] Ejecutar NGX/NR en GPU B con los tres planos sintéticos transportados y runtime compatible; falta sustituirlos por inputs auténticos de un juego.
- [ ] Mantener el monitor de salida conectado a GPU B si el frame final no vuelve a A.
- [x] Medir latencia de transferencia y cola/fence de inferencia en el MVP sintético; presentación y medición end-to-end de juego siguen pendientes.
- [ ] Comparar GPU B ocupación/VRAM contra modo local.
- [ ] Implementar device-loss y fallback local durante el arranque.
- [ ] Validar una sesión continua de 30 minutos.

Nota de la iteración del smoke: las dos variantes sintéticas llegan al upload y la evaluación devuelve éxito, pero el readback final es idéntico (`fnv1a=0xbcf8110a8e1d0383`). Por eso el check de “output escrito” queda marcado como parcial: el siguiente experimento debe distinguir una copia/fill del bridge de una inferencia sensible a color, motion y depth. La sincronización GPU-nativa D3D12/Vulkan continúa pendiente explícitamente; la fence CPU usada aquí es sólo el MVP de laboratorio.

Nota de estado: `fd-probe` ya confirma en laboratorio la importación del heap privado como una `VkImage` utilizable por B, incluido acceso GPU y readback, en la orientación `GPU0 → GPU1`. La nueva ruta lineal también reconstruye una textura D3D12 en B después de `cuMemcpyPeer`, sin staging de RAM. En la orientación física inversa la importación directa como `VkImage` sigue fallando con `VK_ERROR_OUT_OF_DEVICE_MEMORY`. El FD sale con `CLOEXEC`; el wrapper utiliza el shim POSIX sólo para el proceso de prueba. Para producción aún falta conectar color/motion/depth auténticos del juego, sincronizarlos con su cola, ejecutar NGX/NR sobre el device B y devolver/presentar el resultado.

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

- [x] Conseguir un host de laboratorio instrumentado que invoque DLSS/NR bajo Proton y capture los parámetros/recursos del host mínimo; el juego real sigue pendiente.
- [x] Repetir el host oficial con runtime limpio, traza de archivos y watchdog de proceso-grupo; el host alto se detiene antes de `common_passes_ready`, mientras el modo mínimo completa evaluación.
- [x] Aislar la escena del host con `tests/fixtures/ngx_empty_scene.json`; el mismo bloqueo demuestra que no depende de la carga Sponza.
- [x] Reproducir la evaluación local mínima en GPU A y medir su finalización; validar imagen/latencia visual del flujo alto completo sigue pendiente.
- [x] Probar la inicialización de NGX en dos objetos D3D12; la selección de adapters Vulkan distintos quedó bloqueada por VKD3D.
- [ ] Implementar la sincronización cross-adapter entre esos devices.
- [ ] Integrar el transporte P2P con recursos compartidos sin staging por CPU.
- [x] Resolver la causa inmediata de importación: el FD Vulkan tenía `FD_CLOEXEC` y no llegaba abierto al helper; el shim lo limpia sólo durante el probe.
- [x] Validar asignación D3D12/VKD3D → FD → CUDA import/map → escritura → `cuMemcpyPeer` → checksum en GPU1.
- [ ] Resolver la identidad física de GPU1 dentro de la enumeración Vulkan de VKD3D; el host duplica UUID/PCI en las entradas experimentales.
- [ ] Validar la asignación dedicada con un recurso D3D12 real del juego.
- [x] Implementar y validar el transporte básico de fence externa D3D12↔Vulkan
  con el Wine/VKD3D experimental; la señal D3D12 y la espera Vulkan pasan.
- [ ] Extender esa señalización a colas, recursos y ambos adapters físicos antes
  de reemplazar el gate CPU del frame remoto.
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
- [x] `EvaluateFeature` completa con recursos/estados de un host de laboratorio mínimo: estándar y DLSSNR devuelven `0x00000001`; el juego real sigue pendiente.
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

1. Conseguir un juego real bajo Proton que invoque el proxy durante `EvaluateFeature`; el host Donut mínimo ya lo hace, pero no sustituye un frame de juego.
2. Completar la evaluación local con recursos/estados auténticos de juego y capturar una imagen antes de mover nada a GPU B.
3. Extender la SPI de fence ya validada a recursos/colas cross-adapter en VKD3D; no inferir un backend remoto desde handles privados solamente.
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
- [x] Validar que el driver expone las capacidades necesarias cuando Wine y VKD3D
  publican `VK_KHR_external_semaphore_fd`; el fixture D3D12↔Vulkan importa y
  espera la fence correctamente.
- [ ] Integrar esta ruta en un Proton completo que use un juego y extenderla al
  transporte cross-adapter; GE-Proton distribuido sigue ocultando la extensión.

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

## Registro adicional — 2026-09-10: limpieza de artefactos locales

- [x] Confirmar que no había procesos de prueba usando los runtimes descargados.
- [x] Liberar los runtimes Proton y community-runtime, la demo 3DRenderer y la copia temporal de VKD3D.
- [x] Mantener fuentes, repositorios, headers y la DLL mínima que todavía permiten reanudar el build cruzado.
- [x] Borrar los directorios temporales creados para el cross-build y la prueba Wine una vez preservada la evidencia en la documentación.
- [x] Dejar documentado que la limpieza no altera RandR/Xorg ni el estado de las GPUs.
- [ ] Volver a descargar runtimes grandes sólo si una prueba posterior los necesita explícitamente.

## Registro adicional — 2026-09-10: shim público NGX y build Donut

- [x] Implementar un shim opt-in para los wrappers de parámetros y helpers públicos que normalmente aporta `nvsdk_ngx*.lib`.
- [x] Añadir adaptación de callback C a callback C++ para `D3D11`, `D3D12` y CUDA sin alterar el runtime cargado.
- [x] Reproducir el build completo del sample Donut con MinGW y enlazar `ngx_dlss_demo.exe`.
- [x] Confirmar que el bloqueo anterior de símbolos NGX queda superado en la etapa de enlace.
- [x] Ejecutar una prueba de arranque con Wine en prefix aislado.
- [x] Repetir el arranque con Proton/GE real usando un prefix aislado y staging automático de runtimes MinGW.
- [x] Obtener una evaluación NGX mínima con recursos nativos distintos y salida no nula; una imagen visual de un juego real sigue pendiente antes de conectar el ring CPU-gated.
- [ ] No marcar NR remoto, MFG remoto ni `READY_REMOTE` por el mero hecho de que el sample enlace.
