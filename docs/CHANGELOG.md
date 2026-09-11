# Registro técnico de cambios y pruebas

## 2026-09-11 — contexto Steam opt-in y habilitación NVAPI de Proton

- `mgpu_auto` ahora conserva, cuando el usuario las define, las variables
  `SteamAppId`, `SteamGameId`, `SteamClientLaunch`, `SteamOverlayGameId`,
  `PROTON_LOG` y `PROTON_LOG_DIR` al construir la política remota directa.
  No inicia Steam ni reemplaza los valores de UMU/compat-data.
- El modo remoto establece `PROTON_ENABLE_NVAPI=1` salvo que el usuario lo
  desactive explícitamente. Sin esta variable algunos juegos pueden cargar
  Streamline pero no exponer DLSS a través de Proton.
- La regresión queda en `55/55`.
- Auditoría de títulos instalados: No Man's Sky cargó Vulkan y
  `sl.interposer.dll` en una corrida, pero no `sl.dlss.dll` ni `nvngx_dlss.dll`;
  la auditoría de archivos no encontró aún un archivo de configuración
  gráfica en el prefix temporal. Resident Evil 4 cargó `steam_api64.dll` pero
  no llegó a `d3d12.dll` ni a NGX, incluso con AppID Steam opt-in.
- Esto mejora el launcher y descarta dos rutas de arranque, pero no demuestra
  `EvaluateFeature` auténtico. Recursos reales, NR remoto en GPU B, MFG y
  sincronización GPU-native permanecen pendientes.

## 2026-09-11 — runner reversible para un juego real y auditoría negativa

- Se añadió `scripts/run_real_game_remote_probe.sh`, un runner para probar un
  shipping executable real con el proxy NGX y el MVP remote-only CPU-gated.
- El runner valida los DLL esperados, conserva un backup, inyecta sólo durante
  la corrida y restaura el DLL original con reemplazo atómico y verificación
  SHA-256. Un guardian separado con `setsid` cubre también `SIGKILL`, muerte
  del shell y procesos zombie; GPU-native queda en
  `MGPU_CROSS_ADAPTER_GPU_NATIVE=0`.
- La prueba se ejecutó sobre Stellar Blade con GE-Proton11-6, 1280x720 y
  prefijo aislado. El proceso alcanzó el arranque D3D12 pero terminó por
  watchdog; no apareció `dlssnr-proxy.log`, no hubo eventos
  `remote_ngx_*` y `WINEDEBUG=+loaddll` no mostró carga de `nvngx_dlss.dll`.
- El hash final del DLL del juego coincidió con el original:
  `83de996b1589957d6bfb3df77e2f2ba7821f1014c73cfc5f8315241a0e4d3253`.
  No quedó modificación permanente en la instalación.
- Helldivers 2 se probó con el runner endurecido. GameGuard y el ejecutable
  cargaron, D3D12 llegó a inicializarse, pero no hubo carga de `nvngx_dlss.dll`,
  `EvaluateFeature` ni `dlssnr-proxy.log`. En una prueba adicional se mató el
  runner con `SIGKILL` mientras el hash del proxy estaba activo; el guardian
  restauró `8707e53b26c68c606b98bf31c223485ff30d310a261b1a36d48b2eaabc1507ec`.
- La regresión del launcher queda en `54/54`. Esto valida el mecanismo de
  prueba reversible, no una integración DLSS auténtica: siguen pendientes el
  camino Steam/launcher o una aplicación que active DLSS desde el arranque,
  los recursos reales, NR remoto en GPU B, MFG remoto y GPU-native.

## 2026-09-11 — modo remote-only reproducible y smoke completo

- Se añadió `patches/dlss5-linux-bridge-remote-create-fallback.patch` al
  orden reproducible de `scripts/build_bridge.sh`, con detección idempotente
  para checkouts donde la cadena anterior ya está aplicada.
- En el modo explícito `MGPU_DLSSNR_SKIP_LOCAL_NGX=1` más
  `MGPU_DLSSNR_REMOTE_NGX_FEATURE=1`, el bridge ahora evita el
  `CreateFeature` local, crea un handle opaco sintético protegido por
  `state_mutex` y libera correctamente el handle remoto y el token sintético.
  El camino normal no cambia cuando el modo remoto no está solicitado.
- Se reconstruyó el bridge con `scripts/build_bridge.sh`; los warnings de
  MinGW son los casts de ABI ya existentes y las dos DLL se generaron.
- La regresión Python pasa `53/53`, CMake recompila todos los targets y
  `mgpu-auto selftest --json` mantiene `passed=true`, P2P bidireccional y
  Vulkan→CUDA→P2P en ambas direcciones.
- Con GE-Proton11-6 completo, VKD3D experimental y dos RTX 3090, el smoke
  remote-only produjo `probe_return_code=0`; el log nuevo confirma
  `remote_only synthetic_handle`, `remote_only local EvaluateFeature skipped`,
  `remote_ngx_init/create/evaluate=0x00000001`, fence CPU completada y
  `output_return_copy=ok output_return_validation=ok`.
- Esto valida el MVP CPU-gated remoto en laboratorio, no una integración con
  un juego real. La sincronización GPU-nativa sigue pendiente explícitamente:
  la exportación de fence D3D12/VKD3D continúa en `0x80004005`.

## 2026-09-11 — auditoría reproducible de carga del proxy NGX

- Se añadió `patches/dlss5-linux-bridge-loader-audit.patch` al build
  reproducible de `scripts/build_bridge.sh`.
- La auditoría usa un `DllMain` global en `_nvngx.dll` y registra
  `loader_audit dll_process_attach` sin alterar el camino NGX.
- El parche aplica tanto sobre un checkout limpio del bridge como sobre la
  cadena experimental ya aplicada; el build MinGW terminó correctamente.
- El smoke aislado bajo Wine cargó el proxy, verificó los cuatro exports NGX
  principales y produjo el evento de carga. Esto prueba el instrumento, no
  todavía que Unreal invoque DLSS.
- El artefacto actualizado quedó instalado en
  `build/proton-resource-pair-worker-experimental`; no se habilitó
  sincronización GPU-nativa.
- Se intentó repetir inmediatamente el smoke D3D12/VKD3D+NGX con el único
  Proton disponible localmente. Ese runner incompleto aborta antes de crear
  D3D12 por `win32u.NtUserInitializeTouchInjection` y
  `ole32.CoInitialize` no implementadas; no es evidencia contra el bridge y
  no se cambió ningún estado de GPU/pantalla.

## 2026-09-11 — prueba real con VKD3D actual y preservación de selectores

- Se corrigió el formato de `vkd3d-mingw-pathcch-compat.patch`; ahora aplica
  limpiamente sobre un checkout VKD3D-Proton actual y el build fence-only
  termina `209/209`.
- `mgpu-auto` conserva `VKD3D_DUPLICATE_LUID_INDEX` y
  `VKD3D_DUPLICATE_LUID_INDEX_PER_DEVICE` cuando el usuario los activa, y la
  suite queda en `51/51`.
- La demo real se descargó, verificó y ejecutó con GE-Proton11-6 y el VKD3D
  recién compilado. Llegó a D3D12/swapchain, pero registró `Multiple adapters
  found with LUID` y no produjo cargas `nvngx`, `EvaluateFeature` ni eventos
  `remote_ngx_*`.
- Se probó además la colocación explícita de `d3d12.dll`/`d3d12core.dll` en
  un prefix temporal para descartar precedencia de Proton. El resultado fue
  el mismo; el experimento y el prefix fueron aislados y no tocaron monitores.
- Esto deja separado el problema de wiring del launcher del problema restante:
  hacer que Unreal llegue al runtime DLSS/Streamline y resolver identidad en
  el camino real. GPU-native y MFG siguen pendientes.

## 2026-09-11 — identidad física por device en VKD3D

- Se añadió `vkd3d-duplicate-luid-per-device.patch`, un modo opt-in para
  procesos que crean dos `ID3D12Device` con un mismo entorno: cada creación
  avanza por una física Vulkan distinta y conserva la verificación UUID/PCI.
- `run_vkd3d_interop_probe.sh` activa ese modo sólo para su probe de un proceso;
  los launchers multi-proceso siguen usando `VKD3D_DUPLICATE_LUID_INDEX=0|1`.
- El build reproducible incorpora también `vkd3d-mingw-pathcch-compat.patch`.
  `scripts/build_vkd3d_experimental.sh` compiló `d3d12.dll` y `d3d12core.dll`
  con VKD3D-Proton actual.
- Validación bajo GE-Proton11-6: `rc=0`, `multi_adapter_distinct=yes`,
  A=`uuid=af6de4b3 pci=0:1:0.0`, B=`uuid=5b9f385f pci=0:3:0.0`.
- Esto cierra la selección física del probe sintético, no la integración de
  DLSS en un juego real. Recursos auténticos, NR remoto, MFG y GPU-native
  permanecen pendientes.

## 2026-09-11 — VKD3D automático y diagnóstico de la demo real

- `mgpu-auto run --exe --enable-remote` ahora selecciona automáticamente el
  primer perfil VKD3D del proyecto que contenga `d3d12.dll` y
  `d3d12core.dll`; `VKD3D_DLL_DIR` explícito sigue teniendo prioridad.
- Se agregó una regresión para impedir que el launcher dependa de una variable
  de entorno manual cuando el perfil experimental ya está compilado. La suite
  `tests.test_mgpu_auto` pasa `50/50` y CMake recompila todos los targets.
- Con el perfil experimental seleccionado automáticamente, la demo real
  Unreal/D3D12 llegó a crear dispositivos VKD3D y a ejecutar el render bajo
  GE-Proton11-6. El log confirma el stopper de identidad: `Multiple adapters
  found with LUID`.
- Una sustitución temporal y reversible de `nvngx_dlss.dll` por el proxy no
  produjo nuevas llamadas al bridge; el hash del DLL original fue restaurado.
  Por tanto, todavía no se acredita que esta demo llegue a `EvaluateFeature`.
- Se conserva como pendiente la captura de recursos auténticos del juego,
  NR remoto funcional, MFG remoto y sincronización GPU-nativa.

## 2026-09-11 — launcher remoto directo para demos fuera de Steam

- `mgpu-auto run --exe` ahora acepta `--enable-remote` junto con un runner
  Proton y `--prefix` explícitos; prepara la misma política `remote-neural`
  que el camino Steam, incluyendo pair-worker, resource-FD, runtimes NGX,
  identidad física estricta y sincronización CPU-gated.
- La ruta directa no arranca si el transporte remoto no está listo y no
  degrada silenciosamente a NGX local cuando se pidió remoto.
- Se añadieron dos regresiones para prefix obligatorio y wiring completo; la
  suite quedó en `45/45` en ese punto del historial.
- Se verificó GE-Proton11-6 en staging temporal con SHA-256
  `659f8d71f2f78659340120b20c1c5a1464aa138939332a1376dea22f6d2dc2e4`.
- Pendiente: ejecutar una demo real y capturar su primer frame DLSS bajo este
  camino; GPU-native y MFG siguen fuera de este cambio.

## 2026-09-11 — primera ejecución real y cierre selectivo de Proton

- La demo Unreal/D3D12 arrancó con GE-Proton11-6 y VKD3D experimental bajo el
  launcher directo; el proceso consumió `4647 MiB` en una RTX 3090 y fue
  terminado por el watchdog de 45 s.
- El primer intento había fallado sólo porque faltaba crear la raíz del
  compat-data; `execute_direct` ahora crea esa carpeta explícita antes de
  Proton y clasifica retornos no cero como `failed`.
- GE-Proton desacopla `wineserver`/`xalia`; el launcher ahora limpia sólo
  procesos que contienen el runner, ejecutable o prefix de esa ejecución,
  excluyendo el propio launcher y su shell padre. La regresión queda en
  `48/48`, incluyendo un hijo desacoplado mediante `setsid`.
- El arranque real todavía no modificó el log del bridge; queda pendiente
  demostrar carga de `nvngx_dlss.dll` proxy y `EvaluateFeature` desde un juego.

## 2026-09-11 — wiring del launcher automático remoto

- `mgpu-auto` ya no fuerza artificialmente `transport_available=false`: valida
  Proton ejecutable, VKD3D (`d3d12.dll`/`d3d12core.dll`), helper CUDA y un perfil
  NGX completo.
- El perfil automático genera una política `remote-neural` con
  `resource-fd-pair-worker-remote-ngx`, resource-FD, selección de GPU render,
  runtimes NGX, identidad física estricta y `MGPU_CROSS_ADAPTER_GPU_NATIVE=0`.
- El launcher no inicia juegos por defecto. Requiere `--enable-remote` o
  `MGPU_AUTO_LAUNCH_REMOTE=1`, conservando fallback local y evitando activar
  experimentalmente la sincronización GPU-nativa.
- Se añadieron regresiones para la política Proton/pair-worker; la suite quedó
  en `42/42` tests `unittest`.
- La evidencia NGX remota sintética ya existente sigue siendo positiva en ambas
  direcciones; la captura de recursos auténticos de un juego continúa pendiente.

## 2026-09-11 — MVP CPU-gated bidireccional con Wine completo

- Se añadió `scripts/build_wine_runtime_experimental.sh` para reproducir un
  build Wine completo y coherente con X11, en lugar de usar el checkout parcial
  que provocaba un `SIGSEGV` temprano en `ntdll`.
- Wine `8f8792f` completo y VKD3D-Proton `0bd10357` recompilado pasaron el
  queue-SPI en ambas direcciones, con UUID/PCI físicos distintos.
- La cadena parcheada de Wine superó el anterior `ExportVulkanResourceFd=E_NOTIMPL`:
  exportó los tres planos, CUDA importó los FDs y `cuMemcpyPeer` validó A→B y
  B→A con readback no nulo.
- El helper persistente pasó ocho iteraciones consecutivas en cada sentido.
- El frame-loop CPU-gated pasó cuatro frames de payload variable en cada sentido,
  con `frame_loop_frames_completed=4`, `frame_loop_payload_varied=true` y
  validación de readback positiva.
