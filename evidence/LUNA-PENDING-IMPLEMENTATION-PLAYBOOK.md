# Playbook de implementación pendiente para Luna

## 0. Objetivo y alcance

Este documento convierte los cinco bloques pendientes del port de Jak II a
macOS ARM64 en instrucciones de implementación ejecutables. No es una lista de
ideas ni una campaña de pruebas. Luna debe completar los bloques en orden, uno
por ticket y sin mezclar archivos de subsistemas distintos.

El alcance autorizado es exclusivamente:

1. Corregir `catch-frame`, pilas y restauración de contexto ARM64.
2. Robustecer y optimizar mandos externos.
3. Hacer transaccional fullscreen y fiable la elegibilidad para Apple Game
   Mode.
4. Consolidar e higienizar la integración ARM64 existente.
5. Preparar un bundle ARM64 redistribuible sin assets, firma ni notarización.

Quedan fuera de esta fase la campaña completa, mandos físicos, matriz de Macs,
sesiones largas, firma, notarización, stapling y Gatekeeper. Sólo se permiten
comprobaciones dirigidas necesarias para demostrar que una modificación no
rompe su contrato técnico.

No cambiar Metal/OpenGL, audio, gameplay, formatos de objetos o la corrección
SIMD `ZIP1/ZIP2`. No usar Rosetta.

## 1. Protocolo obligatorio para cada bloque

Antes de editar cada bloque:

```bash
cd /ruta/al/clon/OpenGOAL-Jak2-ARM64

git status --short
git branch --show-current
git rev-parse HEAD
uname -m
sysctl -n sysctl.proc_translated 2>/dev/null || true
```

Resultados obligatorios:

```text
git status --short: vacío
uname -m: arm64
sysctl.proc_translated: 0
```

El orquestador debe asignar un ticket ARM independiente antes de cada bloque.
La rama debe seguir el patrón:

```text
codex/arm-XXX-descripcion-corta
```

No trabajar en los cinco bloques dentro de un único diff. El orden es:

```text
catch-frame -> mandos -> fullscreen/Game Mode -> limpieza -> packaging
```

La entrega de cada ticket debe contener:

- Requisito exacto modificado.
- Archivos modificados.
- Invariante preservado.
- Comando dirigido ejecutado y código de salida.
- Riesgos que siguen abiertos.
- Commit terminado en `(AI-assisted)`.

No crear PR: el `AGENTS.md` interno lo prohíbe.

## 2. Bloque 1 — `catch-frame`, pilas y restauración ARM64

### 2.1 Fallo que se debe cerrar

`Jak2KernelTest.RunFunctionInProcess` entra en el proceso hijo y termina con
`SIGSEGV` durante el retorno normal o el `throw 'initialize`. La ruta afectada
es:

```text
run-function-in-process
  -> new catch-frame
    -> init-child-proc
      -> retorno normal, o process-deactivate
        -> throw 'initialize
          -> throw-dispatch
```

Hay dos sospechas separadas y Luna no debe mezclarlas:

1. El fixture asigna `*kernel-dram-stack*` a varios procesos, creando posible
   alias de pila.
2. El constructor ARM64 guarda el LR del llamador en `catch-frame.ra`, pero la
   ruta normal termina con `.ret` después de una llamada que ha sobrescrito
   `x30`. Debe demostrarse que el LR original se restaura tanto en retorno
   normal como en `throw-dispatch`.

### 2.2 Archivos permitidos

- `goal_src/jak2/kernel/gkernel.gc`.
- `goal_src/jak2/kernel/gkernel-h.gc` sólo si el contrato de `catch-frame`
  necesita documentación o un campo demostrado.
- `goalc/compiler/IR.cpp` y `IR.h`.
- `goalc/compiler/compilation/Asm.cpp`.
- `goalc/compiler/compilation/Function.cpp`.
- `game/kernel/asm_funcs_arm64.s` sólo si el fallo cruza el puente nativo.
- `test/goalc/source_templates/jak2/kernel-test.gc`.
- `test/goalc/test_goal_kernel2.cpp`.

No tocar renderer, audio, DGO, heaps globales o emitter SIMD.

### 2.3 Contrato que debe cumplir el contexto

