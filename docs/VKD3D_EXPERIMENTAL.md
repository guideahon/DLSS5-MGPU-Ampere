# VKD3D experimental para adapters con LUID duplicado

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

Resultado observado: dos handles `VkPhysicalDevice`/`VkDevice` distintos en la prueba de dos objetos, pero este host reporta la misma identidad UUID/PCI para las entradas duplicadas bajo VKD3D. No se debe interpretar todavía como dos GPUs físicas distintas.

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