- La exportación de fence GPU-nativa continúa sin soporte efectivo en este host:
  `0x80004005` en ambos adapters. No se promociona GPU-native ni NR remoto por
  estos resultados; el MVP operativo sigue usando sincronización CPU explícita,
  timeout y P2P.
- Se repitieron las pruebas sin tocar RandR/Xorg ni monitores. Las advertencias
  `libEGL` del entorno X11 no impidieron el smoke Vulkan/D3D12.

## 2026-09-11 — SPI de queue real para preparar el submit GPU-native

- Se agregó `vkd3d-command-list-queue-spi.patch`, que expone
  `ID3D12DXVKInteropDevice7::GetCommandListQueue` y conserva en cada command
  list la última queue observada por `ExecuteCommandLists`.
- La SPI devuelve `E_PENDING` antes de la primera ejecución y una referencia
  COM válida después; no inventa una queue ni vuelve a usar la queue auxiliar
  creada por el bridge.
- El bridge agregó el probe opt-in
  `MGPU_DLSSNR_GPU_NATIVE_QUEUE_PROBE=1`, que consulta la SPI desde
  `EvaluateFeature`, retiene la queue real y registra el resultado.
- La cadena de parches pasa `git apply --check`/`git diff --check`; VKD3D
  recompiló `d3d12core.dll` y el bridge produjo `bridge-nvngx.dll` y
  `_nvngx.dll` PE32+.
- Esta etapa no cierra GPU-native: el bridge todavía no sabe cuándo el juego
  terminó de ejecutar el command list actual, por lo que no se agregó ningún
  `Signal/Wait` especulativo. El worker remoto CPU-gated y MFG permanecen sin
  cambios.
- El smoke D3D12 ahora puede consultar la SPI tras un submit real con
  `MGPU_CROSS_ADAPTER_QUEUE_SPI=1`, comparar la queue observada y exigirla con
  `MGPU_CROSS_ADAPTER_REQUIRE_QUEUE_SPI=1`; el resultado se conserva en JSON.
- Se agregó `MGPU_CROSS_ADAPTER_QUEUE_SPI_ONLY=1` para aislar esa validación en
  ejecución. Con Wine 9 del sistema, la prueba aislada pasó la SPI y la
  identidad física; la corrida completa posterior quedó separadamente limitada
  por `ExportVulkanResourceFd=E_NOTIMPL`.
- La repetición B→A también pasó: ambas orientaciones devolvieron
  `queue_spi_success=true`, `queue_spi_result=0x00000000` y
  `physical_identity_distinct=true`. El resultado confirma el contrato de
  asociación de queue en ejecución, pero no equivale todavía a una señalización
  posterior al submit ni a NR remoto GPU-native.

## 2026-09-11 — limpieza de residuos temporales

- Se inspeccionaron `/tmp` y `/home/cristian/Juegos` sin tocar el repositorio ni
  rutas ajenas al proyecto.
- Se identificaron 64.939 procesos Wine huérfanos cuyo `WINEPREFIX` apuntaba a
  `/tmp/dlss5-*`, con sus prefijos ya eliminados; se terminaron de forma
  selectiva y no quedó ningún proceso Wine de esas pruebas.
- No quedaron directorios temporales `dlss5-*`/`3090-*`, archivos borrados
  abiertos ni contenido en `/home/cristian/Juegos`.
- El espacio libre pasó aproximadamente de 84 GiB a 85 GiB. Los directorios
  temporales del sistema y de otras aplicaciones se conservaron.

## 2026-09-11 — gate compuesto GPU-nativo + NGX

- Se construyó Wine completo desde el checkout actual `8f8792f` con los tres
  parches de exportación FD; la ejecución dejó de quedar atrapada en el
  page-fault/API-set del build parcial.
- El runtime completo pasó el round-trip de fence D3D12→Vulkan entre las dos
  RTX 3090 y el relay D3D12→Vulkan→CUDA: importación CUDA, espera GPU y
  sincronización del stream devolvieron éxito.
- El frame loop GPU-nativo sintético pasó A→B y B→A con 2/2 frames, tres
  planos resource-FD, payload variable, readback y `physical_identity_distinct`.
- El smoke ahora expone `MGPU_CROSS_ADAPTER_REQUIRE_NGX_WITH_GPU_NATIVE=1` y
  emite `gpu_native_ngx_composite_requested/success`. El gate compuesto fue
  probado: el transporte pasa, pero el proceso termina `rc=25` porque el
  proxy directo NGX devuelve `Init_Ext=0xbad00002` y no evalúa.
- El bridge añade la sonda opt-in `MGPU_DLSSNR_GPU_NATIVE_SYNC_PROBE=1`, que
  intenta exportar una fence compartida en el device fuente y en el remoto y
  registra ambos HRESULT/FD. Se aplicó sobre la cadena completa y las DLL
  resultantes compilaron como PE32+.
- Se corrigieron los hunks del parche de la sonda para que
  `scripts/build_bridge.sh` lo aplique limpiamente después del worker pair;
  `git apply --check` y una recompilación completa del bridge pasan.
- El host NGX de prueba con el Wine completo quedó atrapado en la
  inicialización EGL antes de cargar el bridge y fue detenido por watchdog;
  al no existir `dlssnr-proxy.log`, esa ejecución se conserva como no
  concluyente y no como fallo de la sonda.
- Un segundo intento con el Proton GE recuperado de la papelera abortó antes
  de cargar NGX por `win32u.NtUserInitializeTouchInjection`/`ole32.CoInitialize`;
  tampoco produjo `dlssnr-proxy.log` y no aporta evidencia de sincronización.
- Esta evidencia cierra el sustrato GPU-nativo del laboratorio, pero no
  promociona NR remoto: falta conectar el bridge `resource-fd-pair-worker` a
  la señalización GPU-nativa en el mismo ciclo y luego probar con un juego.

## 2026-09-11 — revalidación del stopper GPU-native D3D12

- Se corrigió el orden de `build_winevulkan_experimental.sh`: primero se
  aplica `winevulkan-expose-external-memory-fd.patch` y luego
  `winevulkan-expose-external-semaphore-fd.patch`, ya que ambos parches
  comparten el bloque `UNEXPOSED_EXTENSIONS` de `make_vulkan`.
- Se agregó una regresión que fija ese orden y evita que una base Wine actual
  falle al aplicar el segundo parche por contexto ya modificado.
- El smoke de fence D3D12/VKD3D ahora tiene watchdog interno de 60 s
  (`MGPU_VKD3D_FENCE_TIMEOUT_SECONDS`) y logging silencioso por defecto. La
  prueba del runtime Wine parcialmente compilado quedó registrada como no
  concluyente: entró en `load_apiset_dll`/page-fault y expiró sin tocar
  monitores ni producir evidencia de fence.

- Repetida la prueba `MGPU_CROSS_ADAPTER_GPU_NATIVE=1` con GE-Proton11-6
  oficial y el profile pair-worker experimental.
- VKD3D creó ambos devices y exportó los resource-FD de color/motion/depth,
  pero la fence no se pudo exportar: `proc=0`, `enabled=0`,
  `export_a=0x80004001` (`E_NOTIMPL`), `export_b=0x80004005` (`E_FAIL`) y
  `fds=-1/0`.
- Se agregó al resumen JSON del smoke el HRESULT específico de cada
  exportación de fence y el FD obtenido; el probe ahora emite un resumen
  mínimo aunque el gate falle antes del cierre normal.
- El smoke registra ahora UUID/PCI de ambos devices y el runner exige por
  defecto identidad física distinta en el camino resource-FD. La entrada
  duplicada que VKD3D puede enumerar queda diagnosticada y no se acepta como
  segundo adapter.
- Se agregó `vkd3d-duplicate-luid-strict-identity.patch`: si VKD3D no puede
  resolver el índice solicitado a una física distinta, aborta explícitamente
  en lugar de caer al matching por LUID y seleccionar potencialmente la GPU
  equivocada. El parche base aplica limpiamente sobre VKD3D-Proton actual
  (`0bd10357`) y el target `libs/d3d12core/d3d12core.dll` se reconstruyó con
  éxito como PE32+. Falta instalarlo en un Proton/Wine completo y repetir la
  corrida oficial para cerrar el check de ejecución.
- Se portó el diagnóstico de capacidades de fence a la base actual y se
  corrigió el corte de `VKD3D_FENCE_ONLY=1` para que incluya también ese
  parche después de agregar el guard estricto.
- El warning `LD_PRELOAD ... wrong ELF class` corresponde al shim Linux de
  64 bits heredado por un proceso Proton de 32 bits; no cambia el diagnóstico
  y no es la causa del fallo.
- GPU-native D3D12/VKD3D, identidad física duplicada y juego real siguen
  pendientes. El MVP CPU-gated remoto permanece como camino validado.

## 2026-09-11 — remote-ngx automático sobre Proton oficial

- `mgpu-auto remote-selftest --json` pasó A→B y B→A con GE-Proton11-6
  oficial y `build/proton-resource-pair-worker-experimental`.
- El bridge confirmó en ambos sentidos `remote_ngx_init/create/evaluate=0x1`,
  submit/fence correcto, retorno P2P y `output_return_validation=ok`.
- El perfil persistente oficial pasó tres frames en cada orientación.
- `mgpu-auto` ahora selecciona automáticamente el perfil pair-worker coherente
  y completa sus artefactos cuando están disponibles; la suite queda en 31/31.
- La selección automática fue probada sin variables explícitas de bridge/core/
  runtime/NR: Proton oficial pasó A→B y B→A con todos los gates de retorno
  remoto positivos.
- GPU-native D3D12 y juego real siguen explícitamente pendientes.
- El perfil de presentación descubrió que el runner no tenía watchdog externo
  y podía quedar esperando un helper de output; ahora usa `setsid timeout` con
  TERM/KILL y timeout configurable (`MGPU_CROSS_ADAPTER_TIMEOUT_SECONDS`).
- La corrida bloqueada fue terminada sólo dentro de su grupo de prueba; no se
  modificaron RandR/Xorg ni se usa como evidencia de presentación exitosa.
- Repetida la presentación con el watchdog activo sobre Proton oficial:
  A→B y B→A pasaron con `presentation_success=true` y 3/3 frames presentados;
  `DP-0` y `HDMI-1-0` permanecieron conectados.

## 2026-09-11 — wiring DXVK del host NGX

- `run_ngx_test.sh` ahora acepta `MGPU_DXVK_DIR` y opcionalmente
  `MGPU_DXVK_NVAPI_DIR`, valida sus DLLs y aplica overrides nativos coherentes.
- La regresión queda en 30/30.
- Una corrida directa con el `dxgi.dll` experimental enumeró las RTX 3090,
  pero terminó en `Failed to initialize DXVK` por faltar la pila completa
  `winevulkan`/Proton. Esto no se cuenta como evaluación NGX positiva.
- Con el tarball oficial GE-Proton11-6 verificado por SHA-512, DXVK incluido
  y el staging correcto de `_nvngx_real.dll`, el host pasó en B y A:
  `D3D12CreateDevice`, `Init_Ext`, `CreateFeature`, `EvaluateFeature` y
  `Shutdown1` fueron exitosos; el bridge registró `DLSSNR Evaluate=0x1` en
  ambas orientaciones.
- Es una evaluación NGX local sintética sobre el device seleccionado, no una
  prueba de NR remoto ni de un juego real.
- El tarball GE-Proton11-6 pasó SHA-512 y fue eliminado junto con su extracción
  y prefixes temporales al terminar la prueba.
- Una build GE-Proton recuperada de la papelera abortó antes del host por
  funciones Win32U no implementadas; su prefix temporal fue eliminado y no se
  considera evidencia de ejecución válida.

## 2026-09-11 — propagación del selector PCI

- `run_ngx_test.sh` y `run_official_d3d12_host_probe.sh` ahora pasan
  `MGPU_NGX_PRIMARY_PCI` al host Windows bajo Wine/Proton.
- Se agregó una regresión estática; la suite Python queda en 29/29.
- La ejecución real con el demo/Proton sigue pendiente y no se declara como
  validada hasta obtener `ngx_d3d12_smoke.result.txt` y el log del bridge.

## 2026-09-11 — limpieza de artefactos temporales

- Eliminados los directorios exactos `/tmp/dlss5-*` usados por nuestras
  compilaciones y pruebas; no quedaron coincidencias.
- `/home/cristian/Juegos` fue verificado vacío. El espacio libre del sistema
  quedó en aproximadamente 84 GiB.

## 2026-09-11 — regresión de transporte y CUDA nativo

- Repetidos los probes Vulkan→CUDA→P2P en A→B y B→A: ambas orientaciones
  mantienen `validation=ok` y el mapeo UUID/PCI correcto.
- Repetido el frame-loop CPU-gated de tres planos: 120/120 frames válidos.
- Repetida la sincronización GPU-nativa CUDA↔CUDA en ambas direcciones:
  120/120 frames en cada caso, con `gpu_native_waits=true`.
- `./scripts/mgpu-auto selftest --json` volvió a informar `passed=true`,
  incluyendo P2P, interop Vulkan↔CUDA, semáforos externos, imágenes CUDA P2P
  y el frame-loop CUDA nativo.
- Esta evidencia no cierra el gate D3D12/VKD3D: la fence externa del host
  continúa devolviendo `E_NOTIMPL` y el MVP remoto mantiene coordinación CPU.

## 2026-09-11 — selector físico PCI para el host NGX

- `tests/ngx_d3d12_smoke.cpp` acepta `MGPU_NGX_PRIMARY_PCI=0:3:0.0` y
  verifica el BDF mediante `GetVulkanPhysicalDeviceIdentity` antes de elegir
  el `ID3D12Device`. Esto prepara el host para usar la 3090 B sin depender del
  orden de adapters/LUIDs de VKD3D.
- El binario cross-compilado pasa la compilación MinGW. La ejecución con el
  host modificado quedó bloqueada antes del primer log del host dentro del
  harness Wine experimental; se detuvo por timeout y no se declara integración
  positiva ni evaluación remota adicional.
- GPU-native, juego real y MFG remoto permanecen sin cambios como pendientes.

## 2026-09-11 — MVP remoto automático CPU-gated corregido y validado

