# ARM64 — entrega final de implementación del playbook

Fecha: 2026-08-11  
Rama: `codex/arm-024-full-jak2`  
Objetivo: cerrar los cinco bloques de implementación/optimización definidos en
[`LUNA-PENDING-IMPLEMENTATION-PLAYBOOK.md`](LUNA-PENDING-IMPLEMENTATION-PLAYBOOK.md).

Este informe no distribuye la ISO ni ningún asset extraído. El bundle generado
es unsigned y no contiene datos del juego.

## Resultado

Los cinco bloques del playbook están implementados y separados en commits
coherentes:

| Bloque | Commit | Resultado |
|---|---|---|
| Catch-frame, pilas y contexto ARM64 | `4f3b297cc` + `8a6e66320` | Retorno normal, throw y contexto GPR/XMM cubiertos; fixture con pilas privadas y detección de solapamiento. |
| Ciclo de vida SDL de mandos | `f0b5c55fe` | Índices de vector correctos, liberación SDL, desconexión/remapeo y efectos validados defensivamente. |
| Fullscreen/Game Mode | `4db9ddd62` | Transiciones transaccionales, errores SDL propagados y fullscreen nativo ARM64 predeterminado. |
| Consolidación ARM64 | `4f3b297cc` y commits ARM64 anteriores | Sin instrumentación temporal en el kernel; se conserva ZIP1/ZIP2 y la selección de registros Apple ARM64. |
| Distribución nativa | `c4e1c7097` | Bundle relocatable unsigned, sólo ARM64, dylibs en `Frameworks`, sin ISO/assets. |

## 1. Contexto ARM64 y la regresión de Luna

### Producción

`thread-resume` y `set-to-run-bootstrap` conservan la semántica histórica de
los registros GOAL sin tratar AArch64 como x86:

- `x18` no se usa.
- `sp` permanece alineado a 16 bytes.
- `saved-gpr5` y `saved-gpr6` se mapean explícitamente a registros callee-saved
  ARM64.
- La continuación ARM64 se conserva antes de saltar al código reanudado.
- La continuación normal termina con `.jr` a la dirección reconstruida; no
  intenta ejecutar un `.ret` con un `x30` ya sobrescrito.
- La ruta x86 no se modifica.
- Se retiró `print-asm` de las rutas de producción después del diagnóstico.

### Fixture

`test/goalc/source_templates/jak2/kernel-test.gc` ahora reserva cada pila con:

```lisp
(malloc 'global PROCESS_STACK_SIZE)
```

y usa el extremo superior como intervalo `[stack-top -
PROCESS_STACK_SIZE, stack-top)`. `test-stack-tops-overlap` compara todos los
intervalos y lanza un fallo si dos procesos se solapan. Esto se aplica a los
procesos de `kernel-test-2`, `state-test` y `gpr-context-test`.

La prueba GPR crea dos procesos con dos pilas distintas: uno mantiene siete
valores a través de 32 suspensiones y el otro sobrescribe los siete registros
persistentes antes de suspender. La prueba C++ espera las dos respuestas del
listener; la prueba negativa verifica explícitamente la rama de detección.

## 2. Mandos SDL

La implementación en `game/system/hid/` quedó preparada para hot-plug sin
bloqueos perceptibles:

- `SDL_GetJoysticks()` se libera mediante `SDL_free()` RAII.
- El índice guardado en `m_controller_port_mapping` es el índice del vector
  filtrado de mandos, nunca el índice de la enumeración SDL sin filtrar.
- Los identificadores SDL y los puertos lógicos se mantienen separados.
- Se procesan `SDL_EVENT_GAMEPAD_ADDED`, `SDL_EVENT_GAMEPAD_REMOVED` y
  `SDL_EVENT_GAMEPAD_REMAPPED`, agrupando el refresco al final del polling.
- Se reutilizan handles existentes y sólo se cierran los mandos desaparecidos.
- Al desconectar o reasignar se limpian botones, presión, sticks, estado
  simulado y vibración.
- Los índices negativos o fuera de rango no llegan a rumble, LED ni efectos
  DualSense.
- Al cerrar un mando se detienen rumble, trigger rumble y efectos DualSense
  antes de cerrar el handle.

## 3. Fullscreen nativo y Game Mode

