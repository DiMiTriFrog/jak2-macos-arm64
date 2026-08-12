# Playbook de implementación: AOT y Metal para Jak II en plataformas Apple

Estado: **propuesta de ejecución**. Este documento no declara implementado AOT, Metal ni iOS.

Ámbito: Jak II, macOS ARM64 e iOS ARM64. La compatibilidad x86-64 existente debe mantenerse.

Baseline de la propuesta:

- Upstream congelado por el proyecto: `641fb572e876750d8618434d9f7c1a531b43b366`.
- Snapshot auditado para este documento: `184fecf9137f7542dcad57aeac6fdc33121364b4`.
- El runtime ARM64 actual es dinámico y utiliza `MAP_JIT`, páginas W^X y trampolines emitidos en memoria.
- El renderer actual utiliza OpenGL de escritorio y 92 shaders GLSL `410 core`.
- Jak II contiene aproximadamente 849 fuentes GOAL y más de 10 000 declaraciones de funciones,
  métodos, behaviors y estados. La conversión AOT debe ser automática y determinista.

## 1. Resultado final exigido

El trabajo solo se considera terminado cuando existen estos cuatro productos:

1. `Jak II macOS ARM64 AOT + Metal`, sin Rosetta y sin JIT.
2. `Jak II iOS ARM64 AOT + Metal`, ejecutándose en dispositivo físico y sin JIT.
3. El modo dinámico de desarrollo de macOS continúa funcionando para REPL y recarga de GOAL.
4. Las builds x86-64 existentes continúan compilando y superando su regresión.

La build AOT final debe cumplir simultáneamente:

- Todo el código GOAL, C++, ensamblador y MIPS2C está enlazado en el Mach-O firmado.
- Ningún byte importado desde un paquete de datos se ejecuta.
- No existe `MAP_JIT`, `PROT_EXEC` aplicado a memoria anónima, `pthread_jit_write_protect_np`,
  `allow-jit`, generación de trampolines ni flush de código generado.
- La memoria EE es únicamente RW, excepto la zona de guardia `PROT_NONE`.
- Los shaders se compilan durante el build a `metallib`; la aplicación no compila GLSL ni MSL.
- El backend gráfico de Apple es Metal. OpenGL permanece como backend de compatibilidad para las
  plataformas existentes hasta una decisión posterior.
- Los assets proceden de una copia legal aportada por el usuario y nunca se añaden al repositorio,
  a GitHub, a tests públicos ni al bundle de la aplicación.

Llegar al menú o completar una misión no equivale a “100 %”. La aceptación final exige campaña,
guardado, carga, mandos, audio, suspensión, memoria, rendimiento, firma y matriz de dispositivos.

## 2. Arquitectura objetivo

### 2.1 Pipeline de host

Las herramientas se ejecutan en un Mac y nunca se cross-compilan para iOS:

```text
Fuentes GOAL de Jak II ──> goalc AOT ──> ensamblador ARM64 por objeto
                                      ├─> manifiesto de funciones
                                      ├─> plantillas de datos
                                      └─> relocalizaciones de datos

ISO original ──> extractor existente ──> assets extraídos ──> data-packager
                                                        └─> paquete de datos sin código

ensamblador + registro C++ + shaders MSL ──> Xcode/AppleClang ──> Mach-O firmado
```

La generación AOT debe producir un directorio reproducible y no versionado:

```text
out/aot/jak2/<target>/
├── manifest.json
├── registry.generated.h
├── registry.generated.cpp
├── objects/
│   ├── <object-id>.S
│   ├── <object-id>.data.bin
│   └── <object-id>.reloc.json
└── build-report.json
```

No se permiten rutas absolutas, nombres de usuario, timestamps no deterministas ni UUID aleatorios
en esos archivos.

### 2.2 Pipeline de runtime

```text
Paquete de datos validado
        ↓
Loader de datos AOT
        ├── copia plantillas a memoria EE RW
        ├── aplica solo relocalizaciones de datos
        ├── crea descriptores GOAL de función
        ├── actualiza símbolos y métodos
        └── invoca inicializadores precompilados
                         ↓
               tabla nativa AOT en Mach-O
```

El loader AOT no debe aceptar segmentos de código. Si el paquete declara uno, la carga falla antes
de modificar el heap.

### 2.3 Representación de funciones

Los punteros de datos GOAL continúan siendo offsets `u32` relativos a `g_ee_main_mem`. Una función
GOAL deja de ser una dirección ejecutable dentro de esa memoria y pasa a ser un descriptor de datos.

Contrato inicial que debe validarse en `AOT-002` antes de fijarlo mediante ADR:

```cpp
using AotFunctionId = u32;

struct GoalAotFunctionPayload {
  u32 magic;          // constante, por ejemplo 'AOTF'
  AotFunctionId id;   // 0 siempre es inválido
  u32 generation;     // identidad de la instancia cargada
  u32 flags;          // ABI, C bridge, MIPS2C, etc.
};
```

El puntero GOAL apunta al payload y conserva el type tag normal colocado por el allocator. No se
debe fijar el tamaño ni los offsets definitivos hasta caracterizar la representación actual.

La tabla nativa reside fuera de la memoria EE:

```cpp
using GoalAotEntry = u64 (*)(GoalCallContext*);

struct AotNativeEntry {
  GoalAotEntry entry;
  u32 abi_version;
  u32 flags;
  const char* debug_name;
};

std::span<const AotNativeEntry> get_jak2_aot_registry();
```

Reglas:

- Las llamadas indirectas validan descriptor, ID, generación y flags antes de `blr`.
- Las llamadas directas pueden usar un símbolo Mach-O cuando sea seguro, pero deben conservar la
  misma semántica que una llamada mediante descriptor.
- Símbolos, method tables, callbacks, states y behaviors almacenan offsets a descriptores.
- Recargar un objeto crea una nueva instancia de descriptor; no se reutiliza silenciosamente una
  identidad antigua si el runtime original tampoco lo hace.
- Un descriptor inválido produce un error determinista con objeto, función e ID. Nunca se cae por
  saltar a memoria de datos.

### 2.4 Objetos AOT

Cada objeto GOAL se divide en dos productos:

1. Código ARM64, compilado y enlazado en `__TEXT,__text`.
2. Plantilla de datos, copiada a la memoria EE cuando se carga el objeto.

El manifiesto por objeto debe contener como mínimo:

```json
{
  "schema": 1,
  "game": "jak2",
  "object": "example",
  "source_sha256": "...",
  "functions": [
    {
      "id": 1,
      "segment": 0,
      "ordinal": 0,
      "symbol": "example-function",
      "native_symbol": "_og_j2_o0001_s0_f0000_abcd1234"
    }
  ],
  "data_templates": [],
  "data_relocations": [],
  "top_level": []
}
```

Los IDs se generan desde una clave canónica que incluya juego, objeto, segmento y ordinal. El
generador ordena primero todas las claves, asigna IDs de forma estable y falla ante duplicados o
colisiones. No se usa `std::hash`, direcciones, orden de un `unordered_map` ni rutas del host.

Las relocalizaciones AOT permitidas en datos son explícitas:

- `DataOffset32`: offset a otra plantilla instanciada.
- `SymbolValue32`: valor actual de un símbolo GOAL.
- `TypePointer32`: offset de un type object.
- `FunctionDescriptor32`: offset del descriptor correspondiente a un `AotFunctionId`.
- `StringOffset32`: offset a string GOAL instanciado.

No existe un tipo `CodePointer`, `NativePointer` ni una relocalización que escriba una dirección de
64 bits del proceso dentro de datos importables.

### 2.5 Metal

El backend Metal se añade junto al backend OpenGL:

```text
GfxRendererModule
├── OpenGLRendererModule
└── MetalRendererModule
```

Los tipos públicos del renderer no pueden contener `GLuint`, `GLenum` ni headers de GL. Se usa un
handle opaco, con generación, para detectar recursos destruidos:

```cpp
struct GpuTextureHandle {
  u32 index;
  u32 generation;
};
```

El backend Metal debe:

- Crear `SDL_MetalView` y obtener su `CAMetalLayer`.
- Seleccionar `MTLDevice`, command queue y formatos de color/depth explícitos.
- Mantener varios frames en vuelo sin bloquear la CPU cada frame.
- Crear buffers, texturas, samplers, render targets y pipelines mediante RAII.
- Convertir las 92 fuentes GLSL a MSL y compilarlas offline.
- Mantener una caché determinista de pipelines basada en una clave de estado completa.
- Reemplazar operaciones OpenGL no portables, no imitarlas mediante lecturas GPU síncronas.
- Producir capturas comparables con el backend OpenGL.

Mapeos obligatorios:

| OpenGL actual | Metal objetivo |
|---|---|
| `glTexImage1D` | Textura 2D de altura 1 para compartir comportamiento macOS/iOS |
| `glGetTexImage` | Copia blit a staging buffer o copia sombra mantenida en CPU |
| `glPolygonMode` | Pipeline de líneas o índices de wireframe generados explícitamente |
| `glMultiDrawElements` | Bucle de draws correcto primero; indirect command buffers solo al optimizar |
| FBO | `MTLTexture` + `MTLRenderPassDescriptor` |
| VAO/VBO | Layout explícito de vértices + `MTLBuffer` |
| uniforms | Ring buffer por frame con alineación validada por plataforma |
| GLSL program | función de `metallib` + `MTLRenderPipelineState` |

### 2.6 Matriz de builds

| Target | CPU | Renderer | Código GOAL | Uso |
|---|---|---|---|---|
| macOS dev | ARM64 | OpenGL | dinámico/JIT | REPL y referencia |
| macOS release | ARM64 | Metal | AOT | producto principal |
| macOS regresión | x86-64 | OpenGL | dinámico | no regresión |
| iOS simulator | ARM64 | Metal | AOT | integración rápida |
| iOS device | ARM64 | Metal | AOT | aceptación real |

Rosetta solo puede utilizarse para ejecutar la regresión x86-64. Nunca se acepta para el producto
macOS ARM64, las herramientas ARM64 ni ninguna evidencia de rendimiento.

## 3. Protocolo obligatorio de ejecución

### 3.1 Una tarea por ticket

El agente implementador trabaja en un único ticket. No empieza el siguiente hasta que:

1. Ha creado su handoff.
2. Un revisor diferente ha repetido las pruebas.
3. El orquestador ha integrado el commit.
4. La dependencia figura como `DONE`.

Un ticket no puede mezclar AOT, Metal, iOS y packaging. Si un fallo pertenece a otro subsistema se
registra y se devuelve al ticket propietario.

### 3.2 Inicio de cada ticket

Ejecutar desde la raíz del repositorio:

```bash
git status --short
git branch --show-current
git rev-parse HEAD
uname -m
sw_vers
cmake --version
clang --version
```

Después:

1. Leer completos `AGENTS.md`, el ticket, el perfil y las ADR relacionadas.
2. Confirmar que el árbol está limpio en todos los archivos que se van a modificar.
3. Guardar el baseline del ticket en `logs/<TICKET>/baseline.log`.
4. Ejecutar primero el test que deberá seguir pasando.
5. Añadir un test que falle por el motivo correcto.
6. Implementar el cambio mínimo.
7. Ejecutar test dirigido, regresión compartida y `git diff --check`.
8. Crear `evidence/<TICKET>-handoff.md`.

No se puede usar reset destructivo, limpiar assets, firmar, publicar, hacer push o abrir PR como
parte implícita de ningún ticket.

### 3.3 Evidencia mínima

Cada handoff debe relacionar requisito, cambio y prueba. Los logs deben omitir rutas absolutas y
datos personales. Para sanitizarlos:

```bash
rg -n '/Users/|/home/|BEGIN .*PRIVATE KEY|gh[pousr]_|Bearer |Authorization:' \
  evidence logs docs
```

El resultado esperado es vacío. Los assets y outputs generados se mantienen ignorados por Git.

### 3.4 Baseline común

Antes del primer ticket y en cada puerta:

```bash
cmake --build build-arm64-debug --parallel
build-arm64-debug/goalc-test \
  --gtest_filter='Jak2KernelTest.*:ARM64IRAsmBasic.*:ARM64RuntimeBridge.*:ARM64Trampoline.*'

cmake --build build-x86_64-release --parallel
arch -x86_64 build-x86_64-release/goalc-test \
  --gtest_filter='Jak2KernelTest.*'

git diff --check
```

Si la máquina no dispone del build x86-64, se registra `NO EJECUTADO` y el orquestador debe obtener
esa evidencia en CI antes de integrar. Nunca se declara que pasó una prueba no ejecutada.

## 4. Puertas de fase

| Puerta | Requisitos | Resultado binario |
|---|---|---|
| `GATE-A0` | ADR, baseline, inventario y ownership | Arquitectura aceptada |
| `GATE-A1` | Fixture GOAL AOT enlazado en Mach-O | Ejecución AOT mínima correcta |
| `GATE-A2` | Descriptores, llamadas y bridges estáticos | ABI AOT completa en tests |
| `GATE-A3` | Loader de datos y todos los objetos Jak II | Boot headless AOT reproducible |
| `GATE-A4` | Auditoría no-JIT en macOS | Cero código dinámico |
| `GATE-M0` | Backend Metal inicial y shaders offline | Frame sintético correcto |
| `GATE-M1` | Buckets principales | Escenas jugables comparables |
| `GATE-M2` | Efectos, UI y paridad | Renderer funcional completo |
| `GATE-I0` | Build y ciclo de vida iOS | Arranque headless en dispositivo |
| `GATE-I1` | Datos, audio, mando y touch | Partida controlable y persistente |
| `GATE-R0` | Campaña y matriz Apple | Candidato de release |

No existe “PASS con excepciones” si hay corrupción, ejecución desde datos, crash determinista,
save roto, regresión x86 o discrepancia ABI.

## 5. Tickets AOT

### AOT-000 — Gobernanza, ADR y congelación de baseline

Dependencias: ninguna.

Objetivo: autorizar formalmente el nuevo producto sin reinterpretar las decisiones del port JIT.

Archivos de coordinación:

- `AGENTS.md`
- `DECISIONS.md`
- `STATUS.md`
- `SOURCE_BASELINE.md`
- nuevos tickets `AOT-*`, `MTL-*`, `IOS-*` y `REL-*`

Procedimiento:

1. Registrar que la petición del propietario amplía el alcance a iOS y Metal.
2. Añadir una ADR que mantenga el JIT actual como backend de desarrollo y cree `AOT` como backend
   independiente. No sustituir ni reescribir la historia de la ADR de W^X.
3. Añadir una ADR para la representación de funciones mediante descriptores e IDs, inicialmente
   `propuesta` hasta terminar `AOT-002`.
4. Añadir una ADR para Metal como backend Apple de producción, conservando OpenGL.
5. Añadir una ADR para paquetes importables exclusivamente de datos.
6. Fijar el commit de integración del que parten estos tickets.
7. Registrar matriz de SDK, Xcode, macOS mínimo e iOS mínimo. Si el propietario no los ha decidido,
   dejar la decisión `BLOCKED`; no inventar versiones.
8. Crear ownership por carpetas para impedir que AOT y Metal modifiquen simultáneamente `gfx.*`,
   `runtime.*` o CMake.

Pruebas/evidencia:

- Baseline común completo.
- `git status --short` limpio.
- Tabla de archivos solapados entre tickets.

Aceptación:

- Todas las decisiones estructurales están registradas.
- No existe ningún ticket global asignado a un implementador.
- `GATE-A0` permanece pendiente hasta finalizar también `AOT-002` y `MTL-001`.

### AOT-001 — Modos de ejecución y matriz CMake

Dependencias: `AOT-000`.

Objetivo: separar en build-time el runtime dinámico y el runtime AOT sin condicionales dispersos.

Propiedad prevista:

- `CMakeLists.txt`
- `CMakePresets.json`
- `game/CMakeLists.txt`
- `goalc/CMakeLists.txt`
- nuevos archivos de configuración bajo `common/platform/` o equivalente

Implementación:

1. Introducir una opción CMake de tipo string `OPENGOAL_EXECUTION_MODE` con valores exactos
   `DYNAMIC` y `AOT`. Cualquier otro valor debe producir `FATAL_ERROR`.
2. Introducir `OPENGOAL_GRAPHICS_BACKEND` con `OPENGL` y `METAL`; Metal aún puede producir un error
   claro hasta `MTL-003`.
3. Generar un único header de configuración; no añadir decenas de `#if TARGET_OS_IPHONE`.
4. Definir capacidades constexpr: `kDynamicCode`, `kAotCode`, `kMetal`, `kIos`, `kMacos`.
5. Separar host tools de runtime. Una configuración iOS no debe intentar compilar `goalc`, extractor,
   editores, Discord RPC, REPL, servidor, diálogos de escritorio ni utilidades CLI.
6. Corregir la selección de SDK: `APPLE` no implica macOS. No ejecutar
   `xcrun --show-sdk-path` para sobrescribir un `iphoneos` o `iphonesimulator` proporcionado.
7. No usar `-march=native`, `-mavx`, `-mcrc` o stack linker flags de macOS en iOS.
8. No aplicar `macos/Info.plist.in` ni `jak-jit.entitlements` a targets iOS.
9. Añadir presets futuros con nombres estables:
   - `Release-macos-arm64-aot-metal`
   - `Debug-macos-arm64-aot-metal`
   - `Debug-ios-simulator-arm64-aot-metal`
   - `Release-ios-device-arm64-aot-metal`
10. Mantener byte-idéntica la configuración efectiva de los presets existentes, salvo cambios
    estrictamente necesarios y demostrados.

Tests:

```bash
cmake --preset Debug-macos-arm64-clang
cmake --build build-arm64-debug --parallel

cmake -S . -B /tmp/og-invalid-mode \
  -DOPENGOAL_EXECUTION_MODE=INVALID
# Debe fallar durante configure y explicar los valores permitidos.
```

Añadir tests CMake que inspeccionen la configuración generada para cada plataforma sin necesitar
assets. La regresión x86 debe continuar compilando.

Aceptación:

- El modo de ejecución se decide una sola vez durante configure.
- Ningún target iOS enlaza accidentalmente componentes de host.
- El target dinámico conserva sus tests y bytes de emisor.

### AOT-002 — Caracterización de la ABI de funciones GOAL

Dependencias: `AOT-000`.

Objetivo: medir antes de cambiar la representación de funciones.

Propiedad prevista: exclusivamente tests, fixtures y evidencia. Los archivos del runtime son
solo lectura.

Casos que deben quedar caracterizados:

1. Posición y valor del type tag de una función generada.
2. Offset exacto que se almacena en un símbolo de función.
3. Igualdad de dos referencias a la misma función.
4. Identidad tras recargar el mismo objeto.
5. Funciones en method tables, behaviors, states y callbacks.
6. Referencia a función desde datos estáticos.
7. Función anónima y función con nombre duplicado en objetos diferentes.
8. `nothing`, `zero`, bridges C y bridges con stack args.
9. Top-level de objetos y orden de ejecución al cargar un DGO.
10. Qué ocurre con una referencia retenida después de descargar o sobrescribir el heap de nivel.
11. Excepciones/catch frames atravesando llamadas directas, indirectas y bridges.
12. Inspección, impresión y debugger de una función.

Implementación del test:

- Extender `Jak2KernelTest` con fixtures sintéticos que no dependan del ISO.
- Guardar offsets, tags y resultados esperados; no verificar únicamente “no crash”.
- Añadir un fixture de recarga con dos versiones de un objeto que devuelvan valores distintos.
- Ejecutarlo en ARM64 y x86-64 para distinguir contrato GOAL de detalles del backend.

Comando mínimo:

```bash
build-arm64-debug/goalc-test --gtest_filter='Jak2FunctionModel.*'
arch -x86_64 build-x86_64-release/goalc-test --gtest_filter='Jak2FunctionModel.*'
```

Aceptación:

- Existe una tabla factual con cada comportamiento observado en ambas arquitecturas.
- El orquestador convierte la propuesta de descriptor en ADR aceptada o la corrige.
- Cualquier diferencia no explicada entre ARM64 y x86 bloquea `AOT-003`.

### AOT-003 — Esquema, IDs y manifiesto determinista

Dependencias: `AOT-002` y ADR de descriptor aceptada.

Objetivo: crear el modelo de datos AOT sin emitir todavía un juego completo.

Archivos previstos:

```text
goalc/aot/AotManifest.h
goalc/aot/AotManifest.cpp
goalc/aot/AotFunctionId.h
goalc/aot/AotNameMangler.h
goalc/aot/AotNameMangler.cpp
test/goalc/test_aot_manifest.cpp
```

Implementación:

1. Definir structs tipados para objetos, funciones, plantillas y relocalizaciones.
2. Definir serialización JSON canónica: claves en orden fijo, arrays ordenados y números en decimal.
3. Construir la clave de función desde `game/object/segment/ordinal`; el nombre GOAL es metadata,
   no identidad única.
4. Asignar IDs consecutivos después de ordenar todas las claves. Reservar ID 0.
5. Generar símbolos Mach-O con caracteres ASCII seguros y un hash estable de la clave.
6. Validar límites antes de convertir tamaños u offsets a `u32`.
7. Rechazar duplicados, IDs fuera de rango, referencias a objetos ausentes y schemas desconocidos.
8. Prohibir rutas absolutas mediante validación del manifest writer.
9. Incluir `schema_version`, `aot_abi_version`, `game_version`, target triple y hash de fuentes.
10. Separar el manifest de código del manifest de datos importables.

Tests obligatorios:

- Mismo input en distinto orden produce bytes idénticos.
- Dos funciones homónimas en objetos distintos reciben IDs distintos.
- Duplicado exacto falla.
- ID 0 falla.
- Overflow de offset/tamaño falla.
- Ruta absoluta o `..` falla.
- Schema futuro desconocido falla de forma limpia.
- Golden file pequeño revisable.

Aceptación: dos generaciones en directorios temporales distintos tienen el mismo SHA-256 de árbol.

### AOT-004 — Emisor de ensamblador Mach-O ARM64

Dependencias: `AOT-003`.

Objetivo: convertir una función GOAL mínima en código AOT que el assembler de Apple pueda enlazar.

Archivos previstos:

```text
goalc/aot/AotAssemblyWriter.h
goalc/aot/AotAssemblyWriter.cpp
goalc/aot/AppleArm64Assembly.cpp
test/goalc/aot-fixtures/
test/goalc/test_aot_assembly.cpp
```

Implementación:

1. Reutilizar la selección de instrucciones y asignación de registros ARM64 existente.
2. Emitir secciones Apple explícitas, alineación y símbolos globales/locales deterministas.
3. Para instrucciones sin símbolo, emitir una representación ensamblable estable.
4. Para referencias, usar expresiones Mach-O soportadas (`@PAGE`, `@PAGEOFF`, branch symbols o
   literal pools) y no parchear direcciones después del link.
5. No insertar direcciones absolutas, offsets observados ni asumir que dos símbolos quedan a menos
   de un rango sin delegar esa comprobación al linker.
6. Mantener `x18` prohibido, `sp` alineado a 16 y el contrato de registros GOAL existente.
7. Emitir `.subsections_via_symbols` cuando proceda y conservar cada función alcanzable desde el
   registro para que dead stripping no la elimine.
8. No escribir timestamps ni paths de fuentes. Los nombres de debug deben ser relativos.
9. Crear un fixture AOT con aritmética, branch, llamada interna, SIMD y retorno.
10. Compilar el fixture como parte del build, enlazarlo en un test separado y verificar resultados.

Comprobaciones manuales del objeto:

```bash
xcrun -sdk macosx clang -arch arm64 -c <fixture>.S -o <fixture>.o
file <fixture>.o
otool -hv <fixture>.o
otool -tvV <fixture>.o
nm -m <fixture>.o
```

Tests:

- Resultado escalar y SIMD exactos.
- Branch forward/backward.
- Símbolos con nombres problemáticos.
- Fallo del assembler ante una referencia no resuelta de fixture negativa.
- Comparación de bytes del modo dinámico antes/después: idénticos.

Aceptación `GATE-A1`:

- El fixture se ejecuta desde `__TEXT` de un binario enlazado.
- El test no reserva ni modifica memoria ejecutable.
- `vmmap`/inspección equivalente no muestra región JIT creada por el test.

### AOT-005 — Generación en dos etapas e integración de build

Dependencias: `AOT-004`.

Objetivo: impedir que una configuración cross-compilada intente ejecutar herramientas iOS.

Implementación:

1. Crear un target de herramientas host `aot-host-tools` que incluya `goalc` y packager.
2. Crear un comando `generate-jak2-aot` que consume un binario host ya construido.
3. Hacer que la build runtime reciba `OPENGOAL_AOT_GENERATED_DIR` como input de solo lectura.
4. Añadir dependencias CMake de cada `.S`, manifest y registry generado.
5. Hacer que un archivo generado ausente o de ABI incorrecta produzca un error de configure claro.
6. No regenerar AOT silenciosamente durante un build iOS firmado.
7. Añadir un script orquestador que ejecute: build host, generación, configure target y build.
8. Usar directorios diferentes para macOS, iphoneos e iphonesimulator.
9. Añadir todos los outputs generados a `.gitignore` sin ignorar fuentes reales ni manifests de
   fixtures.
10. Comparar dos generaciones completas y escribir `build-report.json` con hashes y conteos.

Interfaz de comandos que debe quedar funcionando:

```bash
cmake --preset Release-macos-arm64-clang
cmake --build build-arm64-release --target aot-host-tools --parallel

cmake --build build-arm64-release --target generate-jak2-aot

cmake --preset Release-macos-arm64-aot-metal \
  -DOPENGOAL_AOT_GENERATED_DIR=out/aot/jak2/macos-arm64
```

Los nombres exactos del binary dir pueden adaptarse al preset, pero se documentan y se prueban.

Aceptación: tocar una fuente GOAL regenera solo los objetos afectados y el registro necesario;
repetir sin cambios no altera ningún hash.

### AOT-006 — Descriptores y resolver C++

Dependencias: `AOT-003` y `AOT-005`.

Objetivo: resolver funciones precompiladas sin alterar todavía todas las llamadas generadas.

Archivos previstos:

```text
game/kernel/common/execution/GoalExecutionBackend.h
game/kernel/common/execution/DynamicExecutionBackend.cpp
game/kernel/common/execution/AotExecutionBackend.cpp
game/kernel/common/execution/AotFunctionDescriptor.h
game/kernel/common/kscheme.cpp
test/goalc/test_aot_runtime.cpp
```

Implementación:

1. Centralizar `resolve`, creación de función, llamada normal y llamada con stack en una interfaz.
2. `DynamicExecutionBackend` delega en el comportamiento actual sin cambiar bytes ni layout.
3. `AotExecutionBackend` valida que el offset esté dentro de memoria EE y alineado.
4. Validar type tag, magic, ID, generación, ABI y presencia de entrypoint.
5. El resolver devuelve una función nativa del registry, nunca `g_ee_main_mem + offset`.
6. Separar claramente errores: null, descriptor corrupto, ID desconocido, ABI incompatible y objeto
   descargado.
7. Hacer que `call_goal`, `call_goal_on_stack` y `call_goal_function` utilicen el backend central.
8. Mantener intacto el bridge ensamblador existente para `DYNAMIC`.
9. Crear un registry fixture estático enlazado al test.
10. Añadir contadores debug opcionales por ID, desactivados en Release.

Tests obligatorios:

- Llamada válida con 0, 3 y 8 argumentos.
- Retorno entero y SIMD.
- Llamada anidada y recursiva.
- Descriptor null/corrupto/fuera de rango.
- Generation antigua tras recarga.
- Registro vacío e ID máximo.
- Excepción GOAL atravesando resolver.
- Regresión completa `Jak2KernelTest.*` dinámica.

Aceptación: ninguna ruta C++ de llamada AOT convierte un `Ptr<Function>` directamente en dirección
ejecutable.

### AOT-007 — Llamadas GOAL directas e indirectas

Dependencias: `AOT-006`.

Objetivo: enseñar al codegen ARM64 AOT a invocar código enlazado.

Propiedad prevista:

- `goalc/compiler/IR.*`
- `goalc/compiler/CodeGenerator.*`
- nuevos helpers AOT del emitter
- tests AOT específicos

Implementación:

1. Añadir el modo AOT al contexto de codegen, no inferirlo desde `__APPLE__`.
2. Para llamada directa conocida, emitir llamada a símbolo o stub enlazable.
3. Para llamada indirecta, cargar el descriptor, validar/resolver mediante la convención definida y
   ejecutar el entrypoint.
4. Preservar registros GOAL, `s7`, process pointer, stack pointer y registros SIMD según ABI.
5. No reservar un nuevo registro global sin ADR y matriz de liveness.
6. Soportar tail position únicamente después de una prueba específica; inicialmente usar llamada y
   retorno normales.
7. Mantener el codegen dinámico en una función separada y comparar sus bytes con fixtures previos.
8. Añadir mensajes de linker que incluyan objeto y función cuando falta un símbolo nativo.
9. Probar calls entre objetos y segmentos.
10. Probar una función almacenada en símbolo, method table, vector y closure/estado si aplica.

Tests:

```bash
build-arm64-debug/goalc-test --gtest_filter='AotGoalCall.*:ARM64RuntimeBridge.*'
```

Debe existir un ejecutable de fixture enlazado en build-time; compilar GOAL dentro del test y saltar
a un buffer ejecutable no cuenta como prueba AOT.

Aceptación: todas las variantes de aridad usadas por Jak II tienen retorno y preservación de
registros comprobados.

### AOT-008 — Estáticos, símbolos y relocalizaciones de datos

Dependencias: `AOT-007`.

Objetivo: permitir que código AOT acceda a datos cuya dirección EE se decide al cargar.

Implementación:

1. Crear un `AotObjectInstanceTable` de datos RW con offsets EE por objeto/estático.
2. Generar para cada acceso estático una clave estable, no una dirección.
3. Resolver tipos y símbolos mediante las tablas GOAL actuales cuando su valor sea dinámico.
4. Aplicar relocalizaciones únicamente al copiar una plantilla de datos.
5. Validar source range, destination range, alineación y overflow antes de escribir.
6. Aplicar todas las validaciones antes de mutar el heap; un objeto inválido no deja una carga
   parcial.
7. Mantener un journal de las escrituras o construir en scratch y publicar atómicamente.
8. Soportar referencias internas, cross-object y a función mediante descriptor.
9. Rechazar expresamente native pointers y code relocations.
10. Añadir dump diagnóstico por objeto sin incluir direcciones ASLR en evidencia reproducible.

Tests:

- Plantilla vacía y plantilla máxima permitida.
- Relocalización a inicio/fin válido.
- Overflow, underflow, destino desalineado y objeto ausente.
- Símbolo modificado después de una carga.
- Función referenciada desde datos.
- Rollback íntegro ante el último relocation inválido.
- Hash idéntico de memoria EE entre loader dinámico y AOT para fixture equivalente.

Aceptación: una fixture con tipos, strings, símbolos, estáticos y function references ejecuta el
mismo resultado en `DYNAMIC` y `AOT`.

### AOT-009 — Bridges C++ estáticos

Dependencias: `AOT-007` y `AOT-008`.

Objetivo: sustituir los trampolines creados por `make_function_from_c*`.

Implementación:

1. Inventariar todas las funciones C registradas por Jak II y clasificarlas por firma:
   arg-call, arg3-as-process-pointer y stack-args.
2. Generar un registry C++ estático con ID, puntero firmado, firma y nombre.
3. Crear entrypoints genéricos precompilados para cada firma soportada.
4. Incluir el ID de bridge en el descriptor para que el entrypoint genérico seleccione la función.
5. En AOT, `make_function_from_c*` busca el puntero en el registry y crea solo el descriptor RW.
6. Un puntero no registrado debe fallar; no se crea un thunk dinámico.
7. `nothing` y `zero` se convierten en funciones ensambladas/C++ estáticas registradas.
8. Conservar las rutas dinámicas actuales sin cambios.
9. Validar punteros completos de 64 bits y pointer authentication si el target futuro lo exige; no
   truncar ni almacenar el puntero nativo en memoria EE.