| Estado | Valor obligatorio |
|---|---|
| Entrada a `new catch-frame` | `sp % 16 == 0`; `x30` contiene la continuación del llamador |
| `catch-frame.sp` | Offset GOAL de ese `sp`, no dirección nativa truncada |
| `catch-frame.ra` | Offset GOAL de la continuación original, no el LR de la llamada interna |
| Llamada a `func` | Puede sobrescribir `x30`, pero no `catch-frame.ra` |
| Retorno normal | Desenlaza el frame una vez, restaura `sp` y la continuación original |
| Throw | Desenlaza hasta `this->next`, restaura registros, `sp`, valor y continuación |
| Salida | `pp` vuelve al proceso anterior y no se ejecuta dos veces la limpieza normal |

En Apple ARM64 nunca usar `x18`. Toda modificación de `sp` debe mantener
alineación de 16 bytes.

### 2.4 Separar primero el fixture de producción

1. Dividir `RunFunctionInProcess` en dos recorridos dirigidos:
   - `init-child-proc 1`: retorno normal.
   - `init-child-proc 0`: `process-deactivate` y `throw 'initialize #f`.
2. Asignar a cada proceso una pila de prueba válida, alineada y no solapada.
   No inventar direcciones observadas en LLDB. Usar memoria de test ya mapeada
   por el runtime o reservar bloques explícitos dentro del fixture.
3. Añadir una comprobación en el fixture que compare los intervalos
   `[stack-top - stack-size, stack-top)` de todos los procesos y falle si se
   solapan.
4. Mantener un caso separado que demuestre el comportamiento cuando el proceso
   hijo se desactiva durante inicialización.
5. Si usar pilas únicas elimina el `SIGSEGV`, el arreglo pertenece al fixture.
   No cambiar producción sin otra evidencia.
6. Si el fallo continúa, seguir con la restauración de LR/SP.

### 2.5 Restaurar LR de forma explícita

La semántica x86 histórica de `.push/.pop rax` representa una dirección de
retorno apilada. En AArch64, `BL/BLR` escribe `x30` y no coloca nada en la pila.
El código actual convierte `.pop/.push` del rol histórico `rax` en movimientos
entre `x8` y `x30`. Esto no preserva por sí solo el LR a través de otra llamada.

Luna debe hacer lo siguiente:

1. En `new catch-frame`, capturar el `x30` de entrada antes de llamar a `func`.
2. Guardarlo como offset GOAL en `this.ra`:

   ```text
   ra_goal = caller_x30 - native_memory_base
   ```

3. Guardar el `sp` de entrada como offset GOAL en `this.sp`:

   ```text
   sp_goal = native_sp - native_memory_base
   ```

4. En el retorno normal, después de recuperar temporales y retirar
   `stack-frame-top`, reconstruir la continuación:

   ```text
   caller_x30 = this.ra + native_memory_base
   ```

5. Escribir esa dirección en LR antes de `.ret`. La rama ARM64 puede usar la
   abstracción existente que escribe `x30`, pero no debe consumir un slot de
   pila ficticio.
6. En x86 conservar exactamente la secuencia histórica. Usar una selección
   explícita `ARM64_TARGET`, no cambiar globalmente `.push/.pop` sin necesidad.
7. En `throw-dispatch`, cargar todos los campos de `this` antes de restaurar
   `sp`, porque `this` puede pertenecer a la región que deja de estar accesible
   como frame local después del cambio.
8. Restaurar en este orden conceptual:

   ```text
   next frame -> GPR/FPR -> saved_sp -> saved_ra -> return value -> branch/ret
   ```

9. No volver a la continuación interna posterior a `func` en la ruta de throw.
   El destino es el llamador de `new catch-frame`.

### 2.6 Auditar `.push/.pop` sin crear otra excepción frágil

Revisar `IR_AsmPush::do_codegen_arm64` e `IR_AsmPop::do_codegen_arm64`:

- El caso `X8` no puede modificar `sp`; representa lectura/escritura de LR.
- Los demás registros deben usar slots ARM64 alineados.
- Cada push real debe tener un pop real en todas las salidas.
- No añadir comparaciones por dirección de función, nombre de proceso o nivel.
- Si más de un asm-func necesita leer/escribir LR, considerar una operación IR
  explícita para LR en vez de ampliar casos especiales alrededor de `X8`.
- Cualquier operación nueva necesita prueba de bytes y prueba ejecutable ARM64.

### 2.7 Instrumentación temporal permitida

Durante el diagnóstico se pueden registrar, en una build Debug:

```text
pp
sp nativo
x30
this
this.sp
this.ra
pp.stack-frame-top
destino reconstruido antes del ret
```

No imprimir direcciones en Release ni dejar estos logs en el commit. No usar
`sleep` para alterar el orden de ejecución.

### 2.8 Comprobaciones dirigidas del bloque

```bash
./build/goalc-test --gtest_color=no \
  '--gtest_filter=Jak2KernelTest.Basic:Jak2KernelTest.RunFunctionInProcess:Jak2KernelTest.ThrowXmm:Jak2KernelTest.GprContextAcrossSuspend'

./build/goalc-test --gtest_color=no \
  '--gtest_filter=ARM64IRAsmBasic.*:ARM64RuntimeBridge.*:ARM64Trampoline.*'

cmake -B build-x86_64-release --preset=Release-macos-x86_64-clang
cmake --build build-x86_64-release --target gk --parallel 6
git diff --check
```

Esto no es QA final. Son comprobaciones del contrato de contexto.

### 2.9 Criterio de finalización

El bloque termina únicamente cuando retorno normal y throw recuperan el mismo
`sp`, LR y `pp` esperados, el frame se retira una sola vez, no existen pilas
solapadas y x86 sigue compilando sin cambio accidental de bytes.

## 3. Bloque 2 — Mandos externos robustos y sin hitch evitable

### 3.1 Defectos concretos presentes

En `InputManager::refresh_device_list()`:

- `i` es el índice de la lista completa de joysticks SDL.
- `m_available_controllers.size() - 1` es el índice real del vector filtrado.
- El mapping guarda `i`; si un joystick no es gamepad o no abre, ese valor ya
  no corresponde al vector.
- `SDL_GetJoysticks()` devuelve memoria que debe liberarse con `SDL_free()`.
- Cada evento borra y vuelve a abrir todos los mandos, causando trabajo
  síncrono evitable.

Además:

- `PadData::clear()` no pone `pressure_data` a cero.
- Rumble y trigger rumble no comprueban el límite del vector.
- `SDL_EVENT_GAMEPAD_REMAPPED` no se gestiona.
- Varios miembros de `GameController` no tienen inicialización explícita; una
  salida temprana puede hacer que el destructor lea capacidades indeterminadas.

### 3.2 Archivos permitidos

- `game/system/hid/input_manager.cpp` y `.h`.
- `game/system/hid/input_bindings.h` para neutralizar completamente `PadData`.
- `game/system/hid/devices/game_controller.cpp` y `.h`.
- Tests nuevos del subsistema HID si existe un target adecuado.

No cambiar mappings de gameplay por preferencia personal.

### 3.3 Corregir propiedad de la lista SDL

Encapsular la lista desde el momento de creación. Esquema recomendado:

```cpp
struct SDLFreeDeleter {
  void operator()(void* ptr) const noexcept { SDL_free(ptr); }
};

int count = 0;
std::unique_ptr<SDL_JoystickID, SDLFreeDeleter> ids{
    SDL_GetJoysticks(&count)};

if (!ids) {
  log SDL_GetError();
  return false;
}
```

La propiedad RAII debe cubrir retornos tempranos y excepciones. No añadir una
llamada manual a `SDL_free()` sólo al final dejando rutas de fuga.

### 3.4 Construir el mapping con el índice correcto

Para cada ID aceptado:

```cpp
auto controller = std::make_shared<GameController>(ids.get()[i], m_settings);
if (!controller->is_loaded()) {
  continue;
}

const int controller_index =
    static_cast<int>(m_available_controllers.size());
m_available_controllers.push_back(controller);
```

Usar `controller_index` en `m_controller_port_mapping`; nunca `i`.

El puerto lógico debe calcularse por separado:

1. Puerto persistido para el GUID, si es válido y está libre.
2. Si no, primer puerto lógico libre empezando en cero.
3. Crear `m_data[port]`, no `m_data[i]`.
4. Si dos GUID reclaman el mismo puerto, resolverlo de forma determinista y
   registrar el conflicto; no dejar un índice huérfano.

Mantener separados estos identificadores:

```text
SDL_JoystickID != índice del vector != puerto PS2
```

### 3.5 Centralizar la validación `port -> controller`

Añadir helpers privado y const:

```cpp
GameController* controller_for_port(int port) noexcept;
const GameController* controller_for_port(int port) const noexcept;
```

Contrato del helper:

```cpp
auto it = m_controller_port_mapping.find(port);
if (it == end) return nullptr;
if (it->second < 0) return nullptr;
if (static_cast<size_t>(it->second) >= m_available_controllers.size()) return nullptr;
if (!m_available_controllers[it->second]) return nullptr;
return m_available_controllers[it->second].get();
```

Hacer que consultores de capacidades, rumble, trigger rumble, LED y todos los
efectos DualSense usen exclusivamente este helper. Eliminar copias parciales de
la validación. `set_controller_for_port()` también debe rechazar índices
negativos.

### 3.6 Neutralizar desconexiones

Antes de cerrar un mando todavía presente:

1. Enviar rumble `0, 0` si el handle sigue válido.
2. Enviar trigger rumble `0, 0`.
3. Limpiar ambos efectos adaptativos DualSense.
4. Para cada puerto que apuntaba al dispositivo, ejecutar `PadData::clear()`.
5. Corregir `PadData::clear()` para poner:

   ```text
   button_data = false
   analog_data = 127
   pressure_data = 0
   analog_sim_tracker = 0
   ```

6. Cerrar `SDL_Gamepad` una sola vez.
7. Poner handles a `nullptr`, `m_loaded=false` y capacidades a `false`.
8. Reconstruir índices y mapping después de eliminar el elemento del vector.

Si teclado y mando comparten puerto, el estado neutro puede durar un frame; el
polling normal del teclado lo reconstruirá. No conservar un botón de mando
atascado para evitar ese frame neutro.

### 3.7 Inicialización segura de `GameController`

Inicializar en la declaración:

```cpp
SDL_Gamepad* m_device_handle = nullptr;
SDL_Joystick* m_low_device_handle = nullptr;
bool m_has_led = false;
bool m_has_rumble = false;
bool m_is_dualsense = false;
bool m_has_trigger_rumble = false;
bool m_has_pressure_sensitive_buttons = false;
```

Una salida temprana del constructor debe dejar un objeto destruible. Si abrir el
gamepad tuvo éxito pero falla una etapa posterior, cerrar el handle antes de
retornar o dejar que un destructor seguro lo haga sin leer booleanos no
inicializados.

### 3.8 Gestionar eventos sin reabrir todo varias veces

Incluir:

```text
SDL_EVENT_GAMEPAD_ADDED
SDL_EVENT_GAMEPAD_REMOVED
SDL_EVENT_GAMEPAD_REMAPPED
```

No ejecutar el refresco inmediatamente por cada evento. Implementar:

```cpp
bool m_controller_refresh_pending = false;
```

Los eventos de topología marcan el flag y dejan de propagarse a handles que van
a cerrarse. En un único punto del frame se consume el flag y se refresca una
vez.

Para evitar el hitch de cerrar y abrir todos los mandos:

1. Construir el conjunto actual de `SDL_JoystickID`.
2. Reutilizar los `GameController` cuyo instance ID sigue presente.
3. Crear objetos sólo para IDs nuevos.
4. Neutralizar y cerrar sólo IDs desaparecidos.
5. En `REMAPPED`, reconstruir binds/capacidades del ID afectado sin recrear
   dispositivos no relacionados.
6. Reconstruir `port -> vector index` al final, cuando el vector ya sea estable.

No mover SDL a un thread secundario en este bloque. El modelo del proyecto
procesa eventos SDL en el thread gráfico; introducir un thread exigiría
sincronización adicional de handles, vector, mapping y `PadData`.

### 3.9 Comprobaciones dirigidas del bloque

Añadir pruebas sin hardware mediante listas/abstracciones inyectables:

- Joystick no reconocido antes de un gamepad válido.
- Fallo al abrir el elemento intermedio.
- Índice negativo, puerto inexistente e índice igual a `size()`.
- Dos mandos con preferencias de puerto en conflicto.
- Desconexión con botones, presión, sticks y rumble activos.
- Diez eventos de topología en un frame producen un solo refresh.
- `REMAPPED` conserva el puerto y actualiza el mapping.

Después:

```bash
cmake --build build --target gk --parallel "$(sysctl -n hw.logicalcpu)"
git diff --check
```

No conectar todavía la matriz de mandos físicos; pertenece al QA final.

### 3.10 Criterio de finalización

Ningún puerto puede indexar fuera del vector. Toda lista SDL se libera. Una
desconexión deja estado neutro y efectos apagados. Los eventos de un frame se
coalescen y los mandos no afectados no se cierran ni reabren.

