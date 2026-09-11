# Prueba de demo real bajo Proton — 2026-09-11

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
