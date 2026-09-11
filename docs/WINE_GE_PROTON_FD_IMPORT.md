# Importación FD en Wine/GE-Proton

## Evidencia reproducible

El probe nativo `mgpu-vulkan-cross-device-fd-probe` separa Vulkan del camino
Wine/VKD3D. En las dos RTX 3090 actuales obtuvo:

```text
nvidia[0] uuid=af:6d:e4:b3
nvidia[1] uuid=5b:9f:38:5f
source_export_fd result=VK_SUCCESS
destination_import_allocate result=VK_ERROR_OUT_OF_DEVICE_MEMORY
```

La consulta auxiliar `vkGetMemoryFdPropertiesKHR` devuelve `VK_ERROR_UNKNOWN`,
La primera corrida que parecía exitosa seleccionaba dos handles que
representaban la misma GPU. Después de deduplicar por UUID, la barrera nativa
queda confirmada en la importación cross-device; no debe atribuirse todavía al
thunk Wine.

## Cambio aplicado

`patches/wine-win32u-import-memory-fd.patch` añade el caso
`VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR` a `win32u_vkAllocateMemory`.
El nodo no debe eliminarse del `pNext`: el host Vulkan necesita recibirlo.

## Resultado del intento con GE-Proton

Se compiló un `win32u.so` desde el submódulo Wine de GE-Proton con el caso FD y
diagnóstico temporal. No se dejó instalado: un módulo compilado desde un árbol
sin el pipeline completo de GE-Proton es ABI incompatible con el resto de
Proton. Se observaron incompatibilidades de las interfaces internas de usuario
y Vulkan, se detuvo la prueba y se restauró el módulo original.

El siguiente intento, una vez resuelta o sorteada la importación nativa, debe
ejecutar el pipeline completo de GE-Proton —staging,
Wayland/Wineland y generación de headers— y aplicar el parche FD como último
paso antes de compilar. No hay que mezclar un `win32u.so` parcial con los
binarios distribuidos por GE-Proton.

## Criterio de éxito

El check se marca completo sólo cuando el probe bajo Proton registre:

```text
vkd3d_resource_fd_imported_same_process=yes
```

y luego el worker pueda abrir el recurso en un proceso separado. Hasta entonces,
NR remoto y MFG remoto permanecen cerrados.
