# DXVK-NVAPI experimental en el harness dual 3090

Este camino opt-in permite que el smoke D3D12 use DXVK para exponer una
fábrica DXGI y DXVK-NVAPI para resolver NVAPI sobre Vulkan. No reemplaza
bibliotecas del sistema ni modifica Proton instalado.

## Construcción

Con un checkout de DXVK-NVAPI y MinGW-w64 x64:

```bash
scripts/build_dxvk_nvapi_x64.sh \
  /ruta/al/dxvk-nvapi \
  /tmp/dlss5-dxvk-nvapi-build
```

El builder produce `x64/nvapi64.dll` y `x64/nvofapi64.dll`. El empaquetador
upstream completo también intenta construir x86, que no es necesario para
este fixture Win64.

## Ejecución aislada

```bash
MGPU_DXVK_DIR=/ruta/a/GE-Proton/files/lib/wine/dxvk/x86_64-windows \
MGPU_DXVK_NVAPI_DIR=/tmp/dlss5-dxvk-nvapi-build/x64 \
MGPU_NGX_CROSS_ADAPTER=1 \
MGPU_NGX_PRIME_SOURCE=0 \
MGPU_CROSS_ADAPTER_GPU_NATIVE=1 \
scripts/run_d3d12_cross_adapter_frame_smoke_wine.sh
```

Cuando se provee `MGPU_DXVK_NVAPI_DIR`, el runner activa localmente
`DXVK_ENABLE_NVAPI=1` y, si no se indicó otra configuración, fija
`DXVK_CONFIG='dxgi.customVendorId = 10de'`. Esto evita que DXVK presente la
GPU integrada AMD como vendor lógico durante la inicialización de NVAPI.

## Resultado validado

Con el runtime NGX comunitario SM86 y el driver NVIDIA 595.71.05:

- DXVK-NVAPI real: `NvAPI_Initialize=0x1`.
- Transferencia persistente color/motion/depth A→B: 3/3 frames.
- B-first: `Init_Ext=0x1`, `CreateFeature=0x1`, `EvaluateFeature=0x1`.
- El proxy registró DLSS estándar y DLSSNR en B, ambos con resultado `0x1`.
- Readback de la salida en B: `nonzero=6448576`,
  `fnv1a=0x3d0cf3accd85fd8a`.

Con el orden A-first, `Init_Ext` también llega a `0x1`, pero el feature en el
segundo device devuelve `0xbad00007`. Cambiar el orden invierte qué device
funciona; esto sigue siendo el estado global de NGX, no un fallo de
DXVK-NVAPI ni del transporte P2P.

## Alcance pendiente

El resultado es una evaluación local real en B sobre recursos sintéticos ya
transferidos por el harness. Todavía no prueba un frame capturado desde un
juego, composición/presentación remota ni un pass NR desacoplado del chaining
DLSS→DLSSNR. La sincronización GPU-native y el MVP remoto completo permanecen
pendientes explícitamente hasta validar ese escenario real.