- Los runners ahora propagan `MGPU_CUDA_WORKER_HELPER`, que era el nombre que
  realmente consume el pair-worker del bridge. La ausencia de esa variable
  hacía que el worker retornara antes de seleccionar la GPU remota.
- `mgpu-auto` fuerza `MGPU_NGX_PRIME_SOURCE=0` en perfiles `remote-ngx` para
  evitar el fallo conocido de estado global NGX (`0xbad00007`) en A-first.
- `mgpu-auto remote-selftest` real, con
  `resource-fd-pair-worker-remote-ngx`, CPU-sync y ambas direcciones físicas,
  devolvió `available=true`: A→B y B→A pasaron P2P/resource-FD/readback,
  `remote_ngx_evaluate`, submit/fence CPU, retorno P2P y validación FNV.
- El log del bridge confirmó `Init/Create/Evaluate=0x1`, fence CPU completada,
  `device_removed=0` y `output_return_copy=ok output_return_validation=ok`.
- No se promociona a GPU-native ni a juego real: GE-Proton sigue devolviendo
  `ExportVulkanFenceFd=E_NOTIMPL`, por lo que esa sincronización permanece
  pendiente explícita.

## 2026-09-11 — persistencia, modo secuencial y presentación sintética

- El perfil `resource-fd-pair-worker-remote-ngx-persistent` pasó 3/3 frames
  remotos en A→B y B→A, con retorno P2P y validación FNV correctos.
- El perfil `resource-fd-pair-worker-sequential-dual` pasó en ambas
  orientaciones: remoto completo y luego `local_after_remote` Init/Create/
  Evaluate exitosos. No representa simultaneidad.
- El sink de presentación sintético pasó 3/3 `Present=0x0`; el runner
  reintentó automáticamente B→A porque A→B no puede crear el swapchain bajo
  VKD3D en esa orientación. RandR conservó `DP-0` y `HDMI-1-0` conectados.
- GPU-native, MFG remoto, inputs auténticos de juego y medición visual real
  continúan pendientes.
- El runner Wine ya no deja un prefix fijo en `/tmp`: crea un directorio
  temporal propio y lo elimina al terminar si el usuario no indicó
  `WINEPREFIX`/`OUT_DIR`; una prueba de salida temprana confirmó limpieza
  completa.
- `mgpu-auto remote-selftest` ya no exige `DLSS_DEMO_DIR` cuando se entrega
  `MGPU_NGX_CORE_DLL`; se agregó una regresión y la suite pasó a 28/28.

## 2026-09-11 — DXVK-NVAPI real y evaluación local en B

- Se añadió soporte opt-in del runner para `MGPU_DXVK_DIR` y
  `MGPU_DXVK_NVAPI_DIR`, incluyendo `dxgi.dll`, `nvapi64.dll` y
  `nvofapi64.dll` en un prefix temporal. También se agregó el builder x64
  `scripts/build_dxvk_nvapi_x64.sh`.
- Con `DXVK_CONFIG='dxgi.customVendorId = 10de'`, DXVK-NVAPI inicializa sobre
  las RTX 3090 reales (`NvAPI_Initialize=0x1`).
- La corrida B-first validó transferencia persistente A→B de tres planos en
  3/3 frames y chaining real DLSS estándar→DLSSNR en B:
  `Init=0x1`, `Create=0x1`, `Evaluate=0x1`, readback no nulo.
- A-first sigue en `CreateFeature=0xbad00007` para el segundo device; el
  diagnóstico confirma estado global de NGX. Esto no se promociona a NR remoto
  ni a sincronización GPU-native completa con un juego real.

## 2026-09-11 — stub NVAPI diagnóstico para NGX

- Se añadió `tests/nvapi_ngx_compat_stub.c` y
  `scripts/build_nvapi_ngx_compat_stub.sh`. Es una DLL sintética, opt-in y
  sólo de laboratorio: completa las estructuras/versiones NVAPI que consulta
  el core NGX, sin reemplazar las librerías NVIDIA del sistema.
- La prueba movió el diagnóstico de un fallo temprano de ABI a
  `Init_Ext=0xbad00001` (`FeatureNotSupported`), manteniendo el transporte
  GPU-native en 3/3 frames. Declarar AD100 en el stub tampoco hizo que el core
  inicializara; no se considera una solución funcional ni se activa por
  defecto.

## 2026-09-11 — ruta Unix opt-in para compatibilidad NVML

- El runner acepta `MGPU_NGX_COMPAT_UNIX_DIR` y la agrega a `WINEDLLPATH` y
  `LD_LIBRARY_PATH`, permitiendo suministrar el `nvml.so` Unix junto a su
  `nvml.dll` PE.
- La prueba encontró igualmente `ERROR_DLL_INIT_FAILED`; el wrapper NVML de
  GE-Proton no es compatible con el Wine experimental usado por el harness.
  No se copian ni modifican librerías NVIDIA del sistema.

## 2026-09-11 — preload opt-in de compatibilidad NGX

- El host acepta `MGPU_NGX_PRELOAD_COMPAT=1` para precargar
  `nvapi64.dll`, `nvml.dll` y `nvofapi64.dll` antes de
  `NVSDK_NGX_D3D12_Init_Ext`.
- En la prueba con DLLs GE-Proton, `nvapi64` y `nvofapi64` cargaron; `nvml`
  devolvió `ERROR_DLL_INIT_FAILED` y NGX siguió devolviendo `0xbad00002`.
- El worker GPU-native continúa independiente y pasa 3/3 frames en ambas
  orientaciones; sólo el inicializador NGX queda fallando.

## 2026-09-11 — diagnóstico de orden NGX y compatibilidad NVIDIA del prefix

- El smoke de tres planos agrega `MGPU_NGX_PRIME_SOURCE=1` (por defecto):
  inicializa primero el proxy NGX en el device A y luego intenta abrir el
  feature en B, reproduciendo el orden del bridge CPU-gated que históricamente
  sí llegó a `EvaluateFeature=0x1`.
- El resultado actual en el Wine experimental no cambia: el core devuelve
  `Init_Ext=0xbad00002` tanto en A como en B. El transporte GPU-native sí llega
  a 3/3 y valida color/motion/depth antes de ese punto.
- El runner acepta `MGPU_NGX_COMPAT_DLL_DIR` para copiar al prefix temporal
  `nvapi64.dll`, `nvml.dll` y `nvofapi64.dll` de una instalación GE-Proton.
  Repetir con esas DLL no eliminó `0xbad00002`; no se modifica ningún prefix
  permanente.
- La traza `+loaddll` confirma que proxy, core y bridge cargan correctamente y
  que el fallo ocurre dentro de `Init_Ext`, no por un DLL ausente. La hipótesis
  restante es una diferencia de capacidades/ABI entre el core NGX y el stack
  Wine/VKD3D experimental; queda pendiente validarla con un stack GE-Proton
  completo que también exponga las fences experimentales.

## 2026-09-11 — runner NGX explícito y combinación GPU-native diagnosticada

- `scripts/run_d3d12_cross_adapter_frame_smoke_wine.sh` ahora arma la cadena
  NGX de forma explícita cuando `MGPU_NGX_CROSS_ADAPTER=1`: proxy, bridge,
  core NGX, runtime DLSS y runtime NR se validan y se copian con nombres
  separados. Esto evita mezclar un proxy con el runtime real y ocultar el
  diagnóstico detrás de una recursión de `Init_Ext`.
- El runner permite seleccionar los artefactos mediante
  `MGPU_NGX_CORE_DLL`, `DLSS_RUNTIME_DLL`, `DLSS_NR_DLL`, `NGX_BRIDGE_DIR`,
  `MGPU_NGX_PROXY_DLL` y `MGPU_NGX_BRIDGE_DLL`; también permite cambiar
  explícitamente `VKD3D_DUPLICATE_LUID_ADAPTERS` para pruebas de identidad.
- La combinación opt-in GPU-native + NGX quedó probada hasta el transporte:
  `gpu_native_worker_spawn=ok`, 3/3 frames, color/motion/depth válidos. NGX
  falla antes de crear el feature con `Init_Ext=0xbad00002` en el runner Wine
  directo, aun con artefactos separados; por lo tanto no se declara NR remoto
  funcional en esta cadena.
- La prueba con LUID no duplicado confirma el stopper alternativo: VKD3D deja
  ambos devices sobre la primera física y el import del worker termina en
  `CUDA_ERROR_UNKNOWN`. El modo duplicado sigue siendo requisito del fixture.
- Regresión cerrada: CMake correcto, 27/27 tests Python, `bash -n`,
  `git diff --check` y GPU-native 2/2 frames en A→B y B→A.

## 2026-09-11 — worker GPU-native opt-in integrado al smoke de tres planos

- `tests/d3d12_cross_adapter_frame_smoke.cpp` ahora tiene un modo opt-in
  `MGPU_CROSS_ADAPTER_GPU_NATIVE=1`. Mantiene imports CUDA y stream persistentes,
  exporta un pool de fences one-shot por frame y coordina los tres resource-FD
  (`color`, `motion`, `depth`) con `cuWaitExternalSemaphoresAsync`,
  `cuMemcpyPeerAsync`, `cuSignalExternalSemaphoresAsync` y
  `ID3D12CommandQueue::Wait` en B.
- Se corrigió además el supuesto de que DXGI debía nombrar literalmente
  `RTX 3090`: el fixture cae a `D3D12CreateDevice(nullptr)` con
  `VKD3D_DUPLICATE_LUID_INDEX`, preservando la identidad física UUID/PCI.
- Se añadió `scripts/run_d3d12_cross_adapter_frame_smoke_wine.sh` para ejecutar
  el fixture con el Wine/VKD3D experimental completo. Las corridas autoritativas
  pasaron 3/3 en A→B y B→A, con `gpu_native_worker_spawn=ok`,
  `gpu_native_worker_stop=ok`, readback color/motion/depth válido y JSON con
  `gpu_native_sync_success=true`.
- Este check sigue siendo sintético y opt-in: la primera preparación y el
  readback final siguen siendo del harness, no hay todavía captura de recursos
  de un juego ni evaluación NR/presentación conectadas. La sincronización
  GPU-native completa del MVP continúa pendiente.

## 2026-09-11 — frame-loop GPU-ordered de resource-FD entre D3D12 y CUDA

- Se añadió `tests/cuda_external_fenced_p2p_helper.cpp`: importa dos
  resource-FD en contextos CUDA A/B, mantiene un worker persistente, espera la
  fence del slot en B, ejecuta `cuMemcpyPeerAsync` y señaliza la fence de salida
  en el mismo stream.
- Se añadió `tests/vkd3d_resource_fd_gpu_sync_smoke.cpp` y el runner
  `scripts/run_vkd3d_resource_fd_gpu_sync_smoke.sh`. El fixture crea recursos
  D3D12 reales en dos devices físicos seleccionados por el modo de LUID
  duplicado, exporta sus allocations, y valida el recorrido completo:
  `D3D12 A → fence A → CUDA B/P2P → fence B → D3D12 B → readback`.
- Después de corregir el estado inicial de las command lists y cambiar el
  transporte persistente a un pool de fences one-shot (la reutilización de una
  misma fence timeline se bloqueaba en el tercer valor), el resultado
  autoritativo fue `gpu_sync_resource_frame_loop=pass frames=3/3 mode=persistent`;
  los imports de recursos/contextos/stream se mantuvieron vivos y cada slot
  completó wait/copy/signal CUDA verificando el payload en B.
- Esto cierra el primer loop GPU-ordered sintético de recursos, pero no el
  ring persistente de un juego ni NR remoto real: el producer/consumer sigue
  siendo un harness y usa un pool finito de fences one-shot. La sincronización
  GPU-native completa del MVP continúa explícitamente pendiente.
- El runner acepta `MGPU_GPU_SYNC_BOTH=1` para repetir automáticamente el
  mismo contrato en A→B y B→A; ambas orientaciones pasaron 3/3 frames en esta
  iteración.

## 2026-09-11 — fence D3D12 → CUDA cross-processo y señalización de cola

- Se añadió `tests/cuda_external_semaphore_wait_helper.cpp`, que importa una
  fence FD OPAQUE en CUDA GPU B y espera con `cuWaitExternalSemaphoresAsync`;
  el helper publica estados `ready`/`done` y admite una compuerta para evitar
  falsos bloqueos del driver mientras se arma el smoke.
- `tests/vkd3d_cross_adapter_fence_smoke.cpp` crea una fence independiente para
  CUDA, lanza el helper de forma asíncrona mediante `__wine_unix_spawnvp`,
  señaliza desde `ID3D12Fence::Signal` y, opcionalmente, desde
  `ID3D12CommandQueue::Signal` (`MGPU_FENCE_CUDA_GPU_SIGNAL=1`).
- Con Wine/VKD3D completos y coherentes, las dos RTX 3090 pasaron ambos modos:
  `cuImportExternalSemaphore=CUDA_SUCCESS`, `cuWaitExternalSemaphoresAsync=CUDA_SUCCESS`,
  `cuStreamSynchronize=CUDA_SUCCESS` y `cross_adapter_fence_roundtrip=pass`.
- El helper admite además un relay opt-in: importa una segunda fence de B,
  encola `cuWaitExternalSemaphoresAsync(A)` seguido de
  `cuSignalExternalSemaphoresAsync(B)` en el mismo stream. El smoke confirmó
  `cuda_fence_relay=pass` y `GetCompletedValue(B)>=1` después de
  `D3D12CommandQueue::Signal(A)`.
- El runner GE-Proton sin `winevulkan` experimental sigue reproduciendo
  `ExportVulkanFenceFd=0x80004001 (E_NOTIMPL)`. El resultado no promociona aún
  `gpu_native_sync`: falta integrarlo al ring de imágenes/recursos de un juego
  y al path real de NR; el MVP automático sigue siendo CPU-gated.

## 2026-09-11 — transporte explícito D3D12 resource-FD → CUDA/P2P → NGX

- Se revalidó el backend existente con `resource-FD`, sin depender del aliasing
  Vulkan directo entre las dos físicas.
