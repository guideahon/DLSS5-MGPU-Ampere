# Integración Proton/NGX

## Estado

El transporte nativo Linux ya fue probado con una memoria `DEVICE_LOCAL` creada por Vulkan, exportada mediante opaque fd, importada desde CUDA y copiada a la otra RTX 3090 por P2P.

La siguiente capa debe vivir dentro del proceso Proton/VKD3D o utilizar una interfaz de memoria externa equivalente. Un proceso Linux separado no debe recibir frames por socket o memoria CPU en el camino final, porque eliminaría la ventaja del P2P. GE-Proton 11-6/VKD3D-Proton ya fue validado como host D3D12: enumera las dos RTX 3090 y crea un dispositivo para cada una.

El smoke multi-device también inicializa NGX en ambos `ID3D12Device` dentro del mismo proceso y crea un feature en ambos objetos con éxito. La sonda posterior demostró que GE-Proton los respalda con el mismo `VkPhysicalDevice`/`VkDevice`; por tanto esto valida la reentrancia de NGX, no dos adapters Vulkan distintos ni evaluación NR remota.

La sonda D3D12 cross-adapter confirmó el límite del backend actual: VKD3D crea el heap/recurso con los flags correctos, pero no implementa `CreateSharedHandle` para el heap (`E_NOTIMPL`) y rechaza la exportación directa del recurso. La ruta recomendada queda entonces en memoria externa Vulkan/CUDA y P2P, manteniendo la coordinación dentro del proceso Proton.

La interfaz privada de VKD3D sí permite recuperar el `VkBuffer` de un recurso y el `VkDeviceMemory` de un heap (`ID3D12DXVKInteropDevice3::GetVulkanHeapInfo`). El build GE-Proton instalado expone esa interfaz. El parche experimental de este workspace resuelve además el caso local de dos adapters con LUID duplicado y permite obtener un FD Vulkan; la importación de ese FD en CUDA todavía falla con `CUDA_ERROR_UNKNOWN`, por lo que siguen pendientes la asignación realmente interoperable y la sincronización entre devices.

La revisión del fuente de VKD3D-Proton ubica el trabajo futuro en `libs/vkd3d/device.c` (`d3d12_device_CreateSharedHandle`), `libs/vkd3d/heap.c` (`VK_EXT_external_memory_host`) y `libs/vkd3d/resource.c` (creación de buffer cross-adapter). Para integrar el bridge habría que añadir una interfaz privada de extracción de memoria/sincronización; no es un cambio que pueda resolverse desde una DLL NGX aislada.

## Base de NGX

El proyecto de referencia es `dlss5-linux-bridge`. Su proxy se compila como DLL para Proton y carga los runtimes proporcionados localmente por el usuario. Este repositorio no debe copiar ni redistribuir:

- `_nvngx.dll`;
- `nvngx_dlss.dll`;
- `nvngx_dlssnr.dll`;
- modelos, pesos o CUBINs.

## Adaptación propuesta

```text
core_proxy.cpp
  ├── render-device adapter
  ├── ngx backend local/remoto
  ├── frame capture
  └── transport backend
       ├── Vulkan external memory
       ├── CUDA P2P
       └── host fallback
```

El hook debe interceptar la evaluación del frame terminado, no intentar dividir el render del juego. La cadena local ya fue validada hasta `Init_Ext`/`CreateFeature` con DLSS estándar y el runtime NR comunitario 310.8.0 en SM86. Para la primera versión:

1. mantener DLSS Super Resolution del juego;
2. ejecutar Neural Rendering una sola vez;
3. copiar solamente el color final;
4. usar optical flow calculado en GPU B si el runtime lo requiere;
5. presentar el resultado en una ventana independiente de GPU B;
6. no activar Frame Generation.

La normalización del contrato de parámetros del bridge corrigió el smoke sintético: `EvaluateFeature` ahora devuelve `0x00000001` y el chaining DLSSNR también completa `0x00000001`. Eso no demuestra todavía un resultado visual de juego: falta un host que entregue formatos, estados, descriptores y contenido temporal auténticos.

También se ejecutó `tests/ngx_nr_direct_smoke.cpp` para separar ambas capas. El DLL NR directo no pudo inicializarse (`0xbad00002`), y pedir `Reserved18` al proxy devolvió `0xbad0000c`; la ruta utilizable sigue siendo el chaining interno del proxy, validado hasta `CreateFeature`, no una API pública para NR independiente.

## Compatibilidad de índices

Nunca usar `Vulkan device index == CUDA device index` sin verificarlo. En la máquina de desarrollo el driver expone duplicados Vulkan. El mapeo correcto se hace por:

```text
VkPhysicalDeviceIDProperties::deviceUUID
VkPhysicalDevicePCIBusInfoPropertiesEXT
cudaDeviceProp::uuid
cudaDeviceProp::pciBusID
```

## Gate de aceptación

La integración NGX se considera lista para pruebas de juego únicamente si:

- el frame source proviene de una swapchain D3D12/Vulkan real;
- el recurso llega a GPU B sin host staging;
- el `frame_id` se conserva de extremo a extremo;
- el resultado se valida en GPU B;
- no se ejecuta Frame Generation;
- una sesión de 30 minutos no produce device loss.

## MVP automático

El MVP automático tendrá un alcance deliberadamente pequeño: un juego D3D12, una versión de Proton y un runtime DLSS suministrado por el usuario.

### Flujo

```text
mgpu-auto doctor
        ↓
inventario CUDA/Vulkan/PCI/monitores
        ↓
self-test P2P + memoria externa
        ↓
selección render GPU / neural GPU
        ↓
configuración por juego
        ↓
política de lanzamiento segura
        ↓
remote NR preparado o fallback local
```

### Selección automática de GPUs

La selección debe usar esta prioridad:

1. GPUs NVIDIA con el mismo vendor y SM compatible.
2. Par con `cudaDeviceCanAccessPeer == true` en ambos sentidos.
3. Par cuyo test Vulkan→CUDA→P2P complete con validación correcta.
4. GPU neural conectada a un monitor activo.
5. GPU con menor carga compute y memoria libre suficiente.
6. GPU restante como render.

La implementación cruza UUIDs: en esta máquina Vulkan 0 corresponde a CUDA 1 y Vulkan 1 a CUDA 0. Asumir índices iguales rompe la importación de memoria externa.

Si hay procesos como VLLM ocupando la segunda GPU, el controlador no debe detenerlos. Debe informar que el modo remoto fue rechazado y utilizar el modo local.

En el VKD3D experimental de este host, la deduplicación por UUID/PCI ya produce identidades físicas distintas en un mismo proceso: A `0:1:0.0` y B `0:3:0.0`. La SPI no se activa en Proton stock y el gate remoto continúa cerrado hasta que exista sincronización externa funcional.

### Estados de salud

```text
READY_LOCAL_ONLY
P2P_UNAVAILABLE
VULKAN_CUDA_MISMATCH
NGX_RUNTIME_MISSING
NGX_INIT_FAILED
DEVICE_LOST
FALLBACK_ACTIVE
```

Cada transición se debe escribir en JSONL junto con el UUID, PCI bus, driver, Proton y juego.

### Configuración por juego

La configuración debe vivir fuera del repositorio, por ejemplo:

```text
~/.config/dlss5-mgpu/config.toml
~/.config/dlss5-mgpu/games/<game-id>.toml
~/.local/state/dlss5-mgpu/<game-id>.jsonl
```

Ejemplo conceptual:

```toml
[game]
name = "cyberpunk2077"
prefix = "/path/to/compatdata/1091500/pfx"
exe = "Cyberpunk2077.exe"

[gpu]
render = "auto"
neural = "auto"
output = "auto"

[safety]
fallback_local = true
watchdog_seconds = 10
anti_cheat = "deny"

[neural]
enabled = true
frame_generation = false
```

### Fallback

El fallback debe ocurrir en tres casos:

- self-test fallido;
- runtime NGX ausente o incompatible;
- pérdida de dispositivo o error de importación durante el arranque.

El fallback no debe dejar DLLs ni variables de entorno activas para el siguiente lanzamiento.

### Orden de implementación del controlador

1. `mgpu-auto doctor`: inventario y JSON.
2. `mgpu-auto selftest`: ejecuta los probes sin iniciar juegos.
3. `mgpu-auto plan`: muestra la pareja GPU elegida sin modificar nada.
4. `mgpu-auto run --dry-run`: crea configuración y política sin lanzar Proton.
5. `mgpu-auto run --exe ... --runner ... --prefix ... --enable-remote`: prueba
   un ejecutable fuera de Steam con el mismo watchdog y política pair-worker.
6. integración del bridge NGX dentro de Proton y watchdog de logs.
7. prueba positiva reproducible mediante `scripts/run_ngx_test.sh` con `PROTON` y `DLSS_NR_DLL` explícitos.
8. opcionalmente repetir la prueba multi-device con `MGPU_NGX_SECOND_DEVICE_TEST=1`.
9. lanzamiento real con fallback local y persistencia de la configuración aprobada.

El modo automático no debe activar todavía `dlssg_for_sm86` ni Frame Generation remoto. Esas funciones se agregan después del MVP NR remoto.