10. Generar un informe de cobertura: cada registro Jak II debe mapear a un ID.

Tests:

- Matriz de 0–8 argumentos.
- `arg3_is_pp` verdadero y falso.
- Stack args con límites y alineación impar/par.
- Retorno entero, puntero EE y vectorial.
- Nested C → GOAL → C.
- Puntero desconocido rechazado.
- `nothing` y `zero` exactos.

Aceptación: el target AOT no compila ni enlaza el emisor de trampolines C.

### AOT-010 — Registro MIPS2C estático

Dependencias: `AOT-009`.

Objetivo: sustituir los stubs emitidos por `mips2c_table.cpp`.

Implementación:

1. Inventariar todas las declaraciones `def-mips2c` y `defmethod-mips2c` de Jak II.
2. Generar una tabla estática con nombre, ID, execute pointer y stack size.
3. Usar un entrypoint AOT genérico que construya `ExecutionContext` y llame al execute pointer.
4. Mantener la alineación y layout comprobados por `ARM64RuntimeBridge`.
5. Crear descriptores GOAL normales que referencien IDs MIPS2C.
6. Rechazar duplicados, stack size fuera de rango y declaración sin implementación.
7. Hacer que el build falle si el inventario fuente y el registry difieren.
8. Excluir el emisor de trampoline MIPS2C del target AOT.
9. Mantener dinámica la ruta existente para los targets no AOT.
10. Guardar conteo y hashes del registry en `build-report.json`.

Tests:

- Ejecutar cada variante de stack size del fixture.
- Verificar los registros y memoria modificada, no solo el retorno.
- Ejecutar nested GOAL → MIPS2C → GOAL.
- Comprobar que los aproximadamente 134 enlaces declarados quedan resueltos exactamente una vez.
- Probar duplicado y entrada ausente.

Aceptación `GATE-A2`: tests de llamadas, C bridge y MIPS2C pasan en AOT y todas las regresiones
dinámicas continúan pasando.

### AOT-011 — Loader de objetos exclusivamente de datos

Dependencias: `AOT-008` y `AOT-010`.

Objetivo: cargar un objeto AOT sin copiar ni ejecutar código de su archivo de datos.

Archivos previstos:

```text
game/kernel/common/aot/AotObjectLoader.*
game/kernel/jak2/aot/AotJak2ObjectLoader.*
goalc/aot/AotDataTemplate.*
test/goalc/test_aot_object_loader.cpp
```

Implementación:

1. Definir header con magic, schema, game, object ID, tamaños y SHA-256.
2. Leer con límites estrictos; ninguna suma de offsets puede desbordar.
3. Descomprimir, si se usa compresión, a un buffer con tamaño máximo previamente validado.
4. Validar manifest y todas las relocalizaciones antes de reservar en el heap definitivo.
5. Copiar plantillas de datos, crear descriptores y aplicar relocalizaciones.
6. Publicar símbolos y method tables solo cuando todo lo anterior ha tenido éxito.
7. Invocar top-level mediante el registry AOT en el orden del manifest.
8. En error, restaurar heap, symbols y tablas al estado anterior.
9. Registrar métricas de bytes, tiempo y número de relocations sin direcciones privadas.
10. Prohibir cualquier campo de código; un schema que lo incluya se rechaza.

Tests de seguridad y límites:

- Header truncado, tamaño enorme, hash incorrecto y schema desconocido.
- Path traversal en nombres.
- Relocation fuera de rango y duplicada.
- Error en top-level con rollback definido.
- Objeto sin datos, sin funciones o sin top-level.
- Repetición de carga y descarga.

Aceptación: el loader pasa fuzzing del parser y nunca cambia protecciones de memoria.

### AOT-012 — Semántica DGO, overlays y orden de inicialización

Dependencias: `AOT-011`.

Objetivo: reproducir la semántica de `link_and_exec` sin enlazar código en runtime.

Implementación:

1. Generar un manifest DGO con lista ordenada de object IDs y hashes.
2. Separar `load bytes` de `instantiate object`; mantener double buffering solo para datos.
3. Conservar exactamente el orden de top-level y actualizaciones de símbolos.
4. Modelar objetos comunes, objetos por nivel y overlays con la misma vida de heap.
5. Invalidar generaciones al descargar para detectar function handles obsoletos.
6. Implementar fast path únicamente después de que la ruta normal tenga paridad.
7. Conservar errores con nombre de DGO/objeto y etapa.
8. No aceptar un DGO de la arquitectura dinámica por accidente.
9. Añadir una marca de formato inequívoca `AOT-DATA`; no inferirlo por extensión.
10. Crear fixture de dos niveles con símbolo sobreescrito, unload y reload.

Tests:

- Comparar trace de carga dinámico/AOT: objetos, top-level, symbols y heap offsets.
- Referencia antigua después de unload produce error determinista.
- Cancelación durante carga no publica estado parcial.
- DGO con orden alterado produce resultado distinto conocido, demostrando que el test observa el
  orden real.
- Fast/normal producen el mismo hash final.

Aceptación: `GAME`, kernel y dos fixtures de nivel llegan al mismo snapshot lógico en ambos modos.

### AOT-013 — Compilación completa de Jak II

Dependencias: `AOT-012`.

Objetivo: generar código AOT y datos para todos los objetos de Jak II.

Procedimiento por fallo:

1. Detenerse en el primer objeto que falla.
2. Guardar objeto, función, IR y diagnóstico completo.
3. Reducir a fixture mínimo.
4. Clasificar como emitter, ABI, manifest, static, symbol, C bridge, MIPS2C o loader.
5. Crear ticket propietario; no acumular arreglos no relacionados en integración.
6. Añadir regresión y repetir desde el objeto fallido.

Comprobaciones:

- Todos los objetos de código del inventario tienen manifest.
- Toda función tiene ID y símbolo nativo.
- Toda referencia está resuelta.
- Todo top-level aparece en el orden de build.
- Todo MIPS2C está registrado.
- No aparece ningún `NYI`, fallback x86 ni target triple incorrecto.
- Dos generaciones limpias tienen hashes idénticos.
- Ningún output contiene rutas del host.

Comandos objetivo:

```bash
cmake --build build-arm64-release --target generate-jak2-aot --parallel
cmake --build build-arm64-release --target verify-jak2-aot-determinism
cmake --build build-arm64-release --target verify-jak2-aot-completeness
```

Aceptación: el linker Apple produce el runtime AOT completo sin símbolos ausentes y el informe de
conteos coincide con el inventario de fuentes.

### AOT-014 — Boot headless AOT de Jak II en macOS

Dependencias: `AOT-013`.

Objetivo: arrancar kernel y game loop sin renderer y sin código dinámico.

Implementación:

1. Reservar memoria EE mediante una clase `EeDataRegion` RW, separada de `JitRegion`.
2. Aplicar `PROT_NONE` únicamente a las páginas de guardia.
3. Inicializar symbols, heaps, kernel, registries y datos AOT.
4. Cargar `GAME` y avanzar hasta un punto estable del dispatcher headless.
5. Añadir un modo test con número limitado de dispatches; no usar sleeps como sincronización.
6. Capturar un snapshot lógico: símbolos clave, heap tops, procesos, retorno de top-level y estado
   de loader.
7. Comparar ese snapshot contra el modo dinámico del mismo contenido.
8. Mantener stacks separados por proceso y todas las pruebas de catch-frame existentes.
9. Fallar si el backend solicitado y los manifests no comparten ABI/version.
10. No inicializar SDL video en este target.

Tests:

```bash
<aot-headless-test> --game jak2 --dispatch-count 10000 --snapshot <output>
```

- Tres ejecuciones producen el mismo snapshot, salvo campos explícitamente normalizados.
- ASan/UBSan host donde sea compatible.
- `Jak2KernelTest.*` completo.

Aceptación `GATE-A3`: boot headless AOT reproducible y sin crash durante la ventana de dispatch
acordada.

### AOT-015 — Eliminación y auditoría de JIT en targets AOT

Dependencias: `AOT-014`.

Objetivo: convertir “no usamos JIT en esta prueba” en una propiedad comprobable del artefacto.

Implementación:

1. Excluir del target AOT `jit_memory`, linker de código y emisores de trampolines.
2. Hacer que cualquier llamada a una API de código dinámico no esté disponible en compile-time bajo
   `AOT`; no dejar stubs que simplemente no hagan nada.
3. Eliminar `allow-jit` del target AOT y mantenerla solo en el bundle dinámico que la necesita.
4. Añadir `scripts/verify_apple_no_jit.py` con entrada de binary y bundle.
5. Verificar entitlements, símbolos importados, load commands, segmentos y permisos.
6. Permitir `mmap`/`mprotect` solo para datos/guard pages; no basarse exclusivamente en buscar esos
   nombres.
7. En test Debug, enumerar las regiones propias y fallar ante memoria anónima ejecutable o W+X.
8. Ejecutar una carga de nivel antes de repetir la inspección de regiones.
9. Escanear el mapa de link para demostrar que no se enlazaron emisores dinámicos.
10. Documentar todas las regiones ejecutables y asociarlas a una imagen Mach-O firmada.

Comandos de puerta macOS:

```bash
codesign -d --entitlements :- <AOT_APP>
otool -l <AOT_BINARY>
nm -u <AOT_BINARY>
python3 scripts/verify_apple_no_jit.py --bundle <AOT_APP>
```

Casos negativos:

- Fixture con entitlement `allow-jit`: debe fallar.
- Binary fixture que enlaza el trampoline emitter: debe fallar.
- Región anónima RX creada por fixture de test: debe fallar.

Aceptación `GATE-A4`: auditoría estática y runtime pasan después de boot y carga de nivel.

### AOT-016 — Bundle AOT de macOS y convivencia con desarrollo

Dependencias: `AOT-015` y `MTL-014`.

Objetivo: integrar AOT+Metal como build de producto sin eliminar el workflow dinámico.

Implementación:

1. Crear nombres de producto inequívocos para evitar abrir por accidente el bundle dinámico.
2. Bundle AOT: Metal, sin REPL, sin servidor, sin JIT entitlement y sin argumentos obligatorios.
3. Bundle dev: comportamiento actual, renderer seleccionable y JIT entitlement solo si usa dynamic.
4. Compartir settings/save format, pero separar caches de shaders y renderer si su schema difiere.
5. Migrar settings de forma versionada y con rollback.
6. Usar fullscreen nativo de macOS y propagar errores; no guardar fullscreen si falló.
7. Configurar categoría de aplicación de juegos y comprobar activación de Game Mode mediante la
   conducta soportada por macOS, sin APIs privadas ni hacks.
8. Validar que todos los binarios y dylibs del bundle AOT son `arm64`.
9. Preparar firma/notarización, pero no realizarlas sin ticket y autorización explícita.
10. Documentar el comando exacto de lanzamiento del bundle AOT.

