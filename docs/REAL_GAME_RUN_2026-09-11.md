# Prueba de demo real bajo Proton — 2026-09-11

## Preparación de contexto Steam/UMU — 2026-09-11

- La política directa admite el modo autenticado sólo con
  `MGPU_USE_STEAM=1 MGPU_STEAM_APPID=<appid>`.
- En ese modo exporta `UMU_USE_STEAM=1`, `UMU_ID=umu-<appid>`,
  `SteamAppId=<appid>` y `SteamGameId=<appid>`. Sin ese opt-in conserva
  `UMU_USE_STEAM=0`.
- La validación de entrada está cubierta por `57/57` tests. La prueba runtime
  queda pendiente hasta completar manualmente el login de Steam; no se
  automatizan credenciales.
- GPU-native permanece en `MGPU_CROSS_ADAPTER_GPU_NATIVE=0`; este cambio sólo
  prepara identidad Steam y no altera transporte, fences ni presentación.

## Bootstrap Steam Linux — 2026-09-11

- Se instaló `com.valvesoftware.Steam` desde Flathub en el scope del usuario.
- El sandbox tiene acceso sólo a las dos rutas Steam montadas:
  `/media/cristian/HDD extra/SteamLibrary` y
  `/media/cristian/Disco local/SteamLibrary`.
- El arranque de 20 segundos no produjo `libraryfolders.vdf`, `loginusers.vdf`
  ni `compatdata`; el wrapper quedó en la comprobación de permisos de input.
- Tras cerrar la advertencia de input, un arranque de 60 segundos completó el
  runtime, abrió `Steam Big Picture Mode` y conectó con los servidores. El log
  de login queda en `WaitingForCredentials` con `steamid=0`.
- Steam aún no registra las bibliotecas ni crea `compatdata`; el siguiente
  paso requiere login manual antes de usar `steam://rungameid` o `-applaunch`.
- El log de hardware enumeró entradas RTX 3090 repetidas con LUID `0`; no se
  usa esa identidad para seleccionar GPU y la validación física sigue siendo
  responsabilidad de VKD3D/PCI del proyecto.

## Auditoría de títulos Steam instalados — 2026-09-11

### No Man's Sky

- Ejecutable: `No Man's Sky/Binaries/NMS.exe`.
- El proceso llegó a cargar Vulkan y, en la corrida de auditoría,
  `sl.interposer.dll`. No se observó carga de `sl.dlss.dll`,
  `nvngx_dlss.dll`, `EvaluateFeature` ni `dlssnr-proxy.log`.
- Se repitió con `PROTON_ENABLE_NVAPI=1` y contexto Steam opt-in
  (`SteamAppId=275850`, `SteamGameId=275850`); el resultado no cambió.
- `WINEDEBUG=+file` mostró que el proceso termina antes de abrir una
  configuración gráfica persistente en el prefix temporal. No se modificó la
  instalación del juego.

### Resident Evil 4

- Ejecutable: `RESIDENT EVIL 4  BIOHAZARD RE4/re4.exe`.
- Sin contexto Steam y luego con `SteamAppId=2050650`, `SteamGameId=2050650`
  y `SteamClientLaunch=1`, el proceso cargó `steam_api64.dll` pero no llegó a
  `d3d12.dll`, `nvngx_dlss.dll` ni Streamline.
- Ambos intentos restauraron el DLL original; no produjeron log del bridge.

La conclusión de esta auditoría es que el wiring remoto ya conserva el contexto
necesario y expone NVAPI, pero el host aún necesita un cliente Steam/UMU real o
un título que active NGX desde el arranque. No se debe promocionar el modo
remoto a `EvaluateFeature` real con esta evidencia.

## Objetivo

Verificar si una demo Unreal/D3D12 real llega a cargar el runtime `nvngx_dlss.dll`
proxy y a producir una llamada observable al bridge CPU-gated remoto.

## Entorno aislado

- Demo: `CitySample_v4b` de Unreal Engine 5.
- Ejecutable: `Binaries/Win64/CitySample-Win64-Shipping.exe`.
- Runner: GE-Proton11-6.
- VKD3D: `build/proton-resource-pair-worker-experimental`.
- GPU render seleccionada: RTX 3090 PCI `0000:03:00.0`.
- GPU neuronal planificada: RTX 3090 PCI `0000:01:00.0`.
- Transporte: resource-FD pair-worker, sincronización CPU-gated, sin
  GPU-native (`MGPU_CROSS_ADAPTER_GPU_NATIVE=0`).

## Smoke de laboratorio posterior — remote-only CPU-gated

- Se reconstruyó el bridge con el parche reproducible
  `dlss5-linux-bridge-remote-create-fallback.patch` y se ejecutó con
  GE-Proton11-6 completo, no con el Proton incompleto de la corrida histórica.
