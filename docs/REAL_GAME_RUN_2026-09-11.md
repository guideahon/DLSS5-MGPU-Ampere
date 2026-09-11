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

## Resultado

- [x] El launcher directo crea el compat-data y arranca el shipping executable.
- [x] GE-Proton11-6 y VKD3D experimental llegan al render D3D12 real.
- [x] La limpieza selectiva elimina los hijos desacoplados de Proton.
- [x] La autodetección del perfil VKD3D funciona sin `VKD3D_DLL_DIR` manual.
- [ ] No se demostró carga de `nvngx_dlss.dll` por la demo.
- [ ] No se demostró `EvaluateFeature` auténtico en GPU B.
- [ ] No se resolvió la identidad LUID duplicada en el camino real.
- [ ] MFG remoto y sincronización GPU-nativa siguen fuera de alcance.

La conclusión no es que el transporte haya fallado: la evidencia sólo muestra
que esta demo/corrida no llegó al punto de invocar DLSS/Streamline dentro del
watchdog. El siguiente experimento debe habilitar explícitamente DLSS en la
configuración de Unreal o usar un ejecutable que haga la llamada desde el
arranque, manteniendo la sustitución reversible y el prefijo temporal.