## 4. Bloque 3 — Fullscreen transaccional y Apple Game Mode

### 4.1 Defectos concretos presentes

`DisplayManager::set_display_mode()` inicializa `int result = 0` y nunca lo
cambia. Cualquier `break` por error termina guardando el modo solicitado como
si hubiera funcionado.

Además:

- `set_display_id()` guarda el monitor antes de saber si la transición tuvo
  éxito.
- Fullscreen no mueve/sincroniza siempre la ventana al monitor objetivo.
- La migración ARM64 actual convierte todo `Borderless` cargado a `Fullscreen`,
  incluso si después fue una elección consciente del usuario.
- `SDL_GetDisplays()` y `SDL_GetFullscreenDisplayModes()` devuelven arrays que
  también deben liberarse con `SDL_free()`.
- El Taskfile de Darwin sigue buscando `build/game/gk`, pero ARM64 produce
  `build/game/gk.app/Contents/MacOS/gk`.

### 4.2 Archivos permitidos

- `game/system/hid/display_manager.cpp` y `.h`.
- `game/settings/settings.cpp` y `.h`.
- `game/macos/Info.plist.in`.
- `Taskfile.yml` y `scripts/tasks/Taskfile_darwin.yml`.
- Pruebas dirigidas del gestor de pantalla.

### 4.3 Convertir el cambio en una transacción

Cambiar la firma privada a:

```cpp
bool set_display_mode(DisplayMode requested,
                      int window_width,
                      int window_height,
                      int target_display_index);
```

Separar tres fases:

1. `validate`: no muta ventana ni settings.
2. `apply`: llama SDL y sincroniza.
3. `commit`: actualiza y guarda settings sólo si todo fue correcto.

Esquema:

```cpp
const auto previous_settings = m_display_settings;
if (!validate(...)) return false;
if (!apply_window_transition(...)) {
  best_effort_restore(previous_settings);
  return false;
}
if (!window_matches_request(...)) {
  best_effort_restore(previous_settings);
  return false;
}
m_display_settings.display_mode = requested;
m_display_settings.display_id = target_display_index;
m_display_settings.save_settings();
return true;
```

El rollback no puede guardar el modo fallido. Si también falla, mantener las
preferencias anteriores en disco y registrar ambos errores.

### 4.4 Secuencia exacta por modo

#### Windowed

1. `SDL_SetWindowFullscreen(window, false)`.
2. `SDL_SyncWindow(window)`.
3. `SDL_SetWindowSize` con dimensiones mayores que cero.
4. Mover/centrar sólo después de salir de fullscreen.
5. `SDL_SyncWindow(window)`.
6. Verificar que `SDL_WINDOW_FULLSCREEN` no está presente.

#### Fullscreen nativo

1. Validar `target_display_index` contra el vector.
2. Obtener `SDL_DisplayID` y modo de escritorio.
3. Mover la ventana al bounds del monitor objetivo si no está allí.
4. Sincronizar el movimiento.
5. `SDL_SetWindowFullscreenMode(window, desktop_mode)`.
6. `SDL_SetWindowFullscreen(window, true)`.
7. `SDL_SyncWindow(window)`.
8. Verificar flag fullscreen y que `SDL_GetWindowFullscreenMode(window)` no es
   `nullptr`.

#### Borderless

1. Validar monitor y bounds.
2. Mover y sincronizar.
3. `SDL_SetWindowFullscreenMode(window, nullptr)`.
4. `SDL_SetWindowFullscreen(window, true)`.
5. Sincronizar.
6. Verificar flag fullscreen y modo fullscreen `nullptr`.

Cada llamada booleana falsa debe registrar `SDL_GetError()` y retornar fallo.

### 4.5 Monitor y eventos

`set_display_id()` no debe mutar `m_display_settings.display_id` antes de la
transacción. Debe pasar el monitor solicitado a `set_display_mode` y guardar
ambos valores juntos si funciona.

Cuando se desconecta un monitor:

1. Reconstruir la lista de displays.
2. Si el monitor guardado desapareció, elegir el primario.
3. Intentar la transición una vez.
4. Guardar el fallback sólo si la ventana terminó realmente allí.

Liberar con RAII tanto el array de `SDL_GetDisplays()` como el de
`SDL_GetFullscreenDisplayModes()`.

### 4.6 Migración de preferencias