- La política explícita fue `MGPU_DLSSNR_SKIP_LOCAL_NGX=1`,
  `MGPU_DLSSNR_REMOTE_NGX_FEATURE=1`, `MGPU_NGX_PRIME_SOURCE=0`,
  resource-FD pair-worker y `MGPU_CROSS_ADAPTER_GPU_NATIVE=0`.
- Resultado: `probe_return_code=0`, físicas distintas
  (`0000:01:00.0`/`0000:03:00.0`), `remote_only synthetic_handle`,
  `remote_only local EvaluateFeature skipped`,
  `remote_ngx_init/create/evaluate=0x00000001`, submit CPU-fence completado y
  `output_return_copy=ok output_return_validation=ok`.
- Esta corrida valida el MVP remoto con recursos sintéticos del probe y dos
  RTX 3090; no convierte la demo Unreal en una integración DLSS real. La
  exportación de fence GPU-nativa sigue devolviendo `0x80004005` y permanece
  pendiente.

## Corrida reproducible con VKD3D actual — 2026-09-11

- Se descargó el archivo oficial `CitySample_v4b.zip`, se verificó con
  `unzip -t` y se extrajo sólo en staging temporal.
- Se recompiló VKD3D-Proton actual con la cadena del proyecto, incluyendo el
  selector opt-in `VKD3D_DUPLICATE_LUID_INDEX_PER_DEVICE=1`; el build terminó
  `209/209` y produjo ambas DLL D3D12.
- GE-Proton11-6 se verificó con SHA-256
  `659f8d71f2f78659340120b20c1c5a1464aa138939332a1376dea22f6d2dc2e4`.
- Dos corridas directas de 60 s y 90 s llegaron a crear D3D12/swapchain. La
  política automática quedó `READY_REMOTE`, con render en PCI
  `0000:03:00.0`, GPU neuronal planificada en `0000:01:00.0` y
  `MGPU_CROSS_ADAPTER_GPU_NATIVE=0`.
- La ejecución con el VKD3D compilado explícitamente dentro del prefix
  temporal tampoco registró `VKD3D duplicate-LUID mode: per-device`; el log
  continuó mostrando `Multiple adapters found with LUID 03f4/03f2`.
- `run-stderr.log` contiene cero cargas observables de `nvngx_dlss.dll`, cero
  `EvaluateFeature` y cero eventos `remote_ngx_*`. Sólo aparecen consultas
  NVAPI de indicadores DLSSG. `nvidia-smi` observó actividad de render, pero
  no una reserva sostenida de VRAM en la GPU neuronal.

La política `mgpu-auto` ahora conserva explícitamente los selectores VKD3D
opt-in en su entorno generado. Esto corrige el wiring del launcher, pero no
demuestra que Unreal use la DLL experimental ni que llegue al camino DLSS.

## Corridas

1. Una ejecución con `WINEDEBUG=+loaddll,+seh` y el perfil VKD3D correcto
   llegó a crear el contexto D3D12 y el swapchain de 1280×720. El log de
   VKD3D confirmó varias veces:

   ```text
   Multiple adapters found with LUID 03f4
   Multiple adapters found with LUID 03f2
   ```

   El watchdog terminó la corrida a los 25 s, sin dejar procesos de la demo,
   Proton, `xalia` o el prefix seleccionado.

2. Se reemplazó temporalmente el `nvngx_dlss.dll` del plugin por el proxy del
   proyecto, conservando un backup exacto y restaurándolo mediante `trap` al
   terminar. La demo volvió a inicializar D3D12, pero no produjo archivo nuevo
   en `OUT_DIR` ni llamadas `remote_ngx_*` observables. El SHA-256 final del
   archivo restaurado coincidió con el backup.

3. Se repitió la sustitución temporal con `-ExecCmds` para habilitar
   `r.DLSS.Enable`, `r.NGX.DLSS.Enable` y `r.Streamline.DLSSG.Enable`. La
   ejecución mostró inicialización NVAPI relacionada con DLSSG, pero tampoco
   cargó `nvngx` ni produjo un log del bridge.

4. Se repitió el arranque con el runtime VKD3D actual y con
   `VKD3D_DUPLICATE_LUID_INDEX_PER_DEVICE=1`, primero vía `VKD3D_DLL_DIR` y
   luego copiando las DLL experimentales al prefix temporal para evitar que
   Proton priorizara sus DLL nativas. Ambas variantes terminaron por watchdog
   sin cargar `nvngx` ni emitir eventos del bridge.

## Resultado

