# VKD3D experimental para adapters con LUID duplicado

El host Proton/DXVK de esta máquina expone dos RTX 3090 con el mismo `AdapterLuid`. El comportamiento normal de VKD3D usa ese LUID para el singleton y termina reutilizando el mismo device Vulkan.

El parche [vkd3d-duplicate-luid-adapters.patch](/home/cristian/Documentos/ChatGPT/3090-DLSS5/patches/vkd3d-duplicate-luid-adapters.patch) agrega un modo opt-in:

```bash
VKD3D_DUPLICATE_LUID_ADAPTERS=1
```

En ese modo, `d3d12core` fuerza devices independientes y selecciona los índices Vulkan en el orden de creación (0, 1). Se usa sólo para el laboratorio de esta máquina: no debe habilitarse globalmente ni en juegos sin validar el orden de creación.

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

Resultado observado: dos `VkPhysicalDevice`/`VkDevice` distintos y un `VkDeviceMemory` de heap D3D12 válido.

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

Resultado observado en este host: `vkGetMemoryFdKHR` devuelve un FD y `__wine_unix_spawnvp` lo entrega al helper nativo. La instrumentación dentro del dispatch de VKD3D confirma para el heap real `allocation=65536`, `type=1`, `export=0`, `properties=-13` (`VK_ERROR_UNKNOWN`). El helper recibe el FD, pero `cuImportExternalMemory` devuelve `CUDA_ERROR_UNKNOWN` tanto para CUDA ordinal 0 como 1. Esto deja validada la transferencia del descriptor, pero no la interoperabilidad de la asignación. El siguiente trabajo es conseguir una asignación dedicada cuyo contrato externo sea aceptado por Vulkan y CUDA, y luego sincronizarla con semáforos/fences.

## Límite NGX observado

Con `MGPU_NGX_SECOND_DEVICE_TEST=1`, NGX puede recibir `Init_Ext` en ambos devices. Sin embargo, sólo el device inicializado primero puede crear el feature; el segundo devuelve `0xbad00007` (`FAIL_NotInitialized`). `MGPU_NGX_SECOND_DEVICE_FIRST=1` permite invertir el experimento y produce el resultado simétrico.

Por eso el parche VKD3D resuelve la selección de hardware, pero no habilita todavía Neural Rendering remoto. Falta aislar o adaptar el estado global del runtime/proxy NGX y luego conectar memoria externa, sincronización y evaluación real.