- A→B y B→A pasaron con tres planos (`color`, `motion`, `depth`), tres
  iteraciones persistentes y `cuMemcpyPeer` exitoso.
- El frame-loop CPU-gated ejecutó 3/3 frames con
  `frame_loop_payload_varied=true`; cada frame completó NGX en B con
  `EvaluateFeature=0x00000001`, readback de planos válido y readback NGX no
  nulo (`fnv1a=0x3d0cf3accd85fd8a` en la corrida con frame-loop).
- Se corrigió `mgpu-auto`: ahora propaga `MGPU_REMOTE_FRAME_LOOP`, raster y
  presentación para cualquier transporte `resource-FD`, no sólo para
  `pair-worker`; el selftest automático A→B/B→A volvió a pasar con 3/3 frames.
- Regresión de esta iteración: CMake correcto, 27/27 tests Python, `bash -n`
  sobre todos los runners y `mgpu-auto selftest --json` con `passed=true`.
- Limpieza: se eliminaron los cuatro directorios/logs temporales de las
  corridas de esta iteración; se conservó únicamente
  `/tmp/dlss5-geproton-clean` como instalación reproducible del laboratorio.
- Este resultado cierra el MVP sintético explícito de transferencia P2P, pero no
  equivale a un juego real: continúa pendiente capturar recursos auténticos,
  presentar desde B y sustituir el gate CPU por sincronización GPU-native.

## 2026-09-11 — importación estructural de recurso D3D12 cross-adapter

- Se agregó `tests/vkd3d_cross_adapter_resource_import_smoke.cpp` y su runner
  `scripts/run_vkd3d_cross_adapter_resource_import_smoke.sh`.
- Wine experimental ahora expone memoria externa FD y `win32u` acepta la cadena
  `VkImportMemoryFdInfoKHR`; VKD3D actual usa una SPI `ID3D12DeviceExt6` para
  importar la allocation en el segundo device.
- Resultado autoritativo en las dos RTX 3090: identidad física distinta
  (`0:1:0.0` → `0:3:0.0`), `export_resource_fd=0`,
  `import_resource_fd_b=0`, binding/copia estructural OK.
- El control A→A produce datos no nulos, pero B→B devuelve
  `resource_b_copy_readback=pass ... nonzero=0`. Por eso se marca como
  `cross_adapter_resource_import=pass` y
  `cross_adapter_resource_content=fail`: todavía no hay visibilidad de payload
  cross-GPU demostrada.
- El build de VKD3D ya no depende de los parches de importación antiguos que no
  aplicaban sobre la base actual; usa `vkd3d-import-resource-fd-current.patch`.
- No se activa el modo remoto automático ni MFG; la sincronización GPU-native
  continúa explícitamente pendiente.

## 2026-09-11 — fence D3D12 cross-adapter A→B

- Se agregó `tests/vkd3d_cross_adapter_fence_smoke.cpp` y el runner
  `scripts/run_vkd3d_cross_adapter_fence_smoke.sh`.
- Con `VKD3D_DUPLICATE_LUID_ADAPTERS=1`, el fixture crea dos devices D3D12,
  consulta la identidad física UUID/PCI y exige que sean distintos antes de
  continuar.
- La corrida autoritativa pasó en las dos RTX 3090: fence compartida creada en
  A, FD OPAQUE exportado, semáforo timeline importado en B, contador `0→1`
  después de `Signal(1)` y `vkWaitSemaphores` exitoso:
  `cross_adapter_fence_roundtrip=pass`.
- El runner ahora instala en el prefix temporal los módulos PE compatibles con
  el Wine aislado (`cryptbase.dll` y `winex11.drv`). Esto elimina el falso
  fallo de bootstrap `SystemFunction036`/`nodrv_CreateWindow` observado en la
  primera corrida.
- Esto cierra la señalización cross-adapter básica, pero no colas, recursos,
  ownership/layout, NR remoto ni sincronización GPU-native del juego; el MVP
  continúa CPU-gated.

## 2026-09-11 — fence D3D12 exportada e importada por Vulkan

- Se agregó `tests/vkd3d_fence_fd_smoke.cpp` y el runner
  `scripts/run_vkd3d_fence_fd_smoke.sh`. El fixture evita depender de la
  enumeración de salidas DXGI: crea el device D3D12 por defecto, consulta
  `ID3D12DXVKInteropDevice5`, exporta la fence compartida como
  `VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT`, importa el FD en un
  semáforo Vulkan timeline y verifica la señalización/espera.
- La corrida con Wine completo X11 parcheado y VKD3D-Proton 3.1 modificado
  pasó: `create_device=0`, `create_shared_fence=0`, `query_interop_device5=0`,
  `export_fence_fd=0`, FD válido, `import_fence_fd=0`, contador Vulkan `0→1`,
  `d3d12_signal_1=0` y `vulkan_wait_value_1=0`.
- `scripts/build_vkd3d_experimental.sh` admite ahora
  `VKD3D_FENCE_ONLY=1` para construir los parches de fence sin bloquearse por
  los dos parches de importación de recursos que no aplican al checkout actual;
  también inicializa submódulos si faltan.
- Este resultado elimina el `E_NOTIMPL` del transporte básico D3D12↔Vulkan,
  pero no demuestra todavía sincronización GPU-native cross-adapter ni un
  frame loop de juego. El MVP remoto continúa CPU-gated deliberadamente.

## 2026-09-11 — validación Wine Vulkan de semáforos externos FD

- Se agregó `tests/wine_vulkan_external_semaphore_probe.cpp`, un fixture
  Windows/Vulkan autocontenido que selecciona un físico que anuncia
  `VK_KHR_external_semaphore_fd`, crea un `VkDevice` real con la extensión,
  obtiene `vkGetSemaphoreFdKHR`, crea un semáforo `OPAQUE_FD` y solicita el FD.
- Se agregó `scripts/run_wine_vulkan_external_semaphore_probe.sh`, que compila
  el fixture con MinGW y lo ejecuta con un loader, wineserver y prefix Wine
  explícitamente indicados. Por defecto usa el build aislado de la prueba y
  no cambia GE-Proton ni el Wine del sistema.
- La corrida autoritativa terminó correctamente: cinco dispositivos
  Vulkan expusieron la extensión; `create_device_result=0`,
  `create_semaphore_result=0`, `export_fd_result=0`, FD válido y
  `vkGetSemaphoreFdKHR_proc=yes exported_fd=yes`. Un sexto dispositivo
  virtual no expuso la extensión y no se seleccionó.
- Esto supera la barrera de exposición de `winevulkan` a nivel Vulkan, pero no
  prueba todavía una fence D3D12/VKD3D. GE-Proton continúa cargando su módulo
  `winevulkan` como `builtin`, por lo que el GPU-native D3D12 sigue pendiente
  y el MVP CPU-gated no cambia.

## 2026-09-11 — candidato Wine para exponer semáforos FD

- Se aisló el filtro exacto que oculta `VK_KHR_external_semaphore_fd` en
  `dlls/winevulkan/make_vulkan` y se agregó
  `patches/winevulkan-expose-external-semaphore-fd.patch`.
- Se regeneró y compiló un par experimental `winevulkan.dll`/`winevulkan.so`
  desde Wine con esa línea removida. La compilación terminó correctamente.
- Se agregó `scripts/build_winevulkan_experimental.sh` para repetir la
  aplicación del parche, regeneración y build de los dos artefactos desde un
  checkout Wine aislado.
- La prueba contra GE-Proton no pudo cargar ese par: el loader siguió
  registrando `C:\\windows\\system32\\winevulkan.dll` como `builtin` y
  VKD3D continuó enumerando `external_semaphore_fd_spec=0`.
- El resultado es un candidato de integración de Wine, no una solución
  activada. La sincronización GPU-native D3D12/VKD3D sigue pendiente y el MVP
  CPU-gated no cambia.

## 2026-09-11 — semáforos externos nativos Vulkan↔CUDA

- Se agregó `mgpu-vulkan-cuda-external-semaphore-probe`, que crea un `VkDevice`
  nativo por GPU física, exporta semáforos `OPAQUE_FD`, los importa en CUDA y
  valida las dos direcciones: señal Vulkan/espera CUDA y señal CUDA/espera
  Vulkan.
- En el host dual RTX 3090 la prueba pasó en ambas orientaciones físicas:
  Vulkan GPU0→CUDA GPU1 y Vulkan GPU1→CUDA GPU0.
- `mgpu-auto selftest` incorpora el resultado bajo
  `vulkan_cuda_external_semaphore` y exige el gate para declarar el self-test
  completo.
- El resultado queda separado deliberadamente de `cuda_native_sync` y del
  camino D3D12/VKD3D: no resuelve el `E_NOTIMPL` del host VKD3D ni cambia la
  política del MVP CPU-gated.

## 2026-09-11 — fixture D3D12 de rasterización con DXC

- Se agregó `tests/shaders/cross_adapter_triangle.hlsl` y el modo opt-in
  `MGPU_CROSS_ADAPTER_RASTER=1`. El runner compila vertex/pixel shaders con
  DXC, los embebe temporalmente en el ejecutable, crea root signature/PSO y
  ejecuta un `DrawInstanced(3, 1, 0, 0)` sobre el recurso `Color` de A.
- El modo normal no requiere DXC y conserva el smoke sintético basado en
  `ClearRenderTargetView`. Para activar el fixture se debe indicar
  `MGPU_DXC=/ruta/al/dxc` con el binario Linux oficial de DXC.
- El gate JSON informa `raster_requested`, `raster_ready` y `raster_submitted`,
  y devuelve fallo si el draw solicitado no pudo crearse o enviarse. Esto
  acerca el host a un frame gráfico real, pero no cierra el check de recursos
  de un juego ni la sincronización GPU-native.
- `mgpu-auto remote-selftest` puede exigir el mismo camino con
  `MGPU_REMOTE_RASTER=1`; el gate no se activa de manera implícita.
- Validación en la RTX 3090 dual: `raster_ready=true`,
  `raster_submitted=true`, `readback_nonzero=453043`, 3/3 frames NGX en B,
  retorno validado por el bridge y 3/3 `Present` tras el retry automático
  B→A. Sigue siendo un fixture de laboratorio y usa fences CPU.

## 2026-09-11 — producer frame loop CPU-gated

- El smoke acepta `MGPU_CROSS_ADAPTER_FRAME_LOOP=1` y
  `MGPU_CROSS_ADAPTER_FRAME_COUNT=N`. Después del primer frame, vuelve a
  grabar `Color`, `Motion` y `Depth`, espera la fence D3D12 del productor y
  repite el transporte heap-FD o resource-FD/P2P por frame.
- Se validaron 3/3 frames con payload cambiante en ambos modos de transporte;
  resource-FD confirmó además los readbacks de motion/depth. El JSON ahora
  expone `frame_loop_frames_completed`, `frame_loop_payload_varied` y
  `frame_loop_success`.
- `mgpu-auto remote-selftest` puede exigirlo con `MGPU_REMOTE_FRAME_LOOP=1`.
  Esto acerca el MVP a un ring de productor, pero no es todavía un frame loop
  de un juego ni reemplaza fences/semaphores GPU-native.
- Corrida combinada autoritativa: `MGPU_REMOTE_RASTER=1`,
  `MGPU_REMOTE_FRAME_LOOP=1`, transporte resource-FD, 3/3 frames variables,
  3/3 evaluaciones NGX, retorno validado y 3/3 `Present`; el fallback de
  orientación terminó en B→A. Continúa siendo laboratorio CPU-gated.

## 2026-09-11 — presentación sintética y selección automática de orientación

- El smoke D3D12 admite `MGPU_CROSS_ADAPTER_PRESENT=1` y crea un swapchain
  `DXGI_SWAP_EFFECT_FLIP_DISCARD` en el device consumidor. El output remoto se
  copia a sus backbuffers y se registran `Present`, frames completados y tiempo
  QPC; la ruta usa fence CPU y no declara sincronización GPU-native.
- En el host dual RTX 3090, el primer intento A→B reproduce
  `CreateSwapChainForHwnd=0x80070057` bajo el perfil VKD3D de LUID duplicado.
  La orientación inversa pasa `Present=0x0`; no se confundió `EnumOutputs` con
  una salida real porque VKD3D expone salidas virtuales en ambos adapters.
- El runner implementa un fallback automático acotado: si la presentación
  falla y `MGPU_CROSS_ADAPTER_PRESENT_AUTO=1`, reintenta una sola vez con
  `MGPU_CROSS_ADAPTER_REVERSE=1` y ajusta los ordinales CUDA. La validación
  persistente más reciente completó 3/3 evaluaciones NR y 3/3 `Present`.
- Esto valida presentación en un host sintético, no una integración de juego:
  siguen pendientes recursos auténticos, simultaneidad local+remota, retorno
  al swapchain real y sincronización GPU-native (`E_NOTIMPL`).
- El experimento remoto-first añadió el control diagnóstico
  `MGPU_DLSSNR_SHUTDOWN_REMOTE_BEFORE_LOCAL=0`: libera el feature B pero
  conserva inicializado el runtime. En este host B y A devuelven `0x1` en
  Init/Create/Evaluate, pero el cierre termina igualmente en
  `device_removed=0x887a0005`; no se habilita como solución.
- `scripts/build_bridge.sh` ahora detecta antes de aplicar los parches de
  inserción sin contexto (`remote-persistent` y `frame-timing`), evitando
  duplicar declaraciones/telemetría cuando se recompila sobre un checkout ya
  parcheado.
- `mgpu-auto remote-selftest` incorpora el gate opt-in
  `MGPU_REMOTE_PRESENT=1`: propaga cantidad de frames, exige presentación
  exitosa y acepta la orientación efectiva del retry automático. La ejecución
  real más reciente informó `available=true`, 3/3 `Evaluate` y 3/3 `Present`;
  el reporte interno de `remote_ngx` también refleja ahora esos contadores.

## 2026-09-11 — matriz de handles Vulkan para imagen cross-device

- `tests/vulkan_cross_device_fd_probe.cpp` admite
  `MGPU_VK_EXTERNAL_MEMORY_HANDLE=dma-buf` además de `opaque-fd`, y el modo
  diagnóstico `MGPU_VK_DEDICATED_IMPORT=1` añade `VkMemoryDedicatedAllocateInfo`.