Aceptación: ambos bundles pueden coexistir; el AOT juega sin Rosetta/JIT y el dev conserva REPL.

## 6. Tickets Metal

### MTL-001 — Inventario OpenGL y baseline visual

Dependencias: `AOT-000`.

Objetivo: convertir el renderer actual en una especificación observable antes de portarlo.

Propiedad prevista: tests, herramientas de captura y documentación. Renderer de producción en modo
solo lectura.

Inventario obligatorio:

1. Todas las llamadas OpenGL agrupadas por recurso, estado, draw, sync y readback.
2. Todos los shaders con inputs, outputs, uniforms, samplers, defines y variantes.
3. Todos los formatos de textura, depth/stencil y framebuffer.
4. Estado de blend, depth, stencil, culling, scissor, viewport y color mask por bucket.
5. Dependencias GL incrustadas en tipos públicos, especialmente `TexturePool`.
6. Uso de `glTexImage1D`, `glGetTexImage`, `glPolygonMode`, multidraw y primitive restart.
7. Orden exacto de buckets de Jak II y render targets intermedios.
8. Puntos de sincronización CPU/GPU y lecturas que puedan bloquear.
9. Funciones de ImGui, screenshot, profiler y debug que dependan de GL.
10. Requisitos de macOS que no existen en iOS.

Herramientas a añadir:

- Dump JSON de una frame con bucket, draw count, pipeline state lógico, recursos y hashes de inputs.
- Captura de imágenes en puntos definidos del frame.
- Escenas sintéticas sin assets comerciales para triángulos, depth, blend, texture, stencil y MSAA.

Baseline con assets locales, guardado solo en evidencia no pública:

- Logos iniciales.
- Menú.
- Escena de ciudad con TFRAG/TIE/Merc.
- Agua/cielo.
- Sombra.
- Partículas/glow/postprocesado.
- UI/subtítulos.

Las imágenes con assets no se versionan. En el repositorio solo se guardan hashes, tolerancias y
fixtures sintéticos propios.

Aceptación para `GATE-A0`: cada llamada y shader tiene un ticket Metal propietario; no existe una
categoría “ya se resolverá después”.

### MTL-002 — Tipos de recursos independientes de OpenGL

Dependencias: `MTL-001`.

Objetivo: sacar `GLuint` y headers GL de las interfaces compartidas sin cambiar el renderer OpenGL.

Propiedad prevista:

- `game/graphics/texture/TexturePool.*`
- nuevos `game/graphics/backend/GraphicsHandles.*`
- adaptadores OpenGL mínimos
- tests del texture pool

Implementación:

1. Crear handles opacos tipados para texture, buffer, sampler, render target y pipeline.
2. Cada handle contiene índice y generación; valor cero es inválido.
3. Mover el objeto GL real a un registry privado del backend OpenGL.
4. `TextureData`, `TextureVRAMReference` y `TextureInput` dejan de exponer `GLuint`.
5. Separar metadata CPU de ownership GPU.
6. Mantener el lookup rápido por VRAM slot y medir que no introduce asignaciones en draw.
7. Definir explícitamente quién crea, retiene y destruye cada recurso.
8. Al descargar un nivel, invalidar generación y detectar usos posteriores.
9. No crear una abstracción que copie toda la API OpenGL; modelar necesidades del juego.
10. Mantener las funciones GL dentro de `opengl_renderer`/adaptador.

Tests:

- Insert, replace, relocate, placeholder, duplicate, unload y stale handle.
- Concurrencia loader/render con el sanitizer disponible.
- Frame trace OpenGL antes/después idéntico.
- Rendimiento de lookup dentro del margen fijado por `MTL-001`.

Aceptación: ningún header compartido de texture/backend incluye GL y OpenGL mantiene imágenes de
baseline.

### MTL-003 — Dispositivo Metal, ventana y frame vacío

Dependencias: `AOT-001` y `MTL-002`.

Objetivo: producir un frame Metal válido en macOS y en el simulador iOS.

Archivos previstos:

```text
game/graphics/pipelines/metal.h
game/graphics/pipelines/metal.mm
game/graphics/metal/MetalContext.h
game/graphics/metal/MetalContext.mm
game/graphics/metal/MetalDisplay.h
game/graphics/metal/MetalDisplay.mm
```

Implementación:

1. Añadir `GfxPipeline::Metal` y seleccionar por build/setting validado.
2. Crear ventana SDL con flags Metal, `SDL_Metal_CreateView` y `SDL_Metal_GetLayer`.
3. Crear `MTLDevice` y command queue; fallar con diagnóstico si no existen.
4. Elegir pixel format, colorspace, depth/stencil, sample count y drawable size explícitos.
5. Actualizar drawable size ante Retina, resize y orientación; usar pixels, no puntos lógicos.
6. Implementar autorelease pool por frame en Objective-C++.
7. Crear command buffer, render pass de clear, presentar drawable y completar frame.
8. Limitar frames en vuelo con semáforo; no hacer `waitUntilCompleted` cada frame.
9. Propagar `MTLCommandBufferError` y device removal al estado de runtime.
10. Destruir view, layer y objetos Metal en orden seguro con la GPU drenada.

Tests:

- Clear rojo/verde/azul y lectura de píxel en fixture.
- Resize repetido, minimizado/oculto en macOS y cambio de escala.
- 1 000 frames sin crecimiento de objetos/allocations significativo.
- Error simulado al no disponer de drawable.
- Backend OpenGL continúa seleccionable.

Aceptación `GATE-M0` parcial: frame vacío reproducible en macOS y simulador, sin warning de
validación Metal.

### MTL-004 — Compilación offline de shaders y contratos CPU/GPU

Dependencias: `MTL-003`.

Objetivo: crear `metallib` durante el build y eliminar shader compilation del runtime.

Estructura prevista:

```text
game/graphics/metal/shaders/
├── common/
├── synthetic/
└── jak2/
game/graphics/metal/ShaderLibrary.*
scripts/verify_metallib.py
```

Implementación:

1. Añadir shaders MSL sintéticos para vertex color, texture, depth y blend.
2. Definir structs compartidos C++/MSL con tamaños, offsets y alineación comprobados mediante
   `static_assert` y tests de reflection/build metadata.
3. Compilar `.metal` a AIR y después a `metallib` para cada SDK.
4. No fijar la versión de Metal language hasta que `AOT-000` fije deployment targets.
5. Tratar warnings de shader como errores en CI.
6. Empaquetar la `metallib` en Resources del bundle correspondiente.
7. Cargar exclusivamente la librería precompilada; no llamar a APIs de compilación desde source.
8. Buscar todas las funciones requeridas durante init y fallar listando las ausentes.
9. Generar un manifest con nombre, stage, layouts y hash de cada función.
10. Comparar manifest contra los pipelines declarados.

Comandos de inspección:

```bash
xcrun -sdk macosx metal <flags> -c <shader>.metal -o <shader>.air
xcrun -sdk macosx metallib <shader>.air -o <shader>.metallib
xcrun -sdk iphoneos metal <flags> -c <shader>.metal -o <shader>.air
xcrun -sdk iphoneos metallib <shader>.air -o <shader>.metallib
```

Los `<flags>` se obtienen del target y quedan registrados; no se copian a mano entre SDK.

Tests:

- Falta de shader y stage incorrecto producen error claro.
- Cambio de layout CPU sin actualizar MSL falla en build/test.
- La `metallib` no contiene paths absolutos.
- El binary no importa APIs de compilación de shader desde source.

Aceptación `GATE-M0`: triángulo texturizado sintético correcto en macOS y simulador usando solo
`metallib`.

### MTL-005 — Recursos, uploads y render targets

Dependencias: `MTL-004`.

Objetivo: implementar la infraestructura que compartirán todos los buckets.

Implementación:

1. Registry RAII para buffers, textures, samplers, depth/stencil y pipelines.
2. Allocator de buffers dinámicos por frame con límites, wrap y fences.
3. Upload de texturas RGBA y formatos realmente presentes en el inventario.
4. Mipmaps: cargar precomputados cuando existan; generar mediante blit solo cuando sea semánticamente
   equivalente.
5. Sampler cache por filter/wrap/aniso con clave completa.
6. Render target cache por tamaño, formato, sample count y usage.
7. MSAA resolve explícito.
8. Readback asíncrono mediante staging buffer; prohibir waits en mitad del frame salvo test.
9. CPU shadow copy para recursos que el juego consulta frecuentemente.
10. Resource labels con nombres de objeto/bucket para capturas Xcode.
11. Budget y telemetría de memoria, sin depender de que allocation siempre tenga éxito.
12. Destrucción diferida hasta que ningún command buffer use el recurso.

Tests:

- Upload y readback de tamaños 1x1, NPOT y límites acordados.
- Formatos, swizzle y endianness exactos.
- Mipmap y sampling golden sintético.
- Stale handle y double destroy.
- Simulación de out-of-memory sin fuga ni estado parcial.
- 10 000 ciclos de create/use/destroy con contadores estables.

Aceptación: TexturePool funciona con un fake backend y con OpenGL/Metal sin tipos nativos en su API.

### MTL-006 — DMA, sincronización y dispatcher de buckets

Dependencias: `MTL-005`.

Objetivo: alimentar Metal con la misma DMA chain que OpenGL.

Implementación:

1. Reutilizar `FixedChunkDmaCopier` y loader sin copiar de nuevo el frame completo.
2. Separar parsing de DMA, preparación CPU y comandos de backend.
3. Definir `RenderFrameInput` inmutable mientras la GPU lo usa.
4. Mantener frame index, level set y texture relocations en el mismo orden.
5. Implementar `send_chain`, `texture_upload_now`, `texture_relocate`, `set_levels`,
   `set_active_levels` y reloads del `GfxRendererModule` Metal.
6. Sustituir condition variables ambiguas por predicados y contadores de frame comprobados.
7. No bloquear el EE thread en trabajo GPU salvo el contrato real de `sync_path`/vsync.
8. Implementar un bucket desconocido como error visible con ID y offset, no skip silencioso.
9. Emitir frame trace Metal en el mismo schema que OpenGL.
10. Añadir modo `parse-only` para comparar buckets sin dibujar.

Tests:

- Cadena vacía, truncada, bucket desconocido y tamaño fuera de rango.
- Dos frames alternando buffers sin data race.
- Texture upload concurrente con unload.
- Trace parse-only idéntico entre backends para fixtures.
- TSAN en componentes CPU donde sea viable.

Aceptación: Metal recibe todos los buckets de un frame real y produce un trace completo aunque los
buckets aún no renderizados se marquen como `UNIMPLEMENTED` y bloqueen la puerta visual.

### MTL-007 — Direct, blit, progress y sprite básico

Dependencias: `MTL-006`.

Objetivo: portar las rutas 2D y directas necesarias para boot, logos y UI básica.

Componentes:

- `BlitDisplays`
- `ProgressRenderer`
- `DirectRenderer`
- `DirectRenderer2`
- sprite básico
- depth cue común necesario

