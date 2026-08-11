# Jak II macOS ARM64 — estado, reproducción y trabajo pendiente para Luna

## 0. Propósito y autoridad

Este documento es el relevo maestro del port de Jak II a macOS ARM64. Debe permitir
que otro modelo continúe el trabajo sin repetir el diagnóstico ya cerrado ni
confundir una validación pendiente con un defecto demostrado.

La prioridad documental es:

1. La petición actual del propietario.
2. `AGENTS.md`.
3. Este documento y los handoffs de `evidence/`.
4. El código y las pruebas existentes.

El objetivo sigue siendo Jak II nativo en Apple Silicon. No se autoriza Rosetta,
una migración a Metal, cambios de gameplay, distribución de datos del DVD ni
relajar W^X, SIP, hardened runtime o Gatekeeper.

## 1. Límite legal y de publicación

El repositorio privado contiene exclusivamente código fuente, pruebas,
configuración y documentación. No debe contener material extraído del juego.

Nunca añadir a Git:

- La ISO original.
- `iso_data/`, salvo sus `.gitignore` vacíos ya versionados.
- `decompiler_out/`.
- `out/`.
- `dist/`.
- `build/`, `build-*` o bundles `.app` generados.
- DGO, CGO, VAG, STR, SBK, MUS, texturas, modelos o vídeos extraídos.
- Saves, dumps, capturas o logs que incluyan datos propietarios.
- Identidades de firma, certificados, contraseñas o tokens.

Antes de cualquier commit, Luna debe ejecutar:

```bash
git status --short --ignored iso_data decompiler_out out dist build build-arm64-release
git check-ignore -v iso_data/jak2/SCUS_972.65 decompiler_out out dist build-arm64-release
git ls-files iso_data decompiler_out out dist build build-arm64-release
```

El último comando sólo puede mostrar los `.gitignore` deliberadamente
versionados de `iso_data`. Si aparece cualquier dato del DVD, Luna se detiene y
no hace commit ni push.

## 2. Punto de partida del repositorio

- Rama de integración: `codex/arm-024-full-jak2`.
- Commit base registrado antes del último conjunto local: `d154e5f53`.
- Arquitectura objetivo: Apple ARM64/AArch64.
- Arquitectura de compatibilidad: x86-64 existente.
- Juego en alcance: Jak II.
- Backend gráfico: SDL3 + OpenGL existente.
- Configuración comprobada del DVD suministrado: Jak II NTSC, ELF
  `SCUS_972.65`, configuración `ntsc_v1`.

El árbol recibido contiene trabajo ARM64 posterior al commit base. Luna no debe
hacer `reset`, `checkout`, `stash`, `clean` ni descartar estos cambios. Primero
debe leer el diff y continuar sobre la rama recibida.

## 3. Resultado conseguido

El port ya supera la barrera principal: Jak II se ejecuta como proceso ARM64
nativo en macOS, muestra el título, entra en el menú, inicia campaña y llega a
gameplay. La pantalla negra que se detenía visualmente en Dolby ya no es el
problema activo.

Resultados registrados antes de este handoff:

| Área | Resultado |
|---|---|
| Arquitectura del ejecutable | Mach-O `arm64`, sin slice x86 |
| Rosetta | `sysctl.proc_translated = 0` en la ejecución auditada |
| Título/menú | Visible y operativo |
| Gameplay | Entrada registrada en `prison` y `ctysluma` |
| Paridad completa del emitter ARM64 | 27/27 |
| Regresiones `pext` IR + emitter | 28/28 |
| Suite amplia sin `Jak2KernelTest.*` | 871/871 |
| `Jak2KernelTest.Basic` | 1/1 |
| Build x86-64 de compatibilidad | Compila con código 0 |
| Game Mode | Bundle elegible mediante `LSSupportsGameMode=true` |

Esto demuestra gameplay inicial nativo, no campaña completa ni release final.

## 4. Trabajo técnico ya realizado

### 4.1 Selección explícita de arquitectura

El compilador y runtime dejaron de asumir que macOS ARM significa ejecutar un
binario x86 mediante Rosetta. ARM64 se selecciona como arquitectura real. La
compatibilidad x86 se conserva como ruta separada.

### 4.2 Registros y ABI de Apple ARM64

Se añadieron descripciones de registros ARM64 y su uso por el compilador,
incluidos registros de argumentos, retorno, preservados por el callee y
temporales. Los invariantes que no se pueden romper son:

- `sp` alineado a 16 bytes en cada llamada.
- Nunca usar `x18` en Apple ARM64.
- Preservar los registros no volátiles según AAPCS64/Darwin.
- No tratar el registro 31 como un GPR normal: según la codificación puede ser
  `SP` o `XZR`.

### 4.3 Emitter ARM64

Se implementaron y corrigieron operaciones escalares, enteras, de memoria,
vectoriales e int128 necesarias por Jak II. Entre las correcciones relevantes:

- Movimientos y aritmética donde participa `SP` usan codificaciones que
  conservan el significado de stack pointer.
- La instrucción nula ARM64 es un NOP válido, no `0x00000000`/UDF.
- LDR/STR SIMD indexados dejan limpio el campo `Rm` antes de insertar el
  registro real.
- Se corrigieron bases de opcode de aritmética vectorial.
- Se añadieron pruebas de bytes y pruebas ejecutables nativas.

### 4.4 Causa raíz de la pantalla negra

La pantalla Dolby no estaba bloqueada por audio, OpenGL, el DVD o Game Mode. La
cadena real era:

```text
matrix-transpose! -> pextlw/pextuw -> emitter ARM64
```

El backend ARM64 usaba `UZP1/UZP2`, que desentrelaza lanes. La semántica histórica
del proyecto y el backend x86 usan `PUNPCK`, que intercala lanes. La traducción
correcta es:

```text
pextub -> ZIP2 16B
pextuh -> ZIP2 8H
pextuw -> ZIP2 4S
pextlb -> ZIP1 16B
pextlh -> ZIP1 8H
pextlw -> ZIP1 4S
```

No revertir esta tabla. Un cambio que vuelva a UZP reintroduce matrices de cámara
corruptas, geometría diagonal y el bloqueo visual.

### 4.5 IR, funciones y llamadas

Se implementaron rutas ARM64 de IR fundamental, símbolos, estáticos, control,
llamadas, memoria, pila, matemática escalar, asm, vectores e int128. También se
trabajaron prólogos, epílogos y asignación de registros.

Luna debe distinguir siempre tres niveles:

1. Registro lógico histórico de GOAL/x86.
2. Registro físico ARM64 asignado.
3. Registro especial de ABI o runtime.

No asumir que un nombre histórico como `rax` significa que pueda manipularse
como un registro x86 o como una pila independiente.

### 4.6 Puente C++/GOAL y trampolines

Se añadieron puentes de entrada/salida y trampolines ARM64 para llamar código
GOAL JIT y MIPS2C desde C++, preservando contexto y pasando la base de memoria
GOAL. Los trampolines no pueden depender de direcciones absolutas observadas en
una ejecución.

### 4.7 Memoria JIT

La memoria ejecutable se centralizó alrededor de las protecciones de Apple:

- `MAP_JIT` cuando corresponde.
- Política W^X.
- Escritura y ejecución separadas correctamente.
- Flush de caché de instrucciones después de escribir o relocalizar código.
- Entitlement mínimo `com.apple.security.cs.allow-jit` para el bundle final.

No solucionar ningún fallo creando páginas RWX permanentes.

### 4.8 Jak II completo y runtime

Se conectaron kernel, DGO, fake ISO, IOP simulado, MIPS2C y rutas de carga de Jak
II para ARM64. Se corrigieron problemas de layout, símbolos, heap, pilas,
trampolines y llamadas que impedían alcanzar gameplay.

### 4.9 Bundle macOS y Game Mode

El target ARM64 genera `gk.app` con:

- `CFBundleIdentifier=org.openggoal.jak2`.
- Categoría de aplicación de juegos.
- `LSSupportsGameMode=true`.
- Capacidad Retina.
- macOS 14 como mínimo declarado.

En Apple ARM64 la preferencia inicial es fullscreen nativo, no borderless, para
que macOS pueda activar Game Mode.

## 5. Reconstrucción desde una ISO legal, sin subir la extracción

Esta sección describe cómo preparar una máquina nueva después de clonar el
repositorio privado. Todos los outputs permanecen locales e ignorados por Git.

### 5.1 Requisitos

Usar un Mac Apple Silicon. Abrir Terminal de forma nativa, no con la opción
“Abrir usando Rosetta”.

Clonar el repositorio privado y entrar en la rama de integración:

```bash
gh repo clone DiMiTriFrog/OpenGOAL-Jak2-ARM64
cd OpenGOAL-Jak2-ARM64
git switch codex/arm-024-full-jak2
```

La cuenta usada por `gh` debe tener acceso al repositorio privado. No copiar a
esta carpeta una ISO ni una extracción procedente de otra máquina.

```bash
uname -m
sysctl -n sysctl.proc_translated 2>/dev/null || true
```

Resultados obligatorios:

```text
arm64
0
```

Instalar las herramientas de Apple si faltan:

```bash
xcode-select --install
```

Instalar dependencias de build ARM64 desde Homebrew ARM64:

```bash
brew install cmake ninja go-task clang-format openssl@3 coreutils
```

Confirmar que Homebrew no es x86:

```bash
file "$(command -v brew)"
brew --prefix
```

En Apple Silicon el prefijo normal es `/opt/homebrew`. No instalar ni invocar
Rosetta para construir esta variante.

### 5.2 Configurar Jak II y su revisión

Desde la raíz del repositorio:

```bash
task set-game-jak2
task set-decomp-ntscv1
task settings
```

Para el DVD usado durante este port, `scripts/tasks/.env` debe terminar
equivalente a:

```text
GAME=jak2
DECOMP_CONFIG=jak2/jak2_config.jsonc
DECOMP_CONFIG_VERSION=ntsc_v1
TYPE_CONSISTENCY_TEST_FILTER=Jak2TypeConsistency
```

No seleccionar `ntsc_v2` basándose únicamente en el texto comercial del nombre
de la ISO. La extracción comprobada contiene `SCUS_972.65` y funcionó con
`ntsc_v1`. Si el ELF de otra copia es distinto, detenerse y seleccionar la
versión respaldada por `decompiler/config/jak2/jak2_config.jsonc`; no renombrar
un ELF para forzar la detección.

### 5.3 Montar la ISO en macOS y copiar su contenido

No copiar la ISO dentro del repositorio. Usar una ruta absoluta externa. El
siguiente procedimiento deja la ISO en sólo lectura y copia su contenido al
directorio ignorado `iso_data/jak2`:

```bash
ISO_PATH="/ruta/absoluta/a/Jak II.iso"

test -f "${ISO_PATH}"
mkdir -p iso_data/jak2

mounted_iso=""
cleanup_mounted_iso() {
  if [[ -n "${mounted_iso}" && -d "${mounted_iso}" ]]; then
    hdiutil detach "${mounted_iso}"
  fi
}
trap cleanup_mounted_iso EXIT

attach_output="$(hdiutil attach -readonly -nobrowse "${ISO_PATH}")"
mounted_iso="$(printf '%s\n' "${attach_output}" | awk -F '\t' 'END {print $NF}')"

test -d "${mounted_iso}"
ditto "${mounted_iso}" iso_data/jak2

hdiutil detach "${mounted_iso}"
mounted_iso=""
trap - EXIT
```

Validar la estructura mínima, sin imprimir ni subir su contenido:

```bash
test -f iso_data/jak2/SYSTEM.CNF
test -f iso_data/jak2/SCUS_972.65
test -d iso_data/jak2/DGO
test -d iso_data/jak2/VAG
test -d iso_data/jak2/STR
```

Comprobar que Git la ignora:

```bash
git status --short iso_data
git check-ignore -v iso_data/jak2/SCUS_972.65
```

`git status` no debe listar archivos extraídos. `git check-ignore` debe señalar
una regla de `iso_data/.gitignore` o `iso_data/jak2/.gitignore`.

### 5.4 Construir herramientas ARM64

Usar el preset ARM64 explícito:

```bash
export OPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"
cmake -B build --preset=Release-macos-arm64-clang
cmake --build build --parallel "$(sysctl -n hw.logicalcpu)"
```

No sustituir el preset por `Release-macos-x86_64-clang`. No anteponer
`arch -x86_64` a ningún comando.

Auditar herramientas antes de extraer:

```bash
file build/decompiler/decompiler
file build/goalc/goalc
file build/game/gk.app/Contents/MacOS/gk
lipo -archs build/game/gk.app/Contents/MacOS/gk
```

El ejecutable del juego debe mostrar únicamente `arm64`.

### 5.5 Extraer assets localmente

Con la configuración y el contenido del DVD listos:

```bash
task extract
```

Este comando consume `iso_data`, genera outputs en `decompiler_out` y `out`, y
no debe crear cambios Git. Después:

```bash
git status --short iso_data decompiler_out out
```

La salida debe estar vacía. Si no está vacía, corregir `.gitignore` antes de
cualquier commit.

### 5.6 Compilar el código GOAL de Jak II

Abrir el compilador:

```bash
task repl
```

En el prompt de GOAL ejecutar:

```clojure
(mi)
```

Esperar a que termine. No cerrar el compilador a mitad de la construcción. La
compilación usa los assets locales y genera objetos ignorados por Git.

### 5.7 Ejecutar el juego nativo

Ruta recomendada para lanzamiento directo y diagnóstico:

```bash
build/game/gk.app/Contents/MacOS/gk \
  -g jak2 \
  --proj-path . \
  -- \
  -boot \
  -fakeiso
```

Este arranque directo sirve para depurar. En macOS 26 Apple documenta que Game
Mode no se activa cuando el binario se crea directamente desde Terminal. Para
una sesión normal con Game Mode se debe lanzar el bundle mediante
LaunchServices:

Ruta recomendada para lanzar el bundle mediante LaunchServices y permitir que
macOS gestione el Space fullscreen/Game Mode:

```bash
open "build/game/gk.app" --args \
  -g jak2 \
  --proj-path "${PWD}" \
  -- \
  -boot \
  -fakeiso
```

`LSSupportsGameMode=true` hace que la app sea elegible; Game Mode lo decide y
activa macOS, no el juego mediante una API propia. Referencias oficiales:

- [LSSupportsGameMode — Apple Developer](https://developer.apple.com/documentation/bundleresources/information-property-list/lssupportsgamemode).
- [Notas de macOS 26 — Game Mode y lanzamiento con `open`](https://developer.apple.com/documentation/macos-release-notes/macos-26-release-notes).

No usar por ahora `task boot-game-retail`: el Taskfile histórico busca
`build/game/gk`, mientras que el target ARM64 empaquetado genera
`build/game/gk.app/Contents/MacOS/gk`. Luna debe corregir esa discrepancia como
parte de LUNA-3. Tampoco usar un ejecutable x86, `arch -x86_64`, Terminal bajo
Rosetta ni un bundle antiguo de `dist/`.

## 6. Trabajo de implementación/optimización pendiente

El propietario ha pedido realizar ahora únicamente correcciones y optimización.
La campaña completa, matriz de Macs, mandos físicos, sesiones largas,
notarización y QA exhaustiva se reservan para la fase final.

Las comprobaciones unitarias dirigidas siguen siendo obligatorias para no
introducir regresiones al editar código, pero no sustituyen ni adelantan el QA
final.

### LUNA-1 — Cerrar la restauración de contexto `catch-frame`

#### Problema

`Jak2KernelTest.RunFunctionInProcess` llega a activar el proceso hijo y después
termina con `SIGSEGV` al limpiar/restaurar un `catch-frame` ARM64. La prueba crea
dos procesos pequeños y existe evidencia de alias de `*kernel-dram-stack*`, pero
todavía no se ha demostrado si la causa es sólo el fixture o la ruta real de
`throw 'initialize`.

#### Archivos que Luna debe estudiar

- `goal_src/jak2/kernel/gkernel.gc`.
- `goal_src/jak2/kernel/gkernel-h.gc`.
- `goalc/compiler/IR.cpp` y `IR.h`.
- `goalc/compiler/compilation/Asm.cpp`.
- `goalc/compiler/compilation/Function.cpp`.
- `game/kernel/asm_funcs_arm64.s`.
- `test/goalc/source_templates/jak2/kernel-test.gc`.
- `test/goalc/test_goal_kernel2.cpp`.

#### Procedimiento de diagnóstico

1. No tocar renderer, audio ni el arreglo ZIP.
2. Reducir el flujo a dos casos:
   - retorno normal de `init-child-proc 1`;
   - desactivación/throw de `init-child-proc 0`.
3. En cada caso registrar conceptualmente:
   - proceso actual;
   - `sp` nativo;
   - `x30`/LR;
   - `process.stack-frame-top`;
   - `catch-frame.sp`;
   - `catch-frame.ra`;
   - dirección de continuación del llamador.
4. Confirmar que cada proceso usa una región de pila válida y no solapada.
5. Confirmar que el constructor de `catch-frame` guarda el contexto del
   llamador antes de transferir control.
6. Confirmar que `throw-dispatch` restaura exactamente ese contexto y no vuelve
   a ejecutar la limpieza normal del constructor.
7. Confirmar que cualquier escritura de `sp` usa una codificación ARM64 donde
   el registro 31 representa SP y no XZR.
8. Confirmar alineación de 16 bytes antes y después de cada `blr`.

#### Decisión de arreglo

- Si dos procesos comparten accidentalmente la misma pila sólo en el fixture,
  asignar pilas independientes en la prueba y mantener cobertura separada del
  retorno normal y del throw.
- Si producción restaura mal `sp` o `x30`, corregir la abstracción de contexto o
  el emisor; no añadir un caso especial por dirección, proceso o nivel.
- Si el error está en `.push/.pop`, corregir la semántica ARM64 general y
  conservar los bytes x86.

#### Prohibiciones

- Nada de `sleep`, reintentos temporizados o offsets observados.
- No saltar `throw`, `catch-frame` o `process-deactivate`.
- No aumentar arbitrariamente todos los heaps para esconder el error.
- No desactivar la prueba ni relajar una aserción.

#### Criterio técnico de finalización

La ruta normal y la ruta de throw deben conservar `sp`, LR, proceso actual y
memoria sin solapamiento. La solución debe ser general y compatible con x86.

### LUNA-2 — Robustecer y optimizar mandos externos

#### Problemas encontrados por auditoría estática

1. `InputManager::refresh_device_list()` mezcla el índice `i` de todos los
   joysticks con el índice del vector de mandos aceptados. Si SDL omite un
   dispositivo no reconocido, el puerto puede apuntar fuera del vector.
2. La lista devuelta por `SDL_GetJoysticks()` debe liberarse con `SDL_free()`.
3. Al desconectar un mando no se limpia explícitamente el último `PadData`; un
   botón o stick podría permanecer pulsado.
4. `SDL_EVENT_GAMEPAD_REMAPPED` no se procesa.
5. Algunas rutas de rumble y trigger rumble no validan el índice con la misma
   disciplina que LED y capacidades.
6. Los booleanos y punteros privados de `GameController` deben tener valores
   iniciales explícitos incluso cuando el constructor sale pronto.

#### Archivos en alcance

- `game/system/hid/input_manager.cpp` y `.h`.
- `game/system/hid/devices/game_controller.cpp` y `.h`.
- `game/system/hid/input_bindings.cpp` sólo si hace falta una corrección de
  mapping general; no cambiar mappings de gameplay por preferencia personal.

#### Implementación recomendada

1. Obtener la lista de joysticks con propiedad RAII o garantizar una llamada a
   `SDL_free()` en todas las salidas.
2. Antes de insertar un mando válido, calcular `controller_index` a partir de
   `m_available_controllers.size()`.
3. Guardar siempre `controller_index` en `m_controller_port_mapping`; nunca el
   índice bruto `i` si previamente se han filtrado dispositivos.
4. Separar claramente:
   - instance ID SDL;
   - índice dentro de `m_available_controllers`;
   - puerto lógico PS2.
5. Al refrescar por desconexión:
   - detener rumble/efectos del dispositivo que desaparece;
   - limpiar el `PadData` del puerto afectado;
   - cerrar el handle;
   - reconstruir el mapping;
   - restaurar la preferencia por GUID si sigue disponible.
6. Incluir `SDL_EVENT_GAMEPAD_REMAPPED` en el flujo de actualización, evitando
   procesar el mismo evento contra handles ya cerrados.
7. Crear un helper único que resuelva `port -> GameController*` y devuelva
   `nullptr` cuando el mapping no existe o el índice no es válido.
8. Hacer que rumble, trigger rumble, LED y efectos DualSense usen ese helper.
9. Inicializar punteros a `nullptr` y capacidades a `false` en la declaración.
10. No mover todavía la enumeración a un thread secundario salvo que se diseñe
    explícitamente la sincronización; un arreglo de latencia no puede introducir
    carreras sobre `m_available_controllers` o `PadData`.

#### Criterio técnico de finalización

El código no puede indexar fuera del vector aunque haya joysticks desconocidos,
fallos parciales de apertura, dos mandos, hot-plug o eventos pendientes. Cada
refresco libera la lista SDL y una desconexión deja todos los inputs y efectos
del puerto en estado neutro.

### LUNA-3 — Hacer fiable fullscreen y Apple Game Mode

#### Problema

`DisplayManager::set_display_mode()` declara `int result = 0`, pero las ramas de
error no actualizan `result`. Al final puede guardar el modo solicitado como si
hubiera funcionado aunque SDL no haya entrado en fullscreen. Eso puede hacer que
la configuración diga “Fullscreen” mientras macOS no ha creado el Space nativo,
impidiendo Game Mode.

#### Archivos en alcance

- `game/system/hid/display_manager.cpp` y `.h`.
- `game/settings/settings.cpp` y `.h`.
- `game/macos/Info.plist.in`.
- `Taskfile.yml` y `scripts/tasks/Taskfile_darwin.yml` para que los comandos de
  arranque resuelvan el ejecutable dentro de `gk.app` en ARM64 sin romper las
  rutas históricas de otras plataformas.

#### Implementación recomendada

1. Reemplazar el `result` inerte por un estado `success` que cambie a `false`
   ante cualquier fallo.
2. No modificar ni guardar `m_display_settings.display_mode` si falla:
   - obtener display;
   - obtener modo de escritorio;
   - fijar modo fullscreen;
   - entrar/salir de fullscreen;
   - mover o sincronizar ventana.
3. Conservar el modo anterior hasta confirmar éxito completo.
4. Mantener `DisplayMode::Fullscreen` como default ARM64 y `Borderless` para las
   plataformas históricas donde corresponda.
5. No convertir automáticamente una preferencia explícita `Windowed` del
   usuario.
6. La migración de un antiguo `Borderless` ARM64 sólo debe ejecutarse una vez o
   de forma idempotente, sin sobrescribir una elección posterior consciente.
7. Registrar errores concretos de SDL sin declararlos éxito.
8. Mantener en el plist:
   - categoría de juegos;
   - `LSSupportsGameMode=true`;
   - capacidad Retina;
   - bundle ID estable.
9. Actualizar las variables/tareas de arranque de Darwin para que
   `task boot-game` y `task boot-game-retail` usen
   `build/game/gk.app/Contents/MacOS/gk` cuando el preset sea ARM64. No cambiar
   la ruta x86-64 ni las rutas Windows/Linux.
10. Mantener dos rutas documentadas:
    - ejecución directa del Mach-O para depuración;
    - `open build/game/gk.app --args ...` para una sesión elegible para Game
      Mode.

#### Criterio técnico de finalización

El estado persistido debe coincidir con el estado real de la ventana. Un fallo
de fullscreen no puede dejar configuración falsa ni bloquear un intento
posterior. La ruta ARM64 debe solicitar fullscreen nativo de macOS.

### LUNA-4 — Consolidar e higienizar la integración ARM64

#### Objetivo

Convertir el árbol de integración actual en cambios revisables sin perder
ninguna corrección funcional.

#### Procedimiento

1. Inventariar todos los archivos modificados antes de editar.
2. Clasificar cada diff en:
   - backend/emitter;
   - compiler/IR;
   - ABI/runtime/JIT;
   - integración Jak II;
   - Game Mode/packaging;
   - pruebas/documentación.
3. Buscar instrumentación temporal (`ARM DEBUG`, dumps de registros, prints de
   direcciones, flags de diagnóstico).
4. Eliminar sólo instrumentación inequívocamente temporal. No borrar logging
   normal de errores o telemetría existente.
5. No revertir arreglos sólo porque abarcan código compartido con Jak 1/3; antes
   decidir si son necesarios para compilar código común.
6. No introducir refactors cosméticos mientras se corrige un subsistema.
7. Preparar commits coherentes con `(AI-assisted)` en el mensaje, de acuerdo con
   `AGENTS.md` del repositorio.

#### Criterio técnico de finalización

Cada commit debe tener una finalidad única y el diff completo debe estar libre
de whitespace inválido, binarios, datos del DVD y paths personales.

### LUNA-5 — Preparar un bundle ARM64 redistribuible sin assets

#### Estado

Existe `scripts/package_macos_arm64.sh`, que copia el bundle ARM64, resuelve
dylibs no sistema, normaliza rpaths y firma con hardened runtime más el
entitlement JIT mínimo. No debe copiar ningún dato del juego.

#### Trabajo pendiente de implementación

1. Validar que `output_app` sea una ruta `.app` concreta bajo un directorio de
   salida permitido antes del `rm -rf`; nunca aceptar `/`, el workspace o un
   directorio ambiguo.
2. Evitar depender silenciosamente de rutas Homebrew fijas. Resolver primero
   desde el build y usar prefijos declarados sólo como inputs de empaquetado.
   Alinear además su build por defecto con la guía (`build`) o exigir siempre
   el directorio como argumento; actualmente el script presupone
   `build-arm64-release`.
3. Recorrer dependencias transitivas hasta punto fijo y rechazar:
   - binarios x86-only;
   - rutas absolutas no sistema;
   - dependencias que no puedan resolverse;
   - colisiones de dos dylibs diferentes con el mismo basename.
4. Conservar `@rpath`/`@loader_path` consistentes para ejecutable y dylibs.
5. Firmar dylibs antes del ejecutable y el bundle.
6. Mantener exclusivamente `com.apple.security.cs.allow-jit`, salvo que una
   necesidad adicional sea demostrada y aprobada.
7. No incluir `iso_data`, `out`, `decompiler_out`, saves ni configuración local
   dentro de la app.
8. Dejar notarización, stapling y prueba Gatekeeper para la fase final y sólo
   con autorización/credenciales del propietario.

#### Criterio técnico de finalización

El script debe poder producir un bundle autosuficiente de código ARM64 sin
incluir material propietario y sin riesgo de borrar una ruta amplia proporcionada
por error.

## 7. Comprobaciones dirigidas mínimas durante implementación

Estas comprobaciones no son la campaña ni el QA final. Son el mínimo exigido
después de tocar código para saber que el cambio compila y no rompe su contrato
local.

```bash
./build-arm64-release/goalc-test --gtest_color=no \
  '--gtest_filter=ARM64EmitterParity.*:ARM64IRInt128.Math3AllOpcodesAndAliases'

./build-arm64-release/goalc-test --gtest_color=no \
  '--gtest_filter=Jak2KernelTest.Basic:Jak2KernelTest.RunFunctionInProcess'

cmake --build build-arm64-release --parallel "$(sysctl -n hw.logicalcpu)"
git diff --check
git status --short
```

Si una corrección toca código compartido, conservar una build x86 de regresión,
pero no ejecutar el juego x86 en el Mac ARM64:

```bash
cmake --build build-x86_64-release --target gk --parallel 6
```

## 8. QA deliberadamente aplazado a la fase final

No ejecutar como parte de la fase de optimización salvo nueva orden del
propietario:

- Campaña completa.
- Segunda pasada de opcionales.
- Saves por límites de misión.
- Matriz M1/M2/M3/generaciones posteriores.
- Dos versiones principales de macOS.
- DualSense, DualShock y Xbox físicos por USB/Bluetooth.
- Hot-plug, sleep/wake y sesiones largas.
- Métricas p95/p99, memoria y audio de larga duración.
- Firma de distribución, notarización, stapling y Gatekeeper.

Esta separación no permite afirmar “100 %” antes del QA; sólo impide gastar la
fase actual en pruebas finales mientras aún quedan correcciones de código.

## 9. Condiciones de parada para Luna

Luna debe detenerse y documentar evidencia si:

- El arreglo exige cambiar el formato persistente de objetos.
- Aparece una duda real de ABI ARM64.
- Cambian bytes x86 inesperadamente.
- Se necesitaría una dirección absoluta o un offset observado.
- Se necesitaría relajar W^X, SIP, hardened runtime o Gatekeeper.
- Un archivo contiene cambios ajenos imposibles de separar.
- Git intenta añadir datos del DVD o outputs de extracción.
- La solución requiere sustituir SDL/OpenGL por Metal.

## 10. Definición de relevo correcto

El siguiente modelo debe poder:

1. Clonar el repositorio privado.
2. Construir herramientas ARM64 sin Rosetta.
3. Montar localmente una ISO legal y copiarla a una ruta ignorada.
4. Ejecutar `task extract` sin crear cambios Git.
5. Compilar Jak II mediante `(mi)`.
6. Lanzar `gk.app` nativo con `-boot -fakeiso`.
7. Continuar LUNA-1 a LUNA-5 sin reabrir la causa Dolby ya resuelta.
8. Mantener toda extracción y todo material propietario fuera de GitHub.