- En este host, `opaque-fd` sigue devolviendo `VK_ERROR_UNKNOWN` en
  `vkGetMemoryFdPropertiesKHR`; `dma-buf` devuelve `VK_SUCCESS` pero
  `memoryTypeBits=0`, y la asignación/importación destino falla en ambos casos
  con `VK_ERROR_OUT_OF_DEVICE_MEMORY`.
- El resultado no cambia el MVP CPU-gated: la ruta lineal CUDA/P2P sobre las
  allocations de imagen continúa pasando en ambas direcciones; la importación
  Vulkan directa como `VkImage` permanece pendiente.

## 2026-09-11 — ciclo remoto NGX multi-frame CPU-gated

- El bridge ahora admite `MGPU_DLSSNR_REMOTE_NGX_PERSISTENT=1`: después de
  cada fence CPU completada reutiliza el handle remoto y resetea allocator y
  command list, sin volver a crear el feature ni emitir transiciones de
  estado inconsistentes en los frames siguientes.
- El smoke admite `MGPU_NGX_FRAME_COUNT` y crea un command list D3D12 nuevo
  por frame. En la RTX 3090 dual real se validaron tres frames consecutivos:
  `Evaluate=0x00000001`, fences `1/1`, `2/2`, `3/3`,
  `device_removed=0x00000000` y readback no nulo.
- `mgpu-auto` agrega el perfil opt-in
  `resource-fd-pair-worker-remote-ngx-persistent`, que exige el contador de
  frames del JSON además de retorno y validación FNV del output. Esto no
  convierte el camino en modo de juego ni resuelve simultaneidad local+remota.
- El perfil automático se ejecutó en ambas orientaciones físicas: A→B y B→A
  devolvieron `returncode=0`, 3/3 frames, fences completadas y
  `output_return_validation=ok`.
- El bridge ahora registra `remote_ngx_frame_timing` con QPC para cada frame:
  incluye `frame`, `elapsed_us` desde el inicio de Evaluate hasta la fence y
  un indicador de éxito. Es telemetría de transporte/NR, no una afirmación de
  que el frame haya sido presentado por un juego.
- El transporte continúa CPU-gated por diseño: GPU-native fence/semaphore
  sigue pendiente explícitamente debido a `E_NOTIMPL` de VKD3D.

## 2026-09-11 — MVP remoto NGX B-first y retorno P2P del output

- El bridge experimental crea un segundo device D3D12 con identidad física
  distinta, inicializa allí el runtime NR y crea `Reserved18` con el contrato
  de parámetros normalizado. En el host dual RTX 3090, Init/Create/Evaluate
  remotos devolvieron `0x00000001`.
- Se agregó una cola, command list y fence CPU en el device remoto. Las dos
  orientaciones pasaron con `remote_ngx_submit result=0x00000000`,
  `device_removed=0x00000000`, `completed=1` y `wait=0`.
- Se agregó un segundo `--resource-pair-daemon` opt-in que importa la
  allocation de output remoto y la allocation de presentación del device del
  juego, y ejecuta `cuMemcpyPeer` B→A. El log `dlssnr-proxy.log` registró
  `output_return_copy=ok response=OK ...` en A→B y B→A.
- La ruta quedó cerrada detrás de
  `MGPU_DLSSNR_REMOTE_NGX_INIT_PROBE=1`,
  `MGPU_DLSSNR_REMOTE_NGX_FEATURE=1` y
  `MGPU_DLSSNR_SKIP_LOCAL_NGX=1`. Esto es un MVP remoto experimental
  CPU-gated: omite NR local, no se habilita automáticamente en juegos y no
  declara presentación visual real.
- Se confirmó que inicializar primero NR local y luego el segundo device sigue
  provocando `device_removed=0x887a0005`; la simultaneidad local+remota queda
  pendiente. La variante de cargar una copia del runtime bajo otro nombre/path
  devolvió `0xbad00002` y tampoco se considera solución.
- GPU-native fence/semaphore y MFG remoto permanecen explícitamente
  pendientes; el puente sigue usando timeout y coordinación CPU.
- `mgpu-auto remote-selftest` incorpora el perfil opt-in
  `resource-fd-pair-worker-remote-ngx`: valida el JSON del smoke y, desde el
  offset del comienzo de cada corrida, las marcas de evaluación/fence/retorno
  del `dlssnr-proxy.log`. No cambia el estado conservador de `READY_REMOTE`.
- El worker de salida de un solo par hace `cuMemcpyDtoH` de la allocation
  destino, calcula FNV-1a y cuenta bytes no nulos; así no depende de que
  `__wine_unix_spawnvp` propague variables de entorno al proceso Linux hijo.
  El self-test automático exige además `output_return_validation=ok`. La
  corrida A→B/B→A más reciente observó FNV
  `0x9b19f872fe1f20e0` y `1559809` bytes no nulos en ambas orientaciones.
- Se añadió el perfil opt-in `resource-fd-pair-worker-sequential-dual`:
  después del pass remoto libera y apaga el feature B, re-inicializa NR en A,
  crea el feature local diferido y completa la cola sin `device_removed`. La
  corrida real obtuvo `remote_ngx_submit=0x0`, `local_after_remote_init=0x1`,
  `local_after_remote_create=0x1` y `DLSSNR Evaluate=0x1`. Es un fallback
  secuencial CPU-gated; el check de simultaneidad local+remota sigue abierto.
- Se repitió el experimento con `MGPU_DLSSNR_SKIP_LOCAL_NGX=0`: el runtime
  local completa Init/Create/Evaluate, pero el envío del command list remoto
  termina en `0x800705b4`, `device_removed=0x887a0005`, `completed=0` y
  `wait=258`. El camino local sigue funcionando después del fallo; por eso la
  simultaneidad local+remota continúa cerrada y el perfil automático exige
  explícitamente B-first.

## 2026-09-11 — pair-worker del bridge y selección de GPU física

- El bridge añade `MGPU_DLSSNR_TRANSPORT=resource-fd-pair-worker`: crea un
  `ID3D12Device` adicional, resources equivalentes para los cuatro planos,
  exporta source/destination FD y lanza el daemon CUDA de pares.
- Se detectó que el índice DXGI solicitado podía devolver el mismo UUID/PCI que
  el device del juego por los adapters con LUID duplicado de VKD3D. El bridge
  ahora prueba candidatos, consulta la identidad física por UUID/PCI y elige un
  device distinto; si no encuentra uno, falla cerrado.
- El smoke A→B registró `source=1 destination=0`, `resource_pair_daemon_imports_ready`
  y `resource_pair_daemon_copy copied=1 us=2141`. B→A registró
  `source=0 destination=1`, imports listos y `copy copied=1 us=2056`.
- La cadena fresca de parches se aplicó desde checkout limpio y compiló
  `bridge-nvngx.dll` y `_nvngx.dll`; el perfil también quedó expuesto en
  `mgpu-auto` como `MGPU_REMOTE_TRANSPORT=resource-fd-pair-worker`.
- Este check cierra transporte/copia de allocations creadas por el bridge,
  pero no NR remoto: NGX continúa evaluándose en el device local, no hay
  presentación desde B ni MFG, y la sincronización GPU-native sigue pendiente
  por `E_NOTIMPL`.

Este documento resume todo lo implementado durante el experimento Dual RTX 3090 / DLSS5 en Linux. Incluye resultados negativos: un stopper queda registrado aunque una prueba haya sido compilada correctamente.

## 2026-09-11 — daemon resource-FD conectado al bridge y shim de FDs seguro

- `cuda_external_p2p_copy_helper` añade `--source-daemon`: importa y mapea una
  vez `color`, `output`, `motion` y `depth`, conserva ambos contextos CUDA y
  atiende `c` (copia P2P + sincronización) y `q` (cierre) por TCP loopback.
- El bridge añade el modo opt-in
  `MGPU_DLSSNR_TRANSPORT=resource-fd-worker`. Usa
  `MGPU_CUDA_WORKER_HELPER` para el daemon y mantiene
  `MGPU_CUDA_IMPORT_HELPER` para el importador individual del smoke.
- Se corrigió un bloqueo sutil de `__wine_unix_spawnvp`: el shim anterior
  quitaba `FD_CLOEXEC` a todos los descriptores y también heredaba la tubería
  interna de spawn. Ahora acepta listas y argumentos numéricos de `execvp` y
  sólo marca como heredables FDs abiertos que corresponden a los recursos.
- La cadena reproducible del bridge aplica desde checkout limpio el parche de
  resource-FD y el nuevo worker, enlaza `-lws2_32` y compila ambos DLL.
- Smoke A→B y B→A pasó con los cuatro imports en `CUDA_SUCCESS`, respuesta
  `OK` del daemon, NGX `Init/Create/Evaluate=0x00000001`, readback no nulo y
  sin procesos/socket residuales. El transporte de tres planos repetido ocho
  veces también pasó en ambas direcciones.
- Se añadió el hook opt-in `MGPU_DLSSNR_WORKER_TEST_REPEAT=N`. Con `N=8`, una
  sola conexión del bridge atendió 8 comandos `c` A→B y 8 B→A, con 16/16
  respuestas `OK` y sin recrear imports ni contextos CUDA. Esto valida el
  protocolo persistente, pero no equivale todavía a ocho frames de un juego.
- El helper añade `--resource-pair-daemon` y `--daemon-client`. El smoke puede
  importar simultáneamente los FDs fuente y destino de los tres planos y
  escribir directamente en las allocations D3D12 de B. Con ocho comandos por
  orientación, A→B y B→A completaron `Evaluate=0x00000001`, readback no nulo
  y `fnv1a=0xf0e542b22c97a119`, sin allocation CUDA destino intermedia.
- El smoke ahora devuelve también el output de NGX: exporta la allocation
  producida en B, la copia por un segundo daemon B→A a una allocation D3D12
  de A y valida un readback no nulo. A→B y B→A pasaron con
  `remote_output_returned=true`, `nonzero=6216988` y el mismo FNV.
- `mgpu-auto remote-selftest` acepta ahora `MGPU_REMOTE_TRANSPORT=resource-pair-daemon`
  y valida automáticamente `resource_daemon_mode`, output devuelto y readback
  no nulo; `resource-fd` sigue siendo el perfil de transporte sin round-trip.
- Este check valida el MVP de transporte CPU-gated; todavía no es NR remoto
  real: el daemon copia hacia allocations CUDA de diagnóstico, el host es
  sintético, no hay presentación desde B ni MFG, y la sincronización
  GPU-native continúa pendiente por `E_NOTIMPL`.

## 2026-09-10 — worker persistente de tres planos

- `cuda_external_p2p_copy_helper` incorpora `--pairs-repeat`: importa y mapea
  una vez las tres parejas `Color/Motion/Depth`, crea los dos contextos CUDA y
  repite `cuMemcpyPeer` sobre esos mappings sin relanzar el helper por cada
  iteración.
- `MGPU_CROSS_ADAPTER_PERSISTENT_FRAMES=N` conecta el modo al smoke
  resource-FD y agrega `persistent_worker_iterations` al JSON.
- Ocho iteraciones A→B pasaron con `copy_us=10404`; B→A pasó con
  `copy_us=11766`. Ambas conservaron readback D3D12, `Init/Create/Evaluate=1`
  y readback NGX no nulo.
- Esto cierra la reutilización persistente dentro de una corrida de prueba,
  pero no todavía un daemon/ring sincronizado con frames sucesivos de un juego:
  queda pendiente la señalización productor/consumidor y el worker todavía se
  inicia una vez por corrida.

## 2026-09-10 — bridge resource-FD real hacia CUDA/P2P y corrección de ordinales

- Se corrigió el parche `dlss5-linux-bridge-resource-fd-probe.patch`: ahora se
  aplica desde un checkout limpio del bridge y compila sin depender de hunks
  corruptos o de un árbol previamente modificado.
- El bridge exporta `color`, `output`, `motion` y `depth` del host D3D12 real y
  los entrega al helper CUDA en modo sólo lectura. El helper importa el FD en
  la GPU que realmente creó el recurso, ejecuta `cuMemcpyPeer` a la otra 3090 y
  valida checksum origen/destino.
- Se detectó y corrigió una inversión de identidad: NGX corre en el device B,
  por lo que el origen CUDA del probe del bridge es el ordinal de destino del
  frame y su destino es el ordinal de origen. Se agregaron
  `MGPU_CUDA_BRIDGE_SOURCE_ORDINAL` y `MGPU_CUDA_BRIDGE_DESTINATION_ORDINAL`,
  con fallback automático invertido.
- A→B pasó con los recursos del bridge usando CUDA `1→0`; B→A pasó usando
  CUDA `0→1`. En ambas direcciones los cuatro imports devolvieron
  `CUDA_SUCCESS`, `cuMemcpyPeer` y validación de readback pasaron, y NGX en el
  consumidor terminó `Init/Create/Evaluate=0x00000001` con readback no nulo.
- El runner oficial fuerza `VKD3D_EXPORT_RESOURCE_FD=1` cuando se solicita
  `MGPU_DLSSNR_TRANSPORT=resource-fd-probe`, antes de crear recursos; esto es
  necesario porque un recurso ya creado no puede volverse exportable después.
- El resultado sigue siendo una prueba de transporte/ejecución de laboratorio:
  el host usa el registro de compatibilidad del sample, la espera es CPU-gated,
  no hay presentación remota ni MFG, y la sincronización GPU-native continúa
  pendiente por `E_NOTIMPL`.

## 2026-09-10 — puente D3D12 resource-FD con CUDA P2P para tres planos

- `d3d12_cross_adapter_frame_smoke` añade `MGPU_CROSS_ADAPTER_RESOURCE_FD=1`.
- El modo exporta `Color`, `MotionVectors` y `Depth` en A y las texturas
  equivalentes en B, importa las seis FDs en CUDA y ejecuta `cuMemcpyPeer` sobre
  cada allocation completa; no usa staging de RAM ni el import Vulkan
  cross-device que el driver rechaza.