- [x] El launcher directo crea el compat-data y arranca el shipping executable.
- [x] GE-Proton11-6 y VKD3D experimental llegan al render D3D12 real.
- [x] La limpieza selectiva elimina los hijos desacoplados de Proton.
- [x] La autodetección del perfil VKD3D funciona sin `VKD3D_DLL_DIR` manual.
- [ ] No se demostró carga de `nvngx_dlss.dll` por la demo.
- [ ] No se demostró `EvaluateFeature` auténtico en GPU B.
- [ ] No se resolvió la identidad LUID duplicada en el camino real: el
  selector por device está validado en el probe sintético, pero el proceso
  Unreal continúa informando LUID duplicado.
- [ ] MFG remoto y sincronización GPU-nativa siguen fuera de alcance.

## Instrumentación posterior — auditoría de carga del proxy

- Se recompiló el bridge con un `DllMain` global en `_nvngx.dll` y se instaló
  el artefacto en el perfil experimental `pair-worker`.
- Un smoke aislado bajo Wine cargó ese DLL como `nvngx_dlss.dll`, verificó los
  exports NGX esperados y produjo `loader_audit dll_process_attach`.
- Esta evidencia descarta un problema del entrypoint de la instrumentación,
  pero todavía no prueba que la demo Unreal cargue el proxy ni que ejecute
  `EvaluateFeature`; la corrida real queda pendiente.
- El smoke D3D12/VKD3D+NGX posterior no pudo repetir el transporte con el
  Proton que queda localmente: ese runner es incompleto y aborta antes de
  crear D3D12 por funciones `win32u`/`ole32` no implementadas. El próximo
  intento requiere un Proton completo; no se debe interpretar como fallo del
  loader audit ni del transporte CPU-gated.

La conclusión no es que el transporte haya fallado: la evidencia sólo muestra
que esta demo/corrida no llegó al punto de invocar DLSS/Streamline dentro del
watchdog. El siguiente experimento debe habilitar explícitamente DLSS en la
configuración de Unreal o usar un ejecutable que haga la llamada desde el
arranque, manteniendo la sustitución reversible y el prefijo temporal.

## Stellar Blade — runner reversible y auditoría de carga — 2026-09-11

- Ejecutable: `StellarBlade/SB/Binaries/Win64/SB-Win64-Shipping.exe`.
- DLL probado: `SB/Plugins/Runtime/Nvidia/DLSS/Binaries/ThirdParty/Win64/nvngx_dlss.dll`.
- El nuevo `scripts/run_real_game_remote_probe.sh` creó un prefijo temporal,
  reemplazó el DLL sólo durante la corrida y lo restauró mediante `trap`.
- Corridas directa y con `WINEDEBUG=+loaddll`, ambas con timeout, llegaron al
  arranque del juego pero no generaron `dlssnr-proxy.log` ni eventos
  `remote_ngx_*`. La traza de loader tampoco registró una carga de
  `nvngx_dlss.dll`.
- El resultado reproducible quedó en `/tmp/dlss5-stellarblade-remote` y
  `/tmp/dlss5-stellarblade-loader` mientras duró la investigación; son
  artefactos temporales y se eliminan tras documentar la evidencia.
- El hash restaurado fue
  `83de996b1589957d6bfb3df77e2f2ba7821f1014c73cfc5f8315241a0e4d3253`, igual
  al original. No se modificó permanentemente la instalación del juego.
- Conclusión: el runner y la reversibilidad quedan implementados, pero todavía
  no existe una prueba de DLSS real. El siguiente bloqueo es lograr el flujo de
  Steam/launcher o seleccionar otro título que cargue y active NGX bajo Proton;
  GPU-native sigue siendo una tarea separada y pendiente.

## Helldivers 2 — GameGuard y guardian de restauración — 2026-09-11

- Ejecutable: `Helldivers 2/bin/helldivers2.exe`.
- DLL probado: `Helldivers 2/bin/nvngx_dlss.dll`.
- La corrida endurecida llegó a cargar el ejecutable, varios módulos de
  GameGuard y `d3d12.dll`. No apareció ninguna carga observable de
  `nvngx_dlss.dll`, Streamline o `nvngx_dlssnr.dll`; tampoco apareció
  `dlssnr-proxy.log` ni `EvaluateFeature`.
- Se validó el mecanismo de emergencia con una segunda corrida: el estado era
  `injected` y el archivo tenía el hash del proxy
  `d1c2d8260eca26cae8e73bfaf0bedd94facfe5f89fbce0b16e2b9af4277df66b`; el
  shell fue terminado con `SIGKILL` y el guardian restauró el original
  `8707e53b26c68c606b98bf31c223485ff30d310a261b1a36d48b2eaabc1507ec`.
- Esta prueba cierra un riesgo de seguridad del runner, pero no el camino DLSS:
  GameGuard/Steam y el renderer no llegan a activar NGX en el lanzamiento
  directo. Sigue pendiente una integración real que entregue recursos a
  `EvaluateFeature` remoto en GPU B.