Procedimiento por componente:

1. Documentar inputs DMA y estados GL usados.
2. Crear layout C++/MSL con asserts.
3. Crear pipeline Metal y clave de estado.
4. Portar texturas, blend, scissor, depth y color mask.
5. Comparar draw count y frame trace.
6. Comparar captura con umbral definido en `MTL-001`.
7. Añadir test sintético del caso límite.
8. Marcar el componente completo solo cuando no exista fallback OpenGL dentro del frame Metal.

Casos obligatorios:

- Alpha blend y premultiplicación.
- Clipping/scissor.
- Texture coordinates y pixel center.
- Letterbox y escalado Retina.
- Splash/progress sin asset externo mediante fixture sintético.

Aceptación: logos y menú pueden representarse correctamente mediante Metal.

### MTL-008 — TFRAG

Dependencias: `MTL-007`.

Objetivo: portar geometría estática principal y visibilidad.

Implementación:

1. Reutilizar resultados CPU de loader; separar cualquier upload GL restante.
2. Definir vertex layouts por variante TFRAG y comprobar offsets.
3. Portar culling, draw modes, texture selection, fog/depth cue y color.
4. Sustituir multidraw inicialmente por draws agrupados deterministas.
5. Mantener cache por level y liberar al unload.
6. Portar variantes no-texture y debug solo después de la ruta normal.
7. Comparar número de árboles, draws, índices y texturas por frame.
8. Capturar zonas cercanas/lejanías para comprobar precision y LOD.
9. Medir CPU encode/GPU time y allocations.
10. Optimizar con indirect commands únicamente en ticket posterior si el perfil lo justifica.

Tests:

- Índices en límites, strip restart y geometría vacía.
- LOD y vis data conocidos.
- Carga/unload/reload de nivel.
- Golden sintético de fog y texture sampling.

Aceptación: TFRAG no presenta geometría ausente, corrupta ni Z-fighting introducido frente al
baseline acordado.

### MTL-009 — TIE, shrub y HFRAG

Dependencias: `MTL-008`.

Objetivo: completar fondos instanciados, vegetación y geometría HFRAG.

Implementación:

1. Portar layouts e instancing de TIE.
2. Reemplazar texturas 1D de time-of-day por 2D de altura 1.
3. Portar wind/deformation y comprobar precision SIMD/float.
4. Portar shrub con alpha test/discard equivalente y ordering correcto.
5. Portar HFRAG y montage resources.
6. Reemplazar multidraw con batches y después perfilar.
7. Mantener visibility/LOD y unload por level.
8. Añadir counters por renderer comparables con OpenGL.
9. Probar escenas densas y cambios de time-of-day.
10. Comprobar que no hay pipeline creation durante gameplay normal después del warmup.

Aceptación: ciudad y exteriores muestran fondos, props y vegetación completos sin crecimiento de
memoria al cambiar repetidamente de nivel.

### MTL-010 — Merc, generic, eyes y personajes

Dependencias: `MTL-009`.

Objetivo: portar personajes y geometría foreground.

Implementación:

1. Portar Merc2 y Generic2 conservando skinning, morph, lighting y fog.
2. Determinar qué cálculo permanece CPU y qué shader recibe matrices/paletas.
3. Implementar ring buffers sin sobrescribir datos de frames en vuelo.
4. Portar EyeRenderer y sus render targets intermedios.
5. Resolver texture animation/readbacks mediante shadow copy o blits planificados.
6. Portar EMERC y variantes usadas por Jak II.
7. Comparar joints, draw calls, material state y bounds.
8. Probar personaje quieto, animación rápida, múltiples personajes y cambio de nivel.
9. Validar NaN/Inf en matrices antes de enviar a GPU en Debug.
10. Capturar casos de transparencia y ojos en primer plano.

Aceptación: modelos, animación, ojos, materiales y transparencia coinciden dentro de la tolerancia
visual y no hay corrupción temporal entre frames.

### MTL-011 — Cielo, océano y efectos de entorno

Dependencias: `MTL-010`.

Objetivo: portar renderers que combinan render targets y sampling especial.

Implementación:

1. Portar SkyRenderer y SkyBlend, distinguiendo rutas CPU/GPU.
2. Portar OceanTexture, OceanEnvmap y haze.
3. Sustituir dependencias de estado implícito GL por inputs de pipeline explícitos.
4. Definir formatos y precision de render targets intermedios.
5. Programar barriers/encoder boundaries solo donde haya dependencia real.
6. Probar horizon, cámara en movimiento y transiciones de color.
7. Evitar readback por frame.
8. Comparar captura lineal o sRGB de forma coherente; no comparar bytes en spaces distintos.
9. Medir bandwidth y memoria de targets.
10. Añadir fallback de calidad explícito para dispositivos con budget menor, sin cambiar gameplay.

Aceptación: cielo, agua, envmap y haze son estables y no producen flicker ni cambios de gamma.

### MTL-012 — Sombras, glow, warp, slow-time y postprocesado

Dependencias: `MTL-011`.

Objetivo: completar todos los efectos restantes de la imagen final.

Componentes mínimos:

- Shadow y Shadow2.
- Glow probes, downsample y draw.
- Warp/distortion.
- Slow-time.
- Sprite distortion.
- Collision/debug rendering necesario para QA.
- TextureAnimator restante.

Implementación:

1. Crear un pequeño frame graph explícito de passes y dependencias.
2. Reutilizar render targets solo cuando format, size, samples y lifetime sean compatibles.
3. Implementar clear/load/store actions correctas; no depender del contenido anterior.
4. Portar stencil y depth compare exactamente.
5. Para wireframe, usar pipeline/índices específicos; no depender de polygon mode inexistente.
6. Programar readbacks asíncronos fuera del camino crítico.
7. Comparar cada pass aisladamente antes de comparar frame final.
8. Validar orden de transparencias y blending.
9. Medir GPU counters de las escenas más caras.
10. Documentar cualquier diferencia intencional con captura y ADR, no con comentario vago.

Aceptación: no quedan shaders GLSL requeridos por el backend Metal ni buckets de Jak II marcados
como no implementados.

### MTL-013 — ImGui, screenshots, profiler y herramientas

Dependencias: `MTL-012`.

Objetivo: restaurar herramientas sin contaminar el producto iOS.

Implementación:

1. Integrar backend ImGui SDL3 + Metal en macOS dev.
2. Hacer opcional toda herramienta mediante target/source list, no runtime dead code en iOS.
3. Implementar screenshots desde el drawable o target final con conversión de color correcta.
4. Adaptar profiler GPU a command buffers/signposts disponibles.
5. Mantener subtitle/filter tools solo donde estén soportadas.
6. No incluir file dialogs de escritorio en iOS.
7. Probar mostrar/ocultar ImGui sin cambiar input del juego inesperadamente.
8. Probar screenshot Retina y orientación.
9. Evitar que profiler fuerce una espera GPU por frame.
10. Verificar ausencia de ImGui/debug symbols opcionales en Release iOS si así se configura.

Aceptación: herramientas macOS disponibles; iOS producto no enlaza componentes desktop.

### MTL-014 — Paridad, rendimiento y backend Metal por defecto

Dependencias: `MTL-013`.

Objetivo: cerrar el renderer como producto, no como demo.

Procedimiento:

1. Ejecutar todas las escenas definidas en `MTL-001` en OpenGL y Metal.
2. Comparar frame traces antes de imágenes.
3. Comparar capturas con color space normalizado y máscaras para contenido no determinista.
4. Clasificar diferencias por bucket/pass y corregir la primera divergencia.
5. Ejecutar 30 minutos de cambio de nivel repetido y observar memoria.
6. Capturar Xcode GPU frame sin errores de validación.
7. Medir frame time CPU, encode, GPU, stutter, pipeline creation y memoria.
8. Eliminar creación de pipelines y compilación de shaders durante gameplay.
9. Fijar budgets por clase de dispositivo después de medir, no antes.
10. Seleccionar Metal por defecto solo en builds Apple AOT; mantener override OpenGL en macOS dev.

Aceptación `GATE-M2`:

- Cero buckets requeridos sin implementar.
- Cero errores de validación Metal.
- Cero crecimiento no acotado de recursos.
- Paridad visual aprobada para la matriz de escenas.
- Rendimiento sostenido dentro de los budgets registrados.

## 7. Tickets de plataforma iOS

### IOS-001 — Target iOS, dependencias y app mínima

Dependencias: `AOT-001`, `AOT-015`, `MTL-004`.

Objetivo: compilar una aplicación iOS vacía que enlace solo runtime AOT y Metal.

Implementación:

1. Añadir targets separados para `iphoneos` ARM64 y `iphonesimulator` ARM64.
2. Crear bundle identifier parametrizable sin guardar team ID ni perfiles en el repositorio.
3. Crear `Info.plist` iOS específico con orientación, categoría de juego y requisitos declarados.
4. Añadir frameworks mediante targets: Metal, QuartzCore, UIKit, GameController, AVFoundation y los
   que SDL requiera realmente. No usar `-framework` global sin propietario.
5. Compilar SDL3 con su backend iOS y desactivar drivers/plataformas innecesarios mediante opciones
   soportadas.
6. Excluir `discord-rpc`, `libtinyfiledialogs`, OpenGL desktop, REPL, server, decompiler, compiler,
   editor tools y cualquier `dlopen` de plugins.
7. Resolver libcurl/OpenSSL: si el juego no los necesita para ejecución offline, excluirlos del
   producto iOS; no portar una dependencia sin caso de uso.
8. Enlazar estáticamente el código permitido y empaquetar únicamente frameworks/dylibs aprobados por
   iOS.
9. Añadir una pantalla mínima que muestre versión, estado AOT y estado de data pack.
10. Compilar sin firma en CI y con firma ad-hoc/development solo en máquina autorizada.

Comandos objetivo de CI:

```bash
cmake --preset Debug-ios-simulator-arm64-aot-metal \
  -DOPENGOAL_AOT_GENERATED_DIR=out/aot/jak2/ios-simulator-arm64
cmake --build <ios-simulator-build-dir> --config Debug --parallel

cmake --preset Release-ios-device-arm64-aot-metal \
  -DOPENGOAL_AOT_GENERATED_DIR=out/aot/jak2/ios-device-arm64
cmake --build <ios-device-build-dir> --config Release --parallel
```

Tests:

- `file`/`lipo -info` demuestran ARM64.
- Inspección de link map demuestra que no están los componentes excluidos.
- Simulador abre y cierra sin crash.
- Build device sin firma termina; instalación no forma parte de CI genérica.

Aceptación: bundle iOS mínimo producido sin JIT entitlement, compiler ni OpenGL.

### IOS-002 — Entry point y máquina de estados de lifecycle

Dependencias: `IOS-001` y `AOT-014`.