Mantener `Fullscreen` como valor por defecto en Apple ARM64. Sustituir la
conversión incondicional de `Borderless` por una migración persistida una sola
vez, por ejemplo:

```text
arm64_native_fullscreen_migration_done = true
```

Reglas:

- Config antigua sin marca y `Borderless`: migrar una vez a `Fullscreen`.
- Config nueva: default `Fullscreen`.
- Usuario elige después `Borderless` o `Windowed`: respetarlo.
- x86-64 y otras plataformas conservan sus defaults actuales.

### 4.7 Bundle y Game Mode

Mantener en `Info.plist`:

```xml
<key>LSApplicationCategoryType</key>
<string>public.app-category.games</string>
<key>LSSupportsGameMode</key>
<true/>
<key>NSHighResolutionCapable</key>
<true/>
```

Actualizar Darwin para resolver el binario ARM64 dentro del bundle. Deben
existir dos comandos distintos:

```text
Diagnóstico: build/game/gk.app/Contents/MacOS/gk ...
Game Mode:   open build/game/gk.app --args ...
```

No afirmar que el juego activa Game Mode por su cuenta. El plist lo hace
elegible y macOS toma la decisión. El lanzamiento normal debe usar `open`.

### 4.8 Comprobaciones dirigidas del bloque

Con una capa SDL simulable comprobar:

- Fallo en cada llamada de la secuencia no guarda settings.
- Índice de monitor negativo o fuera de rango.
- Cambio monitor A -> B en windowed, fullscreen y borderless.
- Rollback conserva preferencias anteriores.
- Migración sucede una vez y luego respeta Borderless.
- Arrays SDL liberados también en error.

Validación estática del bundle:

```bash
plutil -lint build/game/gk.app/Contents/Info.plist
/usr/libexec/PlistBuddy -c 'Print :LSSupportsGameMode' \
  build/game/gk.app/Contents/Info.plist
file build/game/gk.app/Contents/MacOS/gk
lipo -archs build/game/gk.app/Contents/MacOS/gk
```

No iniciar aún QA visual de múltiples monitores.

### 4.9 Criterio de finalización

La preferencia persistida coincide con el estado real. Un fallo no guarda
Fullscreen falso. El monitor se confirma transaccionalmente. Una elección
posterior del usuario no es sobrescrita y el lanzamiento de bundle usa
LaunchServices.

## 5. Bloque 4 — Consolidación del código ARM64

### 5.1 Inventario antes de limpiar

```bash
git status --short
git log --oneline origin/master..HEAD
git diff --name-status origin/master...HEAD
git diff --stat origin/master...HEAD
```

Clasificar cada archivo en una sola propiedad primaria:

```text
emitter/SIMD
compiler/IR
ABI/contexto/JIT
runtime Jak II
HID/display
packaging
tests/documentación
```

No mover un archivo entre commits si eso separa una implementación de su prueba
dirigida.

### 5.2 Eliminar sólo instrumentación temporal

Buscar:

```bash
rg -n 'ARM DEBUG|DEBUG ARM|dump.*reg|printf.*0x|fprintf.*0x|GAMEPLAY:' \
  common game goalc goal_src test
```

Para cada coincidencia:

1. Comprobar con `git blame` si era upstream.
2. Conservar logs normales de error y profiler existente.
3. Retirar dumps de direcciones, sentinels y trazas añadidas sólo para LLDB.
4. No eliminar una comprobación de seguridad porque produzca logging.
5. No borrar tests de bytes ARM64.

### 5.3 Invariantes que no se pueden perder

- `pextub/uh/uw` usan `ZIP2`.
- `pextlb/lh/lw` usan `ZIP1`.
- `sp` alineado a 16 bytes.
- `x18` prohibido.
- `MAP_JIT`, W^X y flush de caché de instrucciones.
- x86-64 conserva su backend y sus bytes.
- Nada depende de una dirección, nivel o misión observada.
- No hay paths absolutos del desarrollador.

Buscar paths y parches sospechosos:

```bash
git diff origin/master...HEAD | rg '/Users/|/opt/homebrew|0x[0-9a-fA-F]{8,}'
```

Los opcodes y vectores esperados no son automáticamente defectos. Cada
coincidencia debe clasificarse; no hacer sustituciones masivas.

### 5.4 Commits reproducibles

El orden de commits recomendado es:

1. Contexto/catch-frame y sus pruebas.
2. HID/mandos y sus pruebas.
3. Fullscreen/Game Mode y sus pruebas.
4. Limpieza mecánica demostrablemente no funcional.
5. Packaging y documentación.

Cada commit:

```bash
git diff --check
git status --short
git diff --cached --stat
git commit -m "Descripción concreta (AI-assisted)"
```

No usar `git add -A` en un árbol mixto. No reescribir los 23 commits ARM64
históricos con rebase destructivo. Los nuevos arreglos se añaden encima.

### 5.5 Criterio de finalización

No quedan trazas temporales ni paths personales en los cambios ARM64; cada
arreglo tiene un commit identificable; la tabla ZIP sigue intacta y no se ha
mezclado packaging con runtime o HID.

## 6. Bloque 5 — Distribución ARM64 preparada, pero no firmada

### 6.1 Estado y regla principal

`scripts/package_macos_arm64.sh` es un prototipo, no una herramienta segura
para ejecutar todavía. Contiene un `rm -rf` sobre un argumento no validado,
rutas Homebrew fijas y firma obligatoria. Luna debe corregirlo antes de usarlo.

El output contiene código y dylibs. Nunca contiene ISO, extracción, saves,
configuración local o credenciales.

### 6.2 Validar el destino antes de borrar

Permitir outputs únicamente dentro de:

```text
<project_root>/dist/*.app
```

Procedimiento:

1. Crear/canonicalizar `dist`.
2. Canonicalizar el padre de `output_app` con `pwd -P`.
3. Exigir que el padre sea exactamente el `dist` canonicalizado.
4. Exigir basename no vacío terminado en `.app`.
5. Rechazar `/`, proyecto, `dist`, `.`, `..`, symlinks que escapen y paths con
   padre diferente.
6. Sólo después ejecutar `rm -rf -- "$output_app"`.

No aceptar un output arbitrario aunque sea proporcionado por argumento.

### 6.3 Resolver el build sin paths locales

- Alinear el default con `build`, o exigir `build_dir` explícito.
- Canonicalizarlo y verificar
  `game/gk.app/Contents/MacOS/gk` ejecutable.
- No buscar primero en `/Users/...` ni copiar paths del entorno del autor.
- Homebrew puede ser una raíz de búsqueda declarada, obtenida mediante
  `brew --prefix paquete`, nunca una constante silenciosa.
- Guardar las raíces de búsqueda en un array explícito y registrarlas sin
  imprimir tokens o identidades.

### 6.4 Resolver dylibs transitivas hasta punto fijo

Implementar una cola:

```text
queue = [main executable]
seen = {}
while queue not empty:
  binary = pop
  dependencies = otool -L binary
  for dependency non-system:
    resolve source
    validate architecture
    copy once
    rewrite install name
    enqueue copied dylib
```

Resolver correctamente:

- Path absoluto existente.
- `@loader_path` relativo al binario que referencia.
- `@executable_path` relativo al ejecutable principal.
- `@rpath` recorriendo los `LC_RPATH` efectivos.

Rechazar dependencias no resueltas. No elegir el primer basename encontrado
por `find` si hay dos candidatos.

Mantener un mapa:

```text
basename -> path canónico + hash
```

Si dos fuentes diferentes comparten basename, abortar en vez de sobrescribir.

### 6.5 Garantizar paquete ARM64 puro

Para cada Mach-O:

```bash
lipo -archs "$binary"
```

- `x86_64` sin ARM64: abortar.
- Universal: extraer el slice ARM64 al archivo empaquetado con `lipo -thin
  arm64`; no modificar la dependencia original.
- ARM64: copiar.

Al final, todos los Mach-O dentro de la app deben devolver únicamente `arm64`.

### 6.6 Rpaths e install names

Objetivo final:

```text
ejecutable -> @rpath/libX.dylib
dylib      -> @rpath/libY.dylib
LC_RPATH   -> @loader_path/../Frameworks para el ejecutable
```

Para dylibs dentro de `Contents/Frameworks`, calcular el loader path correcto
según su ubicación; no asumir que ejecutable y dylib comparten directorio.

Después de reescribir, auditar todos los binarios:

```bash
find "dist/OpenGOAL Jak II.app/Contents" -type f -print0 |
  while IFS= read -r -d '' file; do
    file "$file" | grep -q 'Mach-O' || continue
    otool -L "$file"
    otool -l "$file"
  done
```