- A→B pasó con `source=0`, `destination=1`: color `5.767.168` bytes y
  motion/depth `983.040` bytes cada uno; los tres helpers validaron FNV y el
  readback D3D12 devolvió color `00340038003a003c`.
- B→A pasó con `source=1`, `destination=0`, incluyendo readback de los tres
  recursos, usando el selector VKD3D experimental y las UUID físicas correctas.
- El script activa automáticamente `VKD3D_EXPORT_RESOURCE_FD=1` en ese modo.
- Esto completa un transporte de textura sintética CPU-gated; no declara NR
  remoto: inputs auténticos, sincronización GPU-native, presentación B y MFG
  continúan pendientes.

## 2026-09-10 — NGX sobre los tres resource-FD en B

- El modo resource-FD ya no desactiva NGX: después de trasladar `Color`,
  `MotionVectors` y `Depth`, los conserva como texturas D3D12 del consumidor B.
- A→B completó `Init=0x00000001`, `Create=0x00000001`,
  `Evaluate=0x00000001` y readback NGX no nulo (`nonzero=6216988`,
  `fnv1a=0xf0e542b22c97a119`).
- B→A repitió los mismos gates con CUDA `1→0` y el selector físico VKD3D
  invertido; también pasó el readback de color, motion y depth.
- `mgpu-auto remote-selftest` acepta `MGPU_REMOTE_TRANSPORT=resource-fd` y
  exige además `resource_fd_mode` y `resource_planes_readback`. El transporte
  lineal sigue siendo el perfil por defecto.
- Continúa siendo un MVP sintético CPU-gated: no usa recursos de un juego real,
  no tiene fence/semaphore GPU-native, no presenta desde B y no habilita MFG.

## 2026-09-10 — helper CUDA multi-pair

- `cuda_external_p2p_copy_helper` incorpora el modo `--pairs` para importar y
  copiar `Color`, `MotionVectors` y `Depth` en una única invocación.
- El smoke resource-FD deja de lanzar tres procesos: los tres `cuMemcpyPeer`
  pasan por el mismo par de contextos CUDA y mantienen validación FNV/readback.
- El tiempo de transporte observado baja a aproximadamente `0,31 s` desde el
  smoke (el total continúa dominado por crear Proton/prefix y cargar NGX).
- Esto prepara el siguiente paso de un worker persistente/ring; la
  sincronización GPU-native permanece pendiente explícitamente.

## 2026-09-10 — Deduplicación Vulkan y bloqueo FD físico cross-device

- Se añadió `mgpu-vulkan-cross-device-fd-probe` para probar exportación FD,
  importación y bind entre los dos devices Vulkan sin Wine.
- El loader expone cuatro handles NVIDIA; se añadió deduplicación por UUID y se
  confirmó que sólo hay dos GPUs físicas (`af:6d:e4:b3` y `5b:9f:38:5f`).
- Con esos dos UUIDs distintos, la exportación FD pasa (`VK_SUCCESS`), pero la
  importación en la segunda GPU devuelve `VK_ERROR_OUT_OF_DEVICE_MEMORY`.
  `vkGetMemoryFdPropertiesKHR` también devuelve `VK_ERROR_UNKNOWN`.
- La corrida previa que parecía importar y bindear correctamente era un falso
  positivo de dos handles de la misma GPU; queda corregida en el probe y en el
  plan.
- Se identificó que `win32u_vkAllocateMemory` trata
  `VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR` como `Unhandled sType`, y se dejó
  `patches/wine-win32u-import-memory-fd.patch` para conservar el `pNext`.
- Se intentó compilar un `win32u.so` parcial desde el submódulo GE; no se dejó
  instalado porque no comparte la ABI completa del GE-Proton distribuido. La
  prueba se detuvo, se limpiaron sus procesos y se restauró el módulo original.
- Próximo check: investigar una asignación/representación nativa realmente
  importable entre UUIDs distintos, manteniendo como fallback la ruta lineal
  CUDA/P2P. NR remoto, GPU-native sync y MFG siguen sin habilitarse.

## 2026-09-10 — SPI de recursos D3D12 y binding directo en GPU B

- Se añadió `ID3D12DXVKInteropDevice6::ExportVulkanResourceFd`, opt-in con `VKD3D_EXPORT_RESOURCE_FD=1`, y el marcado `VkExportMemoryAllocateInfo` en las asignaciones reales de recursos comprometidos.
- El bridge ahora tiene `MGPU_DLSSNR_TRANSPORT=resource-fd-probe`: exporta `color`, `output`, `motion` y `depth` auténticos del host oficial y los entrega al helper Vulkan de la segunda 3090.
- El helper admite `bind-only` por argumento y formatos adicionales. En una corrida limpia, los cuatro recursos obtuvieron `vkBindImageMemory=VK_SUCCESS` en GPU B; el helper registró los resultados en el log del host.
- Se validó el patch chain desde checkout limpio de VKD3D y bridge, incluyendo build cruzado y host oficial Donut: `EvaluateFeature=0x00000001`, `DLSSNR Evaluate=0x00000001`, watchdog esperado `return_code=124`.
- Esto supera la barrera de exportar únicamente el heap privado. Sigue sin ser NR remoto: no se ejecuta aún el pass NGX sobre esas imágenes en B, la sincronización GPU-native continúa pendiente (`E_NOTIMPL`) y MFG/presentación remota permanecen fuera de alcance.

## 2026-09-10 — `fd-probe` del host oficial validado hasta CUDA/P2P

- `scripts/run_official_d3d12_host_probe.sh` ahora propaga al proceso Proton el helper CUDA, ordinales de origen/destino, flags de exportación VKD3D, `LD_PRELOAD` opcional y la ruta `MGPU_CUDA_HELPER_LOG`.
- Se añadió logging opcional al helper `cuda_external_import_helper`; así el diagnóstico no depende de que Wine/Proton reenvíe el `stderr` del proceso nativo hijo al log del host.
- En una ejecución fresca del host oficial Donut con shaders y runtimes aislados se observó:
  - `fd_probe export hr=0x00000000`, FD `130`, tamaño `1966080` bytes.
  - `cuImportExternalMemory=CUDA_SUCCESS` y `cuExternalMemoryGetMappedBuffer=CUDA_SUCCESS`.
  - Escritura `cuMemsetD8` correcta, `cuMemcpyPeer` A→B correcto y readback completo.
  - `cuda_helper_p2p_validation=ok`.
  - El host continuó `DLSSNR Evaluate result=0x00000001` en frames 1 y 300; el `return_code=124` sigue siendo el watchdog esperado del demo persistente.
- Este resultado valida el transporte del heap de output real del host hacia CUDA/P2P. No demuestra todavía que `Color`, `MotionVectors` y `Depth` del juego se ejecuten en GPU B, ni que el output vuelva a la presentación desde B.
- Permanecen pendientes y cerrados por diseño: recuperación sin shim de prueba, sincronización GPU-native D3D12/Vulkan (`E_NOTIMPL`), inputs auténticos, presentación remota, `READY_REMOTE` y MFG.

## 2026-09-10 — Auditoría adicional del stopper de fence

- Se reconstruyó VKD3D-Proton desde un checkout limpio con la cadena de parches y se probó una variante que habilita `VK_KHR_external_semaphore_fd` y selecciona `VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT` únicamente cuando `VKD3D_EXPORT_FENCE_FD=1`.
- La variante compila correctamente, pero el resultado no cambia: bajo Proton, la enumeración de extensiones de dispositivo devuelve `external_semaphore_fd=no`, `external_fence_fd=no` y `vkGetSemaphoreFdKHR=null`. El probe sigue devolviendo `ExportVulkanFenceFd: 0x80004001 (E_NOTIMPL)`.
- Como control, `vulkaninfo` nativo del mismo host/driver 595.71.05 sí muestra `VK_KHR_external_semaphore_fd` y `VK_KHR_external_fence_fd` para las RTX 3090. La diferencia queda localizada en el loader/driver visible desde el camino PE/Wine-VKD3D, no en la ausencia global de soporte Vulkan del sistema.
- No se promociona esa variante experimental a `build/proton`: la ruta operativa permanece en CPU-gated y no se arriesga el host oficial que ya alcanza `EvaluateFeature`.

## 2026-09-10 — runner reproducible y evaluación mínima completada

- Se corrigió `run_official_d3d12_host_probe.sh` para propagar al proceso Proton los flags de traza, evaluación mínima y probes del bridge.
- El runner ahora detecta y copia automáticamente `libgcc_s_seh-1.dll`, `libstdc++-6.dll` y `libwinpthread-1.dll` cuando el host fue cross-compilado con MinGW. Sin esas DLL, Wine terminaba con `return_code=53` antes de ejecutar la primera etapa del host.
- Se validó nuevamente el bridge recompilado desde la cadena completa de parches con el host Donut instrumentado: `return_code=0`, `device_created=true`, `bridge_log=true`, `bridge_evaluated=true`.
- El runner ahora detecta `donut/shaders` junto al ejecutable o acepta `MGPU_OFFICIAL_HOST_SHADER_DIR`, y lo copia al staging. La ausencia de esos blobs era la causa del bloqueo antes de `CommonRenderPasses`.
- La traza del host llegó a `minimal_eval_end` y `feature_supported`. El bridge registró cuatro recursos nativos distintos (`HDR`, `output`, `motion`, `depth`), `DLSS standard EvaluateFeature=0x00000001` y `DLSSNR Evaluate=0x00000001`.
- Al quitar `MGPU_OFFICIAL_HOST_SKIP_HIGH_LEVEL`, la misma build alcanzó `common_passes_ready`, `shadow_depth_ready`, cargó la escena y produjo `DLSSNR Evaluate=0x00000001` en frames 1, 300 y 600 antes del watchdog esperado (`return_code=124`).
- Se limpiaron los temporales propios identificados con prefijo `dlss5-*` después de verificar la papelera; se conservaron sólo fuentes/runtimes necesarios para continuar. El filesystem quedó con aproximadamente `70 GiB` libres (`60%` usado), sin cambios en RandR/Xorg.
- Esto completa la evaluación mínima local/host de laboratorio, no una integración de juego: el registro de recursos sigue siendo un shim de prueba, la ruta `CommonRenderPasses` completa aún requiere validación, GPU-native semaphore/fence continúa pendiente y `READY_REMOTE`/MFG siguen cerrados.

## 2026-09-10 — host oficial Donut: staging NGX, ABI de recursos y evaluación mínima

- Se corrigió `scripts/run_official_d3d12_host_probe.sh` para separar el core `_nvngx_real.dll` generado por GE-Proton del runtime DLSS limpio `nvngx_dlss_real.dll`. También crea automáticamente un prefix de bootstrap aislado y conserva el watchdog por proceso-grupo.
- El sample oficial Donut se recompiló desde Linux como PE x86-64: `101/101` objetivos, con shaders, NVRHI, escena y ejecutable.
- La instrumentación del host confirmó `D3D12_Init`, `GetCapabilityParameters`, inicialización DLSS y DLSSNR positivas. La lectura pública `SuperSampling_Available` retorna éxito con valor `0`; el getter de diagnóstico que seguía a esa lectura se bloquea, por lo que se dejó un bypass opt-in sólo para continuar la traza.
- El host mínimo ahora crea cuatro recursos NVRHI/D3D12 distintos y alcanza `minimal_eval_begin` bajo GE-Proton/VKD3D. En la primera corrida el runtime no retornó porque todavía faltaban las DLL runtime de MinGW del ejecutable; ese diagnóstico quedó resuelto en la sección más reciente.
- Se identificó una incompatibilidad entre la ABI pública de parámetros NGX y la ABI compacta usada por el bridge: el getter no recuperaba los punteros D3D12 reales.
- Se añadió el registro opt-in `NVSDK_NGX_Compat_GetD3D12Resource` al shim de compatibilidad del host de prueba y `patches/dlss5-linux-bridge-host-resource-registry.patch`. La última traza recupera correctamente cuatro recursos nativos distintos (`HDR`, `output`, `motion`, `depth`). El registro no existe en un juego real y no se presenta como solución de producción.
- Resultado de esa etapa intermedia: inicialización y creación de feature alcanzadas; la evaluación quedó pendiente hasta completar el staging del runtime del ejecutable. `READY_REMOTE`, presentación visual, sincronización GPU-nativa y MFG remoto permanecen cerrados.

## 2026-09-10 — Build cruzado del host Donut D3D12 y nuevo stopper del SDK NGX

- Se comprobó que el snapshot del sample puede configurarse para Windows desde Linux con MinGW usando el DXC incluido en `donut/thirdparty/chk/dxc` y un import library reproducible generado desde los 59 exports de la DLL NGX disponible.
- Se añadieron parches reproducibles, sin modificar el snapshot externo: compatibilidad SIMD MinGW, exclusión de audio cuando `DONUT_WITH_AUDIO=OFF`, `#include <cstring>` faltantes en Donut/NVRHI, selección DXGI pura cuando NVAPI está desactivado, debug D3D12 opcional y guard de inicialización NVAPI del demo.
- El build cruzado alcanzó `100/100`: compiló `donut_core`, `donut_engine`, `donut_render`, `donut_app`, NVRHI D3D12 y los 60 shaders DXIL. Esto confirma que `CommonRenderPasses`, `ShaderFactory`, `TextureCache`, escena y app no son un límite intrínseco del toolchain.
- El enlace final de `ngx_dlss_demo.exe` queda bloqueado porque la DLL NGX no contiene los wrappers del SDK que normalmente aporta `nvsdk_ngx*.lib`: `NVSDK_NGX_Parameter_*`, `NVSDK_NGX_D3D12_DestroyParameters`, `NVSDK_NGX_UpdateFeature` y `GetNGXResultAsString`. No se inventan implementaciones ABI dentro del runtime; el stopper queda separado del problema D3D12/VKD3D.
- Se mantiene la política de no habilitar `READY_REMOTE`: todavía no hay host real recompilado ejecutable, inputs auténticos de juego, identidad física estable dentro de VKD3D ni sincronización GPU-nativa D3D12.

## 2026-09-10 — Diagnóstico del host auténtico y watchdog por proceso-grupo

