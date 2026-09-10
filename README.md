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
- Salida humana y JSON.

## MVP automático

El objetivo del MVP no es soportar todos los juegos. Es ejecutar un título D3D12 bajo Proton con una configuración segura y reproducible:

```text
./scripts/mgpu-auto doctor
./scripts/mgpu-auto run --game cyberpunk2077 --dry-run
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
- El bridge Windows compilado carga bajo Wine y expone los exports NGX esperados.
- El demo D3D12 aislado funciona con VKD3D para renderizar. Con Wine del sistema el smoke enumera un adaptador sintético `NVIDIA GeForce GTX 470` y no alcanza feature level 12.0; con GE-Proton 11-6/VKD3D-Proton el mismo host enumera las dos RTX 3090 y crea ambos dispositivos D3D12 correctamente.
- En el prefix GE-Proton aislado, el proxy NGX inicializa el core (`0x1`), inicializa DLSS estándar (`0x1`), crea el feature DLSS y carga/crea el feature Neural Rendering con el runtime comunitario `nvngx_dlssnr.dll` 310.8.0. El runtime reporta referencias a `sm86`, por lo que la ruta Ampere llega a creación real del feature. La evaluación sintética todavía devuelve `0xbad00005` (`FAIL_InvalidParameter`); falta completar un host con recursos/estados y contrato de parámetros idénticos a un juego real.
- La sonda aislada `tests/ngx_nr_direct_smoke.cpp` intentó además usar NR como feature independiente: el DLL directo devuelve `0xbad00002` en `Init_Ext` y el proxy devuelve `0xbad0000c` para `Reserved18`. Esto confirma que el runtime comunitario sólo está accesible en el chaining interno observado hasta `CreateFeature`.
- Con `MGPU_NGX_SECOND_DEVICE_TEST=1`, el smoke inicializa NGX y crea features en dos `ID3D12Device` simultáneos, y libera ambos correctamente. Esto valida la reentrancia básica de NGX, no que cada objeto esté respaldado por una RTX 3090 distinta ni que exista transporte cross-adapter.
- La sonda `tests/vkd3d_interop_probe.cpp` añadió una comprobación más estricta: en GE-Proton/VKD3D-Proton, ambos `ID3D12Device` del mismo proceso devuelven el mismo `VkPhysicalDevice` y `VkDevice`. `VKD3D_VULKAN_DEVICE=0/1` cambia el device Vulkan elegido para todo el proceso, pero no permite mezclar ambos adapters D3D12 en una sola instancia.
- La misma sonda confirma que GE-Proton expone `ID3D12DXVKInteropDevice3`: se obtiene el `VkBuffer` de un recurso D3D12 real y el `VkDeviceMemory` de un heap mediante `GetVulkanResourceInfo`/`GetVulkanHeapInfo`.
- Se compiló una variante experimental aislada de VKD3D-Proton con `VKD3D_DUPLICATE_LUID_ADAPTERS=1`. El parche permite crear dos devices Vulkan distintos cuando DXGI entrega el mismo LUID; el probe pasa `multi_adapter_distinct=yes`. Los DLL quedan fuera del Proton instalado.
- Se añadió `VKD3D_EXPORT_OPAQUE_FD_MEMORY=1` y se comprobó que el heap entrega un FD con `vkGetMemoryFdKHR`. El FD se hereda correctamente a un helper Linux nativo, pero `vkGetMemoryFdPropertiesKHR` devuelve `VK_ERROR_UNKNOWN` y CUDA responde `CUDA_ERROR_UNKNOWN` en GPU0 y GPU1. Exportar el FD no equivale todavía a memoria Vulkan/CUDA interoperable.
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

En este host imprime `multi_adapter_distinct=no`: `VKD3D_VULKAN_DEVICE=0/1` selecciona una GPU para todo el proceso, no una por cada objeto D3D12. Con `VKD3D_INTEROP_REQUIRE_DISTINCT=1` la limitación se convierte en un gate que termina con código 7.

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

El resultado queda en `build/proton/`. Para pasar a `READY_REMOTE` todavía deben existir, dentro del prefix/juego, las DLLs NGX compatibles proporcionadas por el usuario: `_nvngx_real.dll`, `nvngx_dlss_real.dll` y `nvngx_dlssnr.dll`.

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