No debe quedar ninguna dependencia no sistema con `/Users/`, `/opt/homebrew` o
el directorio de build.

### 6.7 Mantener JIT seguro

No cambiar la política para facilitar packaging:

- Memoria ejecutable mediante `MAP_JIT` donde corresponde.
- Alternancia escritura/ejecución; nunca RWX permanente.
- `pthread_jit_write_protect_np` o abstracción existente correctamente
  balanceada.
- Flush de caché de instrucciones tras escribir/relocalizar.
- Entitlement único `com.apple.security.cs.allow-jit`.

Buscar y rechazar regresiones:

```bash
rg -n 'PROT_WRITE.*PROT_EXEC|PROT_EXEC.*PROT_WRITE|MAP_JIT|allow-jit|jit_write_protect' \
  common game goalc
```

### 6.8 Preparar firma y notarización sin ejecutarlas

En esta fase el empaquetador debe terminar con un bundle sin firmar y mostrar
los pasos pendientes. No exigir `CODESIGN_IDENTITY` ni ejecutar `codesign`,
`notarytool` o `stapler` por defecto.

Separar un modo futuro explícito, por ejemplo `--sign`, que permanezca sin usar
hasta autorización. Preparar el orden:

```text
Frameworks/dylibs -> ejecutable con allow-jit -> bundle -> verify -> archive
-> notarytool -> stapler -> Gatekeeper
```

No almacenar identidad, team ID, Apple ID, contraseña específica ni perfil de
notarytool en Git.

### 6.9 Verificar que no hay assets

Antes de terminar el script:

```bash
if find "$output_app" -type f | rg -qi '\.(iso|dgo|cgo|vag|str|sbk|mus)$'; then
  echo "error: proprietary game data found in bundle" >&2
  exit 1
fi
if find "$output_app" -type d | rg -q '/(iso_data|out|decompiler_out|saves?)(/|$)'; then
  echo "error: local game-data directory found in bundle" >&2
  exit 1
fi
```

La aplicación puede requerir `--proj-path` hacia los datos legales locales,
pero esos datos no se copian al bundle.

### 6.10 Comprobaciones dirigidas del bloque

```bash
bash -n scripts/package_macos_arm64.sh
plutil -lint game/macos/Info.plist.in
git diff --check
```

Añadir casos que demuestren que el script rechaza:

- `/`, raíz del proyecto y `dist` como output.
- Una dependencia x86-only.
- Una dylib no resuelta.
- Dos dylibs distintas con el mismo basename.
- Un asset prohibido dentro del bundle.

No firmar ni notarizar durante estas comprobaciones.

### 6.11 Criterio de finalización

El script produce un bundle de código ARM64 puro, autosuficiente respecto a
dylibs no sistema, sin paths locales ni assets. El modo por defecto no firma.
W^X y MAP_JIT siguen intactos y el destino destructivo está estrictamente
confinado a `dist/*.app`.

## 7. Orden de entrega para Luna

| Orden | Entrega | No mezclar con |
|---:|---|---|
| 1 | Contexto `catch-frame` | HID, display, packaging |
| 2 | Mandos y neutralización | ABI/JIT, fullscreen |
| 3 | Fullscreen/Game Mode | Mandos, catch-frame |
| 4 | Limpieza ARM64 | Cambios funcionales nuevos |
| 5 | Packaging sin firma | Assets, notarización |

Después de cada bloque, Luna debe dejar un handoff en
`evidence/ARM-XXX-handoff.md`. Si el mismo impedimento bloquea tres turnos
consecutivos, debe entregar evidencia concreta y marcar el ticket `BLOCKED` a
través del orquestador; no debe saltar al siguiente bloque silenciosamente.

## 8. Resultado esperado al completar este playbook

Al terminar los cinco bloques:

- `RunFunctionInProcess` no corrompe LR/SP ni usa pilas solapadas.
- Los mandos no dejan inputs o efectos atascados y el refresh no reabre
  dispositivos no afectados.
- Fullscreen sólo se persiste cuando SDL y macOS lo aplican realmente.
- La app se lanza como bundle ARM64 elegible para Game Mode.
- El árbol ARM64 queda revisable, sin instrumentación temporal ni paths
  personales.
- Existe un bundle ARM64 sin assets, preparado para una fase posterior de
  firma/notarización.

Esto aún no autoriza afirmar “100 % jugable”. Esa declaración sigue reservada
para el QA final definido por el proyecto.