- Se repitió el sample oficial D3D12 bajo GE-Proton 11-6 con runtime DLSS limpio, bridge instrumentado y VKD3D experimental durante 120 s. El resultado fue `return_code=124`, `device_created=true`, `ngx_loaded=false`, `bridge_log=false`, `bridge_evaluated=false`.
- Una traza acotada confirmó que el host encuentra `d3d12.dll`, `d3d12core.dll`, `media/sponza.json` y el directorio `Sponza`; después de crear el device queda en waits internos. No se atribuye el bloqueo a una DLL o textura ausente.
- La escena mínima opt-in (`tests/fixtures/ngx_empty_scene.json`, `models=[]`) reproduce exactamente el mismo `device_created=true` sin `ngx_loaded`; el bloqueo ocurre antes de la carga de escena/NGX, probablemente en la inicialización de ventana/swapchain o del backend D3D12.
- El sample nativo Vulkan del mismo paquete cargó texturas Sponza reales y consultó requisitos NGX (`GetFeatureRequirements=0x00000000`, `Min GPU Arch=0x160`), pero también quedó cargando antes de registrar un `EvaluateFeature` dentro de 120 s. Esto es evidencia de host/escena bloqueado, no una evaluación auténtica completada.
- `scripts/run_official_d3d12_host_probe.sh` ahora ejecuta Proton en una sesión aislada y mata el proceso-grupo completo al vencer el watchdog. La prueba de regresión devolvió `124` sin procesos `ngx_dlss_demo` ni procesos CUDA residuales.
- El host auténtico sigue pendiente: no se habilita `READY_REMOTE`, no se usa esta evidencia para declarar NR remoto y la sincronización GPU-nativa continúa pendiente explícitamente.

## 2026-09-10 — MVP combinado bidireccional A↔B

- El launcher acepta `MGPU_CROSS_ADAPTER_REVERSE=1` y, sin ordinales adicionales, invierte la topología: fuente física B/CUDA1 → destino físico A/CUDA0. Para evitar que VKD3D reutilice la primera selección global, el smoke fuerza `VKD3D_DUPLICATE_LUID_INDEX=1` durante la creación del productor y `=0` durante la del consumidor.
- La ejecución directa B→A pasó con los tres rangos (`Color`, `MotionVectors`, `Depth`), importación de ambos heaps, tres `cuMemcpyPeer`, validación FNV completa, reconstrucción D3D12 y readback correcto.
- NGX sobre el device consumidor A también completó `Init/Create/Evaluate=0x00000001` y readback no nulo (`nonzero=6216988`, `fnv1a=0xf0e542b22c97a119`). El JSON reportó `reverse_direction=true`, `source_cuda_ordinal=1`, `destination_cuda_ordinal=0`.
- Se repitió A→B después de corregir el orden de argumentos del JSON: `reverse_direction=false`, CUDA `0→1`, todos los gates positivos. El transporte observado fue ~0,30–0,35 s y la cola B+NGX ~18 ms; el total sigue dominado por el arranque de Proton.
- Esto completa el MVP sintético lineal en ambas orientaciones, no la importación directa de `VkImage`: la orientación física inversa de esa ruta continúa fallando con `VK_ERROR_OUT_OF_DEVICE_MEMORY`. Tampoco habilita todavía `READY_REMOTE`, presentación de juego, evaluación simultánea A+B, sincronización GPU-nativa ni MFG.

## 2026-09-10 — MVP combinado: textura A→B y evaluación NGX en B

- El smoke `d3d12_cross_adapter_frame_smoke` ofrece `MGPU_NGX_CROSS_ADAPTER=1` (por defecto): transporta `Color`, `MotionVectors` y `Depth` desde A a B dentro de un heap FD compartido, mediante tres rangos lineales y tres operaciones `cuMemcpyPeer`, reconstruye los tres recursos D3D12 en B y los entrega a un feature NGX creado/evaluado sobre el device B.
- La prueba pasó con A=`pci=0:1:0.0`, B=`pci=0:3:0.0`, tres validaciones byte-level/FNV, `Init/Create/Evaluate=0x00000001`, fence CPU de B correcta y readback NGX de `7.372.800` bytes, `nonzero=6216988`, `fnv1a=0xf0e542b22c97a119`.
- El launcher genera automáticamente el `_nvngx_real.dll` de GE-Proton en un prefix aislado si no se proporciona `MGPU_NGX_CORE_DLL`; copia por separado core, runtime DLSS real y `nvngx_dlssnr.dll`, evitando la recursión proxy/runtime que había producido `0xbad00000`.
- `MGPU_NGX_CROSS_ADAPTER=0` conserva el modo de transporte sin NGX. Ambos modos son probes de laboratorio: los tres planos son sintéticos y ahora cruzan A↔B; no es todavía una integración de juego ni presentación.
- `mgpu-auto remote-selftest --json` automatiza el probe combinado y exige siete gates: transporte A→B, P2P, dos fences CPU, readback D3D12, evaluación NGX en B y readback NGX. La ejecución real devolvió `available=true`.
- El helper CUDA ahora procesa los tres rangos en una sola invocación: importa cada heap una vez, ejecuta las tres `cuMemcpyPeer` y valida cada FNV. En la ejecución medida, el transporte completo tomó `301314 µs` y la cola/fence de B `17623 µs`; el tiempo total de `4520599 µs` está dominado por el arranque de Proton/prefix.
- La espera entre productor y consumidor sigue siendo CPU-gated. GPU-native semaphore/fence, evaluación simultánea A+B, inputs auténticos de un juego y MFG remoto continúan pendientes.

## 2026-09-10 — Transporte de textura cross-adapter A→B con CUDA P2P

- Se añadió `tests/d3d12_cross_adapter_frame_smoke.cpp` y `scripts/run_d3d12_cross_adapter_frame_probe.sh`.
- El probe crea devices D3D12 físicos distintos en el mismo proceso (`A=0:1:0.0`, `B=0:3:0.0`), renderiza una textura RGBA16F en A y la copia a un buffer lineal de A.
- `cuda_external_p2p_copy_helper` importa los dos heaps VKD3D por FD, mapea los offsets de los buffers y ejecuta `cuMemcpyPeer` A→B. La validación compara todos los bytes y los FNV-1a de origen/destino; pasó con `source_fnv1a=destination_fnv1a=0xcd8c6d79f91c8383`.
- B reconstruye la textura mediante `CopyTextureRegion` y el readback D3D12 coincide con el pixel esperado `00340038003a003c`; resultado JSON: `helper_p2p=true`, `queue_a_cpu_fence=true`, `queue_b_cpu_fence=true`, `readback_validation=true`, `bytes=1843200`.
- Este es un transporte de frame sintético real entre GPUs sin staging de RAM. La espera es CPU explícita; la evaluación NGX en B se añadió en la iteración siguiente como gate combinado.

## 2026-09-10 — Payload determinista y baseline del smoke NGX

- `tests/ngx_d3d12_smoke.cpp` ahora carga por GPU una entrada sintética reproducible de color, motion vectors y depth mediante buffers de upload y `CopyTextureRegion`; `MGPU_NGX_INPUT_VARIANT=0|1` selecciona dos payloads distintos.
- El smoke conserva un readback previo a `EvaluateFeature` y otro posterior, ambos protegidos por la fence CPU de la cola D3D12. Con `MGPU_NGX_OUTPUT_VARIANT=2` fijo, las dos variantes tienen el mismo baseline (`nonzero=3686400`, `bytes=7372800`, `fnv1a=0x096af4a380b90383`) y outputs distintos: input 0 (`nonzero=5881807`, `fnv1a=0x3a300cd59e971a6f`) e input 1 (`nonzero=6086251`, `fnv1a=0xe5da35ab3b4b797b`).
- La cadena positiva GE-Proton/bridge terminó con `EvaluateFeature=0x00000001`, `DLSSNR Evaluate result=0x00000001` y retorno 0 para las variantes 0 y 1.
- El resultado demuestra que la cadena ejecuta una escritura observable y que el resultado byte-level cambia al variar color/motion/depth con el seed de output fijo. Sigue sin demostrar calidad visual, atribución exclusiva a NR ni validación de DLSS5 en un juego real.
- La misma corrida confirma el stopper de identidad: ambos `ID3D12Device` del smoke reportan `pci=0:1:0.0`; todavía no se está ejecutando una evaluación en GPU B dentro del mismo proceso.
- `run_ngx_test.sh` ahora elimina automáticamente sus copias `mktemp` de bridge y logs al terminar; `MGPU_NGX_KEEP_TEMP=1` conserva esos artefactos para depuración. Los prefixes/runtimes externos no se eliminan.
- La corrección del smoke usa el estado válido `GENERIC_READ` para buffers `UPLOAD`, mantiene upload/baseline/evaluación en un único command list y separa `MGPU_NGX_INPUT_VARIANT` de `MGPU_NGX_OUTPUT_VARIANT` (default 2) para poder medir sensibilidad sin confundir el seed del output.
- El launcher propaga dos variables opt-in para el siguiente gate: `MGPU_NGX_SECOND_DEVICE_FIRST=1` hace que B inicialice NGX primero y `MGPU_NGX_EVALUATE_SECOND_DEVICE=1` crea recursos, evalúa y espera la cola de B con una fence CPU.
- En el modo VKD3D experimental con adapters físicos distintos, B-first completó esa evaluación local sintética: B=`pci=0:3:0.0`, `EvaluateFeature=0x00000001`, cierre/ejecución/fence `0x0` y readback `bytes=7372800`, `nonzero=4594848`, `fnv1a=0x3c413a88d2048413`. A, inicializada después, devolvió `CreateFeature=0xbad00007`; esto confirma el estado global de NGX y no es NR remoto.
- Se añadió `run_ngx_same_process_b_probe.sh` para repetir ese gate automáticamente en un probe aislado, verificando identidad A/B, evaluación y readback de B, fence CPU y la barrera global esperada de A.
- Se liberaron temporales regenerables de `/tmp` y paquetes comprimidos ya extraídos de `Juegos`; se conservaron fuentes, runtimes y sample necesarios para continuar. No se modificó RandR/Xorg.

## 2026-09-10 — Matriz de NGX aislado por proceso y verificación física A/B

- El smoke registra la identidad que VKD3D expone para cada `ID3D12Device` mediante `ID3D12DXVKInteropDevice5`, incluyendo UUID abreviado y PCI.
- Se añadió `scripts/run_ngx_process_isolation_matrix.sh`, que ejecuta procesos Proton separados con `VKD3D_DUPLICATE_LUID_INDEX=0` y `=1`, y exige la identidad PCI esperada.
- La matriz pasó en ambos procesos: A `uuid=af:6d:e4:b3 pci=0:1:0.0` y B `uuid=5b:9f:38:5f pci=0:3:0.0`; ambos obtuvieron `EvaluateFeature=0x00000001`, `DLSSNR Evaluate result=0x00000001` y retorno positivo.
- Esto separa el bloqueo de estado global dentro de un proceso del soporte del runtime en hardware: la 3090 B sí puede ejecutar el chaining local cuando tiene su propio proceso/device.
- No se declara NR remoto: siguen faltando el transporte de color/motion/depth, sincronización de productor/consumidor, evaluación NGX en B dentro de la misma cadena y presentación desde B.

## 2026-09-10 — Probe reproducible del host oficial D3D12

- Se añadió `scripts/run_official_d3d12_host_probe.sh`: copia el sample, media, bridge, runtimes aportados por el usuario y VKD3D experimental a un directorio temporal, y ejecuta `ngx_dlss_demo.exe -d3d12` con watchdog.
- El probe reporta en JSON si el proceso arrancó, creó el device, cargó `nvngx_dlss.dll` y produjo evaluaciones del bridge; con `MGPU_OFFICIAL_HOST_REQUIRE_NGX=1` puede convertirse en gate estricto.
- Ejecución actual: `return_code=124`, `started=true`, `device_created=true`, `ngx_loaded=false`, `bridge_log=false`, `bridge_evaluated=false`. El proceso llegó a crear el device, pero no alcanzó NGX durante 45 s.
- El resultado mantiene abierto el check de host real; no se atribuye todavía la causa a NGX, al bridge o a VKD3D. La próxima iteración debe instrumentar la carga de escena/arranque o usar un host D3D12 mínimo con evaluación observable.

## 2026-09-10 — Ejecución y readback del command list NGX D3D12

- El smoke `ngx_d3d12_smoke` ahora crea una cola D3D12, cierra y envía el command list que contiene `EvaluateFeature` y la copia del output a un readback.
- La finalización se espera mediante una `ID3D12Fence` y un evento de CPU; los resultados `WAIT_TIMEOUT`/`WAIT_FAILED` ya no se silencian y bloquean el readback.
- La ejecución positiva bajo GE-Proton terminó con `queue=0x00000000`, `close=0x00000000`, `execute=0x00000000`, `wait=0x00000000`; el readback mapeó `7.372.800` bytes, con `921.600` bytes no nulos y `fnv1a=0xbcf8110a8e1d0383`.
- El host negativo produjo la misma firma. Por eso este check prueba que la cola y el recurso son ejecutables/legibles, pero no demuestra que DLSS/NR haya escrito un resultado visual significativo.
- No cambia los stoppers: NR remoto, recursos auténticos de un juego, identidad física estable dentro de VKD3D y sincronización GPU-nativa siguen pendientes.

## 2026-09-10 — Sincronización CUDA nativa entre GPUs

- Se añadió `mgpu-cuda-native-sync-probe`, que encadena `cudaEventRecord`, `cudaStreamWaitEvent` y `cudaMemcpyPeerAsync` entre las dos RTX 3090.
- El productor de A espera en GPU el evento de consumo de B antes de reutilizar cada slot; el CPU sólo retira completions y valida una muestra.
- Validación actual: 120/120 frames, checksum correcto, `gpu_native_waits=true`, aproximadamente 12,0 GB/s.
- Esto no cambia el stopper D3D12/VKD3D: `ExportVulkanFenceFd` continúa en `E_NOTIMPL`, por lo que `gpu_native_sync` del plan remoto permanece pendiente.
- `run_mgpu_mvp.sh` ahora publica `READY_CUDA_NATIVE_FRAME_SYNC_P2P` y `cuda_native_sync_p2p=1` cuando ese gate pasa; `game_launch` continúa deshabilitado.