`DisplayManager::set_display_mode` devuelve `bool` y sólo persiste el modo
después de que SDL confirme el estado real de la ventana. Si macOS rechaza
fullscreen, no se guarda una preferencia falsa de fullscreen.

Las transiciones entre ventana, fullscreen y monitor:

- validan índices negativos y límites;
- mueven la ventana al monitor destino antes de pedir el Space nativo;
- sincronizan la ventana antes de cambiar de monitor en fullscreen;
- propagan y registran errores SDL;
- limpian y liberan las listas de monitores/modos;
- mantienen fullscreen nativo como predeterminado en Apple ARM64;
- migran una única vez una preferencia antigua de Borderless, respetando una
  elección explícita posterior de Borderless.

El `Info.plist` del bundle mantiene `LSSupportsGameMode=true`, junto con el
identificador de aplicación de juego y el mínimo de macOS definido por el
proyecto.

## 4. Bundle ARM64 relocatable

[`scripts/package_macos_arm64.sh`](../scripts/package_macos_arm64.sh):

1. exige host `arm64` y ejecutable fuente de una sola arquitectura `arm64`;
2. copia sólo `gk.app`;
3. no busca ni copia ISO, `iso_data`, DGO, CGO, texturas, audio ni vídeos;
4. resuelve dylibs desde el árbol de build y prefijos Homebrew de la máquina;
5. copia targets reales de symlinks con `cp -L`;
6. normaliza RPATH a `@loader_path/../Frameworks`;
7. relocaliza dependencias transitivas y elimina rutas absolutas de desarrollo;
8. verifica que cada binario empaquetado sea exclusivamente ARM64;
9. deja deliberadamente sin realizar firma, notarización y stapling.

Comando reproducible ejecutado:

```bash
bash -n scripts/package_macos_arm64.sh
package_dir="$(mktemp -d /tmp/opengoal-arm64-final.XXXXXX)"
./scripts/package_macos_arm64.sh build-arm64-debug \
  "${package_dir}/OpenGOAL Jak II.app"
```

Resultado: código de salida `0`; el ejecutable generado fue identificado como
`Mach-O 64-bit executable arm64`, `lipo -archs` devolvió `arm64`, y sus
dependencias no sistémicas quedaron bajo `@rpath`.

## 5. Verificaciones ejecutadas

Entorno nativo:

```text
uname -m                         -> arm64
sysctl -n sysctl.proc_translated -> 0
build-arm64-debug/game/gk       -> Mach-O 64-bit executable arm64
build-x86_64-release/game/gk    -> Mach-O 64-bit executable x86_64
```

Suite dirigida ARM64:

```bash
./build-arm64-debug/goalc-test --gtest_color=no \
  --gtest_filter='Jak2KernelTest.*:ARM64IRAsmBasic.*:ARM64RuntimeBridge.*:ARM64Trampoline.*'
```

Resultado: `32 tests`, `32 passed`, código de salida `0`.

La regresión de compilación x86 se ejecutó con:

```bash
cmake --build build-x86_64-release --target gk goalc-test --parallel 8
```

Resultado: código de salida `0`; el ejecutable x86 sigue siendo `x86_64`.
Los warnings del build son warnings preexistentes del proyecto/toolchain, no
errores de compilación.

Finalmente:

```bash
git diff --check
git status --short
```

Resultado: sin errores de whitespace y árbol limpio antes de la publicación.

## 6. Trabajo que queda para la QA final, no para la implementación

No queda ningún bloque de implementación del playbook abierto. Sí queda la
validación de producto que el propietario decidió ejecutar al final:

- extraer localmente los assets desde la ISO legal del usuario siguiendo las
  instrucciones del handoff, sin subir la ISO ni la extracción;
- iniciar el bundle ARM64 con esos assets y confirmar título, menú, carga y
  gameplay;
- comprobar el Space nativo de macOS y que Game Mode aparece activo;
- recorrer la campaña completa con guardado/carga;
- probar mando externo, hot-plug, reasignación, rumble, LED y DualSense;
- validar cambios de monitor y modos de ventana;
- ejecutar la matriz de hardware macOS acordada;
- firmar, notarizar, staplear y probar Gatekeeper.

Hasta completar esa lista no se debe afirmar “100 % jugable” según el criterio
de `AGENTS.md`. Tampoco se debe cambiar el script para incluir assets ni para
firmar automáticamente.