Objetivo: adaptar el loop de escritorio al ciclo de vida iOS sin bloquear el main thread.

Estado explícito:

```text
Cold → WaitingForData → Starting → Running ⇄ Paused → Stopping → Stopped
                                  └───────────────→ Failed
```

Implementación:

1. Usar el mecanismo de main callbacks de SDL3 soportado por iOS.
2. Dividir init, event, iterate y quit; `main` no contiene un loop infinito.
3. Mantener SDL, UIKit y Metal en el thread principal.
4. Ejecutar dispatcher/EE en un thread controlado si la arquitectura actual lo requiere.
5. Crear barreras con predicados para start, pause, resume y shutdown; no usar sleeps.
6. `SDL_AppIterate` procesa eventos, input, un frame renderizable y presentación.
7. Al entrar en background: dejar de pedir drawables, detener rumble, pausar audio y llevar el EE a
   un safe point con timeout explícito.
8. Al volver: recrear drawable-dependent resources, reanudar audio/input y continuar sin recargar
   todo el juego si el estado sigue válido.
9. En memory warning, liberar caches recreables, nunca saves ni data pack activo.
10. En termination, guardar de forma atómica solo si el runtime está en estado seguro.
11. Hacer idempotentes init parcial y shutdown tras error.
12. Enviar todos los fallos a una pantalla de error no bloqueante con log exportable.

Tests:

- 100 ciclos sintéticos pause/resume.
- Background durante boot, load de nivel, gameplay y guardado.
- Termination antes de completar init.
- Drawable ausente durante varias iteraciones.
- Dos eventos de pause/resume duplicados.
- Shutdown con thread EE aún activo produce cierre acotado y diagnóstico.

Aceptación `GATE-I0`: boot headless AOT y frame Metal continúan después de background/foreground en
simulador y dispositivo.

### IOS-003 — Formato y creador de paquete de datos

Dependencias: `AOT-012`.

Objetivo: convertir una extracción legal en un paquete importable que no contenga código.

Formato propuesto:

```text
magic: OGDP
schema_version
game_version
release/region identificada
aot_data_abi_version
manifest_sha256
toc_offset/toc_size
payload_offset/payload_size
toc entries: path lógico, tipo de asset, offset, compressed size, raw size, sha256
```

Implementación host:

1. Crear `data-packager` como herramienta de Mac, no iOS.
2. Consumir exclusivamente outputs del extractor y manifests aprobados.
3. Mantener allowlist de tipos/rutas de asset requeridos por Jak II.
4. Rechazar objetos ejecutables, segmentos de código, librerías, Mach-O y extensiones no conocidas.
5. Normalizar paths a `/`, prohibir absoluto, `..`, symlink y colisiones case-insensitive.
6. Ordenar TOC de manera canónica y generar hashes deterministas.
7. Comprimir por entrada o bloques para permitir lectura aleatoria y límites de expansión.
8. Incluir únicamente metadata de compatibilidad, nunca la ruta del ISO ni del usuario.
9. Validar el paquete completo después de escribirlo.
10. Escribir mediante archivo temporal y rename atómico.
11. Generar informe con conteo y tamaño por categoría, sin listar contenido sensible innecesario.
12. Documentar el comando desde una extracción existente, sin añadir ni copiar el ISO al repo.

Interfaz objetivo:

```bash
<host-build>/data-packager \
  --game jak2 \
  --input <EXTRACTED_DATA_DIR> \
  --aot-manifest out/aot/jak2/ios-device-arm64/manifest.json \
  --output <USER_CHOSEN_OUTPUT>.ogdp
```

Tests:

- Paquete sintético reproducible.
- Rechazo de code entry, Mach-O magic, path traversal, symlink, case collision y hash incorrecto.
- Tamaño expandido que excede límite.
- Archivo truncado en cada sección.
- Pack de región/version diferente produce incompatibilidad clara.
- Escaneo Git confirma que ningún `.ogdp`, ISO o asset quedó tracked.

Aceptación: el paquete contiene cero código ejecutable y puede validarse sin cargarlo al heap.

### IOS-004 — Importación, sandbox, saves y configuración

Dependencias: `IOS-002` y `IOS-003`.

Objetivo: instalar datos aportados por el usuario y persistir el juego dentro del sandbox.

Implementación:

1. Presentar document picker desde UIKit en el main thread.
2. Obtener acceso security-scoped solo durante la copia y liberarlo siempre.
3. Validar header/compatibilidad antes de reservar el espacio completo.
4. Consultar espacio disponible y exigir margen para copia temporal + instalación.
5. Copiar a un nombre temporal dentro de Application Support.
6. Calcular hashes durante la copia y validar TOC/payload completo.
7. Hacer rename atómico a versión activa y conservar la anterior hasta confirmar boot.
8. Si el nuevo paquete falla al arrancar, restaurar el anterior y mostrar el motivo.
9. Usar rutas de plataforma, no `HOME` ni búsqueda de `jak-project`.
10. Guardar settings, saves, screenshots y caches en directorios separados.
11. Guardar partidas mediante temporary + fsync/close + atomic replace donde el filesystem lo
    permita.
12. Versionar settings y saves; migraciones deben conservar copia de backup.
13. Aplicar file protection compatible con uso y background definidos por el producto.
14. Añadir una acción explícita para eliminar datos importados, con confirmación y sin borrar saves
    salvo elección separada.

Tests:

- Primera instalación, actualización válida y rollback inválido.
- Cancelar picker y perder security scope a mitad de copia.
- Sin espacio, hash incorrecto, package truncado y app terminada durante import.
- Path con Unicode.
- Save/load tras relanzar y tras actualización del data pack.
- Migración de settings y rollback.

Aceptación: ninguna ruta iOS depende del directorio del repositorio o de una ruta absoluta externa.

### IOS-005 — Mando, touch y vibración segura

Dependencias: `IOS-002` y `MTL-007`.

Objetivo: hacer el juego controlable sin teclado y robusto al hotplug.

Implementación de mando:

1. Usar eventos SDL Gamepad respaldados por GameController; no depender de HID desktop.
2. Enumerar usando el índice real del vector de gamepads y liberar listas SDL con su allocator.
3. Mantener mapping por instance ID estable, no por posición de joystick global.
4. Al desconectar: limpiar botones, sticks, mapping, rumble, LED y efectos pendientes.
5. Procesar remap y reconexión sin bloquear el frame.
6. Validar port/controller antes de rumble, LED o función específica.
7. Tratar LED, adaptive triggers y pressure como capacidades opcionales.
8. En iOS, desactivar rutas DualSense HID privadas/no disponibles; usar APIs públicas SDL/GC.
9. Detener vibración en pause, background, disconnect y shutdown.
10. Probar dos mandos y reasignación de port.

Implementación touch:

1. Definir layout declarativo por orientación y safe area.
2. Proporcionar sticks, D-pad, face buttons, shoulders, start/select y acciones necesarias.
3. Soportar multitouch y tracking por finger ID.
4. No mezclar coordenadas de puntos con píxeles del drawable.
5. Añadir opacidad, escala y posición configurables.
6. Ocultar touch automáticamente al usar mando si la opción lo permite.
7. Limpiar todos los toques al perder focus o entrar en background.
8. Renderizar overlay mediante Metal, sin UIKit views por botón si afecta al frame pacing.

Tests:

- Connect/disconnect/remap durante cada pantalla.
- Dos mandos, port inválido y evento tardío tras disconnect.
- Rumble unsupported no falla.
- Diez dedos, toque cancelado, rotación y safe-area notch.
- Cambio mando ↔ touch sin input pegado.

Aceptación: menú y gameplay pueden controlarse con mando externo y solo con touch.

### IOS-006 — Audio e interrupciones

Dependencias: `IOS-002`.

Objetivo: mantener audio correcto ante llamadas, Siri, cambio de ruta y background.

Implementación:

1. Confirmar backend CoreAudio de SDL y formatos/sample rates realmente usados.
2. Configurar categoría/session coherente con un juego; no forzar mezcla o background sin decisión.
3. Manejar interruption begin/end y route changes.
4. Pausar producción de audio antes de cerrar/recrear dispositivo.
5. Reanudar sin duplicar voces ni perder estado lógico.
6. Manejar auriculares, Bluetooth y cambio de sample rate/buffer.
7. No mantener audio activo en background si el producto no lo declara.
8. Registrar underruns y duración de callbacks sin log por callback.
9. Evitar allocations y locks no acotados en audio callback.
10. Hacer shutdown idempotente tras interrupción.

Tests:

- Interruption durante música, diálogo y silencio.
- Route change repetido.
- Pause/resume de aplicación.
- Dispositivo temporalmente ausente.
- Sesión de 30 minutos sin crecimiento de voces/buffers.

Aceptación: no hay crash, audio duplicado, ruido ni pérdida permanente tras interrupción.

### IOS-007 — Memoria, streaming y thermal behavior

Dependencias: `IOS-003`, `IOS-004`, `MTL-014`.

Objetivo: adaptar los budgets del runtime de escritorio a dispositivos móviles medidos.

Implementación:

1. Medir memoria base: EE region, código AOT, registries, Metal heaps, textures, level data y audio.
2. Mantener EE data region en el tamaño semántico necesario; no reducirla sin pruebas de offsets.
3. Evitar duplicar data pack completo, objeto comprimido y objeto expandido simultáneamente.
4. Stream assets por bloques y liberar buffers temporales tras publicar recursos.
5. Definir caches recreables y orden de purga ante memory warning.
6. Usar budget/working set de Metal cuando esté disponible y manejar allocation failure.
7. Medir dispositivos de gama mínima, media y alta elegidos en `AOT-000`.
8. Observar thermal state y reducir calidad configurable/frame cap, no velocidad de simulación.
9. Probar sesiones largas, cambios de nivel y background.
10. No añadir sleeps para esconder stutter; perfilar el bloqueo.
11. Registrar p50/p95/p99 de frame time, load time y memoria pico.
12. Separar optimizaciones correctas de degradaciones visuales mediante settings versionados.

Aceptación: sin jetsam en la matriz objetivo, sin memoria creciente y con frame pacing aprobado.

### IOS-008 — Firma de desarrollo y auditoría cero JIT en dispositivo

Dependencias: `IOS-004`, `IOS-005`, `IOS-006`, `IOS-007`.

Objetivo: demostrar en hardware que el producto AOT cumple la restricción, sin publicar.

Implementación:

1. Mantener team ID, provisioning profile y certificados fuera del repositorio.
2. Firmar el bundle con entitlement mínimo generado por Xcode.
3. Instalar mediante Xcode/herramienta oficial en dispositivo autorizado.
4. Ejecutar auditoría estática antes de instalar.
5. Arrancar, importar datos, cargar nivel y repetir inspección runtime disponible.
6. Confirmar que no aparecen logs de JIT/protection transition.
7. Confirmar arquitectura del executable y frameworks.
8. Exportar crash logs/symbolication sin rutas personales antes de guardarlos como evidencia.
9. Probar con Developer Mode según requisitos del dispositivo, sin desactivar protecciones del OS.
10. No usar jailbreak, private entitlements, APIs privadas ni firma ad-hoc para simular distribución.