## 2026-09-10 — Primer acceso GPU a imagen importada en B

- Se añadió `mgpu-vulkan-image-import-helper` para reconstruir una `VkImage` RGBA16F en el segundo device Vulkan a partir del FD del heap D3D12.
- El helper ejecuta en B una secuencia real `vkCmdClearColorImage → vkCmdCopyImageToBuffer` y valida el readback en memoria host.
- Resultado `A=0 → B=1`: `bind_result=VK_SUCCESS`, `gpu_access_result=VK_SUCCESS`, `gpu_access_stage=gpu_clear_copy_readback` para 1280×720 (`7.864.320` bytes).
- La repetición física `A=1 → B=0`, usando `VKD3D_DUPLICATE_LUID_INDEX=1`, mantiene el buffer CUDA `cuImportExternalMemory`/P2P correcto, pero la importación de la imagen devuelve `VK_ERROR_OUT_OF_DEVICE_MEMORY`. La prueba anterior que parecía inversa no era válida porque VKD3D había seleccionado GPU0 como origen.
- Se añadió `VkExternalMemoryImageCreateInfo` y consulta opcional de `vkGetMemoryFdPropertiesKHR` al helper; el resultado inverso no cambia, por lo que el fallo queda atribuido al camino de importación de imagen del driver/interop y no se maquilla como éxito.
- Es un check de representación/uso de memoria cross-device; todavía no conecta las imágenes auténticas color/motion/depth del juego con un feature NGX en B.

Regresión de esta iteración: CMake correcto, `12/12` tests Python, `bash -n` y `git diff --check` correctos.

## 2026-09-10 — Bypass lineal de imagen con CUDA P2P

- Se añadió `mgpu-vulkan-image-cuda-p2p-probe`, que crea una imagen RGBA16F equivalente en cada 3090, exporta ambas asignaciones y las mapea como buffers CUDA.
- La copia cruda de la asignación con `cudaMemcpyPeer` y el readback Vulkan pasan en `0→1` y `1→0`; ambas asignaciones son de `7.864.320` bytes.
- `mgpu-auto doctor`/`selftest` ahora ejecutan y reportan este gate en las dos direcciones.
- En ese punto del desarrollo todavía faltaba producir un buffer lineal desde una textura D3D12 real; ese check se completó en la sección siguiente. Sigue faltando coordinar el recurso auténtico de un juego con NGX en B.
- Regresión final de esta iteración: CMake correcto, `13/13` tests Python, `bash -n`, `git diff --check` y `mgpu-auto selftest.passed=true`.

## 2026-09-10 — Textura D3D12 a buffer lineal sin staging de RAM

- Se añadió `vkd3d_d3d12_texture_linear_smoke.exe` y su launcher Linux/Proton.
- El smoke crea una textura D3D12 RGBA16F de 1280×720, la limpia, obtiene el footprint (`row_pitch=10240`, `7.372.800` bytes), ejecuta `CopyTextureRegion` a un buffer colocado y espera una `ID3D12Fence` desde CPU.
- El heap del buffer se exporta con la SPI VKD3D; `cuda_external_readback_helper` importa la asignación, ejecuta CUDA P2P y valida el primer pixel (`00340038003a003c`).
- Resultado: correcto en `GPU0→GPU1` y `GPU1→GPU0`, con selección física experimental de UUID/PCI.
- Se añadió `run_d3d12_texture_linear_matrix.sh`, que ejecuta ambos sentidos en prefixes aislados y valida automáticamente exportación, importación CUDA y readback; la matriz pasó en los dos sentidos.
- La sincronización es CPU-gated y sólo sirve como fallback de laboratorio; no cambia el gate pendiente de semaphore/fence GPU-nativo ni conecta todavía el recurso real de un juego con NGX en B.

## 2026-09-10 — Guardia contra recursión del runtime NGX

- Se reprodujo el timeout de `Init_Ext` con evidencia de un loop de llamadas: el archivo indicado como `nvngx_dlss_real.dll` contenía en realidad el proxy (`_nvngx_real.dll` y `bridge-nvngx.dll`).
- Se confirmó la causa comparando runtimes: con el DLL limpio del SDK (`sha256=3975567b...`) el host GE-Proton llega a `CreateFeature=0x00000001`, `EvaluateFeature=0x00000001` y finaliza; con el proxy autocopiado (`sha256=2c6ccd6e...`) se repite `real core Init_Ext` hasta el watchdog.
- `scripts/run_ngx_test.sh` ahora acepta `DLSS_RUNTIME_DLL`, detecta firmas de proxy, usa automáticamente el runtime limpio del SDK cuando está disponible y aborta con un mensaje accionable si no lo encuentra.
- La corrección es de aislamiento/diagnóstico del launcher; no implementa NR remoto ni sincronización GPU-nativa.

## 2026-09-10 — identidad física, evaluación y gate de sincronización

- Se habilitó de forma optativa `VK_KHR_external_semaphore_fd` en VKD3D para hosts Linux que realmente la anuncien.
- El probe ahora enumera extensiones activas y registra las capacidades externas del semáforo; en este host el driver no publica `VK_KHR_external_semaphore_fd` ni `VK_KHR_external_fence_fd`, y `vkGetSemaphoreFdKHR` queda nulo.
- Las capacidades abstractas devueltas por Vulkan (`features=0x3`, `opaque_fd=0x8`) no son suficientes para exportar un FD: falta la entrada de API del driver.
- Se agregó una prueba de sincronización alternativa mediada por CPU entre colas D3D12 de las dos 3090. Funciona (`cpu_fence_sync=available`) y queda como candidato para el MVP remoto con un gate de latencia explícito.
- `ExportVulkanFenceFd` conserva `E_NOTIMPL` cuando no existe un semaphore FD Vulkan; no se convierte artificialmente un eventfd en un handle GPU.

## 2026-09-10 — Frame ring CPU-gated de tres planos

- Se añadió `FramePlaneSizes` y `benchmark_cpu_synchronized_frame_ring()` para transportar color, motion y depth con un `frame_id` común.
- Se agregó `mgpu-cpu-sync-frame-probe`, con checksum independiente por plano, polling CPU, ring de slots y timeout de stall.
- El probe es sintético y valida el contrato de transporte; no declara NR remoto ni sincronización GPU-nativa.
- `run_mgpu_mvp.sh` y `mgpu-auto doctor` ahora reportan el gate multip plano por separado.
- El smoke NGX positivo ahora tiene watchdog también alrededor de Proton; la prueba del host sigue sin producir una evaluación utilizable y queda registrada como timeout.

## 2026-09-10 — MVP CPU-gated P2P

- Se implementó `benchmark_cpu_synchronized_ring()`: slots CUDA, polling CPU de eventos, validación de cada frame y timeout de stall.
- Se añadió `mgpu-cpu-sync-p2p-probe`, con salida JSON y parámetros de frames/slots/timeout.
- Se validaron las dos direcciones entre las RTX 3090: 120/120 frames correctos en cada dirección.
- `run_mgpu_mvp.sh` ahora informa `READY_CPU_SYNC_P2P` cuando P2P nativo, importación Proton y el ring CPU-gated pasan; el lanzamiento del juego continúa deshabilitado.
- `mgpu-auto doctor` expone `cpu_sync_p2p_available`; el campo `gpu_native_sync` queda explícitamente en `pending`.
- Este MVP sólo valida el transporte/ordenamiento de buffers. No declara todavía NR remoto ni sustituye la sincronización GPU-nativa.

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
- Build completo reproducible de VKD3D: los cinco parches se aplican/detectan y `d3d12.dll`/`d3d12core.dll` se instalan correctamente.
- Smoke de la SPI: heap de 65.536 bytes exportado; el helper ve un FD NVIDIA, importa/mapea, escribe, ejecuta `cuMemcpyPeer` y valida checksum en GPU1.
- Smoke automático del bridge: output 1280x720, heap de 7.864.320 bytes, export exitoso y helper con `spawn_rc=0`.
- El runtime NGX de la prueba sigue devolviendo `0xbad00005`; el resultado no prueba NR remoto ni MFG remoto.
- `vkGetMemoryFdPropertiesKHR` continúa en `-13` (`VK_ERROR_UNKNOWN`) bajo Wine; CUDA acepta el FD en la ruta heredada, pero el contrato Vulkan estándar sigue pendiente.
## 2026-09-10 — Smoke aislado de ventana y swapchain D3D12

- Se añadió `tests/d3d12_window_swapchain_smoke.cpp` y su runner
  `scripts/run_d3d12_window_swapchain_smoke.sh`.
- La prueba registra por separado `CreateWindowEx`, selección de adapter,
  `D3D12CreateDevice`, `CreateCommandQueue`, `CreateSwapChainForHwnd`,
  `GetBuffer`, ejecución/fence y `Present`.
- El runner usa un proceso-grupo Proton aislado, `PROTON_USE_XALIA=0` y un
  watchdog; no cambia RandR/Xorg ni habilita ningún modo remoto.
- La primera ejecución con VKD3D experimental completó todas las fases,
  incluida `CreateSwapChainForHwnd`, fence local y `Present`, en GPU A; por
  lo tanto el bloqueo del sample oficial no se reproduce con una swapchain
  mínima.
- El perfil oficial ampliado también completó factory DXGI 2, tres buffers,
  `DXGI_SWAP_CHAIN_FULLSCREEN_DESC`, carga de NGX y lectura de los tres
  backbuffers. La envoltura mínima equivalente de RTV + clear + fence también
  completó correctamente en GPU A y GPU B. La selección B funciona, pero
  VKD3D continúa informando el mismo LUID lógico para ambos adapters; la
  identidad física UUID/PCI sigue siendo un check separado.
- La variante visible de 5 s también pasó y la comparación de `xrandr` antes y
  después conservó `DP-0` y `HDMI-1-0`; este smoke no cambia la topología de
  monitores.
- El código del sample reveló una fase aún no cubierta por el smoke:
  `EnumOutputs`/`GetDesc` y reposicionamiento GLFW antes de crear el device;
  se añadió como siguiente check aislado.
- La ruta equivalente reprodujo `DXGI_ERROR_NOT_FOUND (0x887a0002)` para
  `EnumOutputs` en GPU A y B. El sample trata ese resultado como fallback y
  sigue sin mover la ventana; el smoke ahora replica ese comportamiento. Esto
  documenta una carencia de outputs en VKD3D, pero no es una desconexión de
  RandR/Xorg.
- Se añadió `tests/nvrhi_d3d12_smoke.cpp` con
  `scripts/run_nvrhi_d3d12_smoke.sh`: compila el snapshot NVRHI del sample,
  crea `nvrhi::d3d12::Device` y envuelve los tres backbuffers mediante
  `createHandleForNativeTexture`. Queda como el siguiente check ejecutable
  para localizar el bloqueo del host oficial.
- El smoke pasó en GPU A y B también con `createFramebuffer`, la textura
  `CascadedShadowMap` 2048×2048×4 y `ShowWindow` posterior a la creación de
  recursos. `xrandr` conservó `DP-0` y `HDMI-1-0`; NVRHI/D3D12 queda
  descartado como causa del bloqueo en esas fases.
- La siguiente diferencia está en la capa Donut de alto nivel: creación de
  `CommonRenderPasses`/`ShaderFactory`, shaders, `TextureCache` y la carga
  asíncrona antes de `NGXWrapper`.
- Este experimento queda como diagnóstico del host oficial. La sincronización
  GPU-nativa D3D12/Vulkan sigue pendiente explícitamente.

## 2026-09-10 — limpieza de artefactos locales

- [x] Verificar que no hubiera procesos Wine/Proton, DLSS, NGX o vLLM usando los artefactos antes de borrar.
- [x] Eliminar `/home/cristian/Juegos/DLSS5-proton` (~1,5 GB), runtime auxiliar de pruebas ya no requerido.
- [x] Eliminar `/home/cristian/Juegos/DLSS5-community-runtime-v1.2.5` (~1,1 GB), conservando en `/tmp` sólo la DLL/headers mínimos necesarios para continuar.
- [x] Eliminar `/home/cristian/Juegos/DLSS5-3DRenderer` (~34 MB), demo descargada que no participa en el MVP actual.
- [x] Eliminar `/tmp/dlss5-vkd3d-proton` (~353 MB), copia temporal redundante del fuente.
- [x] Conservar las fuentes oficiales y repositorios en `Juegos`, además de los headers, DLL y sondas pequeñas que todavía requiere el desarrollo.
- [x] Eliminar después de la prueba las copias temporales del cross-build, import libraries, prefix Wine y host staging (~240 MB adicionales), dejando sólo cuatro artefactos de reanudación pequeños/identificables.
- [x] Verificar espacio posterior: 71 GB libres, 59% usado.

## 2026-09-10 — shim opt-in para el enlace del host Donut

- [x] Añadir `tests/fixtures/ngx_sdk_compat/ngx_sdk_compat.cpp` con wrappers para la interfaz pública de parámetros NGX, destrucción/update, conversión de resultados y las variantes `_EvaluateFeature_C`.
- [x] Añadir `patches/ngx-sdk-compat-source.patch`; el shim sólo se incorpora cuando el build recibe `NGX_SDK_COMPAT_SOURCE` y no reemplaza ni modifica el runtime propietario.
- [x] Repetir el build cruzado MinGW del sample oficial: 101 objetivos compilados y `ngx_dlss_demo.exe` enlazado correctamente como PE x86-64 de 2,9 MB.
- [x] Resolver el último fallo de build como staging, creando la ruta `lib/Windows_/rel/default` esperada por el post-build del sample.
- [x] Ejecutar el host en prefix Wine aislado como experimento de ejecución.
- [ ] Validar el host con Proton real: Wine 9.0 termina el sample sin log útil y el lanzamiento manual registra `stack overflow`; esto no demuestra evaluación NGX, DLSS ni NR.
- [ ] Mantener pendiente la evaluación real, el transporte a GPU B y la sincronización GPU-nativa.