Aceptación:

- `allow-jit` ausente.
- Cero memoria anónima ejecutable atribuible a la app.
- Todo código ejecutable pertenece a imágenes Mach-O firmadas del bundle/sistema.
- Juego arranca y carga gameplay real en dispositivo ARM64.

`GATE-I1` solo pasa además cuando input, audio, saves y lifecycle están aprobados.

## 8. Tickets de release y QA

### REL-001 — CI reproducible AOT + Metal

Dependencias: `AOT-015`, `MTL-014`, `IOS-001`.

Objetivo: impedir que una regresión reintroduzca JIT, OpenGL o outputs no deterministas.

Jobs mínimos:

1. Host tools ARM64.
2. Generación AOT de fixtures sin assets.
3. Runtime macOS ARM64 AOT+Metal.
4. Runtime macOS ARM64 dinámico+OpenGL.
5. Regresión macOS x86-64 existente.
6. iOS simulator AOT+Metal.
7. iOS device build sin firma.
8. Shaders macOS/iOS offline.
9. Auditoría no-JIT.
10. Determinismo de manifest/registry/assembly.
11. Escaneo de privacidad y assets.

La CI pública usa fixtures sintéticos. Las pruebas con assets legales se ejecutan en runner privado y
publican solo resultados sanitizados.

Aceptación: un cambio que introduce `allow-jit`, shader runtime, code segment en data pack o path
privado falla antes de producir artefacto.

### REL-002 — Matriz macOS ARM64

Dependencias: `AOT-016` y `REL-001`.

Objetivo: validar que AOT+Metal es al menos tan funcional como la build dinámica soportada.

Matriz mínima:

- Dos generaciones Apple Silicon cuando haya hardware disponible.
- Resolución Retina y externa.
- Ventana, fullscreen nativo, cambio de monitor y sleep/wake.
- Mando Xbox/PlayStation/compatible SDL.
- Saves nuevos, existentes, load y rollback.
- Campaña completa con checkpoints documentados.
- Escenas de rendimiento definidas por `MTL-001`.
- Auditoría `sysctl.proc_translated == 0` para el proceso de producto.
- Firma, hardened runtime y Gatekeeper en el ticket autorizado de distribución.

Aceptación: cero P0/P1, campaña completa y matriz firmada por revisor independiente.

### REL-003 — Matriz iPhone/iPad

Dependencias: `IOS-008` y `REL-001`.

Objetivo: validar producto móvil, no solo compatibilidad de API.

Matriz mínima:

- Dispositivo mínimo soportado, dispositivo medio y dispositivo reciente.
- Al menos un iPhone y un iPad.
- 60 Hz y displays con refresh variable si están en alcance.
- Landscape, safe areas y rotación soportada.
- Touch y al menos dos familias de mando.
- Interruption de audio, background/foreground, lock/unlock y low-memory.
- Import, update, rollback y eliminación de data pack.
- Saves durante campaña completa.
- Sesión térmica prolongada conectada y con batería.
- Sin red y con red restringida.

Cada checkpoint registra build SHA, device model genérico, OS, save hash, frame metrics y bugs. No se
guardan UDID, Apple ID, nombre del dispositivo ni paths privados.

Aceptación `GATE-R0`: campaña, periféricos, lifecycle, memoria y auditoría no-JIT completos, con cero
P0/P1 abiertos.

### REL-004 — Preparación de distribución, sin publicación automática

Dependencias: `REL-002` y `REL-003`.

Objetivo: preparar artefactos y documentación sin asumir autorización legal o de publicación.

Implementación:

1. Revisar licencias de código y dependencias.
2. Confirmar que bundles y repositorio no contienen assets del juego.
3. Documentar extracción y creación de paquete desde copia legal del usuario.
4. Documentar limitaciones de trademark/copyright y separar análisis legal de validación técnica.
5. Preparar privacy manifest y declaraciones reales de APIs utilizadas.
6. Preparar entitlements mínimos.
7. Generar SBOM de dependencias.
8. Sanitizar symbols, logs, manifests y metadata.
9. Preparar instrucciones de firma/notarización/TestFlight, pero no ejecutarlas sin autorización.
10. Ejecutar una revisión final de que el paquete importado es data-only.

Aceptación: artefacto preparado, pero ninguna subida, notarización, TestFlight o App Store ocurre sin
un ticket y autorización explícitos.

## 9. Orden de ejecución y paralelismo

Orden recomendado para un único implementador:

```text
AOT-000
├── AOT-001
├── AOT-002 → AOT-003 → AOT-004 → AOT-005 → AOT-006 → AOT-007 → AOT-008
│          → AOT-009 → AOT-010 → AOT-011 → AOT-012 → AOT-013 → AOT-014 → AOT-015
└── MTL-001 → MTL-002 → MTL-003 → MTL-004 → MTL-005 → MTL-006 → MTL-007
           → MTL-008 → MTL-009 → MTL-010 → MTL-011 → MTL-012 → MTL-013 → MTL-014

AOT-015 + MTL-014 → AOT-016
AOT/Metal parciales → IOS-001 → IOS-002
AOT-012 → IOS-003 → IOS-004
IOS-002 + Metal → IOS-005, IOS-006
MTL-014 + datos → IOS-007 → IOS-008
Todo lo anterior → REL-001 → REL-002/REL-003 → REL-004
```

Con dos equipos, AOT y Metal pueden avanzar en paralelo después de `AOT-000`, pero:

- Solo AOT modifica compiler, linker, function model y execution backend.
- Solo Metal modifica resource abstractions y renderers.
- CMake y `gfx.*` se asignan en ventanas exclusivas acordadas.
- iOS no empieza integración profunda hasta `GATE-A3` y `GATE-M0`.

## 10. Prompt operativo para cada ticket

Entregar este bloque al implementador sustituyendo los campos:

```text
Implementa exclusivamente el ticket <TICKET> del playbook
docs/apple-aot-metal-playbook.md.

Base:
- Commit: <SHA>
- Rama: <BRANCH>
- Dependencias verificadas DONE: <LISTA>

Lee completos antes de actuar:
1. AGENTS.md
2. docs/apple-aot-metal-playbook.md
3. DECISIONS.md y STATUS.md del directorio de coordinación
4. El perfil y ticket asignados
5. Los handoffs de las dependencias directas

Propiedad:
- Puedes modificar: <LISTA EXACTA>
- Solo lectura: <LISTA EXACTA>
- No puedes modificar: todo lo demás

Procedimiento:
1. Comprueba git status, SHA, arquitectura y versiones.
2. Ejecuta y guarda el baseline indicado.
3. Añade primero un test que falle por la causa correcta.
4. Implementa el cambio mínimo del ticket.
5. Ejecuta tests dirigidos y regresiones obligatorias.
6. Ejecuta git diff --check y revisa cada línea del diff.
7. Escanea datos privados y assets.
8. Crea evidence/<TICKET>-handoff.md con comandos, exit codes y resultados reales.
9. Entrega en REVIEW. No empieces el siguiente ticket.

Restricciones:
- No JIT ni código importado en targets AOT.
- No shader source compilation en targets Metal de producto.
- No Rosetta para el producto ARM64.
- No assets, ISO, credenciales, perfiles o datos personales.
- No push, PR, firma, publicación ni cambios externos sin autorización.
- No TODO/FIXME/NYI nuevo ni test vacío.
- No cambios de ABI/formato sin ADR aceptada.
- Los commits o comunicaciones externas deben cumplir la política de divulgación del proyecto.

Si un criterio no puede demostrarse, entrega BLOCKED con el primer impedimento reproducible.
```

## 11. Checklist de revisión independiente

El revisor debe repetir, no limitarse a leer el handoff:

- [ ] El diff solo toca los archivos autorizados.
- [ ] Existe prueba positiva, límite/error y regresión compartida.
- [ ] La prueba observa retorno, bytes, memoria, registros, estado o imagen; no solo ausencia de crash.
- [ ] El modo dinámico conserva comportamiento/bytes cuando el ticket lo exige.
- [ ] No se usa `x18`; `sp` queda alineado; registros preservados según ABI.
- [ ] No existen direcciones absolutas, offsets observados, sleeps ni fallbacks silenciosos.
- [ ] AOT no ejecuta memoria EE ni data packs.
- [ ] Metal carga `metallib` y no compila shaders desde source.
- [ ] iOS no enlaza herramientas/dependencias desktop.
- [ ] Los parsers validan todos los tamaños antes de mutar estado.
- [ ] La evidencia incluye comandos y exit codes reales.
- [ ] `git diff --check` pasa.
- [ ] El escaneo de privacidad/assets está vacío.

## 12. Condiciones de parada inmediata

El implementador se detiene y entrega `BLOCKED` si ocurre cualquiera:

- La caracterización contradice el descriptor aprobado.
- Se necesita almacenar un native pointer dentro de la memoria GOAL persistente.
- Un objeto necesita ejecutar código no presente en el registry firmado.
- Cambia inesperadamente un fixture dinámico/x86.
- El linker Apple no puede expresar una relocation sin cambiar el ABI acordado.
- Metal necesita una operación sin equivalente y la solución cambia la imagen/gameplay.
- La corrección exige compilar shader source en runtime.
- El port iOS requiere private entitlement, API privada o desactivar una protección.
- Aparece asset comercial o path privado en Git.
- Dos tickets activos necesitan escribir el mismo archivo.
- Se necesita decidir deployment target, device matrix o distribución y no existe decisión del
  propietario.

## 13. Definición final de “hecho”

No se declara terminado hasta que todos estos comandos/controles tengan evidencia:

```text
Build host tools ARM64                       PASS
Generación AOT completa y determinista       PASS
Tests ABI/bridge/loader AOT                  PASS
Boot headless AOT macOS                      PASS
Auditoría no-JIT macOS                       PASS
Renderer Metal completo                      PASS
Paridad visual y performance                 PASS
Build macOS ARM64 AOT+Metal                  PASS
Build iOS simulator ARM64 AOT+Metal          PASS
Build/instalación iOS device ARM64           PASS
Auditoría no-JIT en dispositivo              PASS
Mando externo y touch                        PASS
Audio/lifecycle/import/save                  PASS
Campaña y matriz macOS                       PASS
Campaña y matriz iOS                         PASS
Regresión dinámica ARM64 y x86-64            PASS
Escaneo de assets/privacidad                 PASS
```

El resultado técnico final es un único núcleo GOAL AOT y un único backend Metal compartidos, con
artefactos diferentes por SDK. macOS e iOS no comparten el mismo Mach-O ni necesariamente la misma
`metallib`; comparten fuentes, schemas, tests y comportamiento.
