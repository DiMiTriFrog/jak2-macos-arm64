# Instrucciones precisas para Luna

## Objetivo

Mantener el juego Jak II funcionando nativamente en Apple Silicon ARM64 y, como
trabajo separado, cerrar `Jak2KernelTest.RunFunctionInProcess`. No cambiar el
renderer, no sustituir SDL/OpenGL por Metal, no distribuir la ISO/assets y no
ejecutar nada con Rosetta.

## Estado que Luna debe tomar como base

Ruta:

```text
raíz del repositorio privado jak-project
```

Rama y base:

```text
git branch --show-current
git rev-parse HEAD
git status --short
```

La rama esperada es `codex/arm-024-full-jak2` y el commit base documentado es
`d154e5f53`. No hacer reset, checkout, stash ni borrar cambios ajenos.

## Diagnóstico ya cerrado de la pantalla negra

El bloqueo visual no era audio, OpenGL, Game Mode ni la ISO. Era una operación
SIMD de transposición:

```text
matrix-transpose! -> .pextlw/.pextuw -> IGenARM64.cpp
```

El backend ARM64 estaba usando `UZP1/UZP2`, que separa lanes. El backend x86 del
proyecto implementa `VPUNPCKL/VPUNPCKH`, que intercala lanes. La sustitución
correcta ya aplicada es:

```text
pextub = ZIP2 16B
pextuh = ZIP2 8H
pextuw = ZIP2 4S
pextlb = ZIP1 16B
pextlh = ZIP1 8H
pextlw = ZIP1 4S
```

No revertir este cambio. Verificarlo con:

```text
./build-arm64-release/goalc-test --gtest_color=no \
  '--gtest_filter=ARM64EmitterParity.*:ARM64IRInt128.Math3AllOpcodesAndAliases'
```

## Procedimiento para el bloqueo de prueba restante

1. Ejecutar únicamente la prueba problemática y guardar salida:

   ```text
   gtimeout 180 ./build-arm64-release/goalc-test --gtest_color=no \
     '--gtest_filter=Jak2KernelTest.RunFunctionInProcess' \
     > logs/ARM-024/luna-run-function-repro.log 2>&1
   ```

2. Confirmar antes del crash que aparecen `kt2 return`, `ipf: 1` y
   `child-proc activated`. Si falla antes, reparar primero el listener/puerto:
   Jak 2 usa `8113`, no `8112`.

3. En LLDB detener tras cargar el código JIT, no fijar breakpoints a ciegas antes
   de mapearlo:

   ```text
   lldb -- ./build-arm64-release/goalc-test
   settings set -- target.run-args --gtest_color=no '--gtest_filter=Jak2KernelTest.RunFunctionInProcess'
   breakpoint set --name 'jak2::KernelDispatch(unsigned int)'
   run
   ```

   Cuando se detenga `KernelDispatch`, localizar el bloque JIT del constructor
   `new catch-frame` y comprobar, en cada llamada:

   ```text
   x20 = process pointer
   x22 = GOAL native-memory base
   sp  = current native stack
   *(u32 *)(x22 + x20 + 0x68) = process.stack-frame-top
   ```

4. Comparar dos casos: `init-child-proc 1` (retorno normal) y `init-child-proc 0`
   (llama `process-deactivate`, que debe salir por `throw 'initialize #f`).
   Registrar `sp`, `x30`, `stack-frame-top`, `catch-frame.sp`, `catch-frame.ra`
   antes y después del `blr`. El resultado correcto del caso 0 es saltar al
   llamador de `run-function-in-process`, no continuar por la limpieza normal del
   constructor.

5. Revisar específicamente:

   - `goal_src/jak2/kernel/gkernel.gc`: `new catch-frame`, `throw-dispatch`,
     `run-function-in-process`, `deactivate`.
   - `goalc/compiler/IR.cpp`: semántica ARM64 de `.push/.pop` y el hecho de que
     el rol histórico `rax` usa `x8/x30`; no tratarlo como una pila normal.
   - `game/kernel/asm_funcs_arm64.s`: conversión de pila GOAL/native y retorno.
   - La prueba artificial: ambos procesos de 1024 bytes reciben actualmente
     `*kernel-dram-stack*`; determinar si la prueba está creando alias de stack.

6. El arreglo aceptable debe preservar invariantes:

   - `sp` alineado a 16 bytes.
   - `x18` nunca usado.
   - Sin direcciones absolutas observadas ni sleeps.
   - Sin spills ocultos en `asm-func`.
   - La salida x86 no cambia.
   - La ruta normal de `catch-frame` y la ruta de `throw` se prueban por separado.

7. Pruebas obligatorias después de cualquier cambio:

   ```text
   ./build-arm64-release/goalc-test --gtest_color=no \
     '--gtest_filter=Jak2KernelTest.Basic:Jak2KernelTest.RunFunctionInProcess'
   ./build-arm64-release/goalc-test --gtest_color=no \
     '--gtest_filter=ARM64EmitterParity.*:ARM64IRInt128.Math3AllOpcodesAndAliases'
   gtimeout 900 ./build-arm64-release/goalc-test --gtest_color=no
   cmake --build build-x86_64-release --target gk -j6
   git diff --check
   git status --short
   ```

   No marcar `DONE` si el caso 0 sigue con `SIGSEGV`. Si el problema se demuestra
   exclusivo de la prueba por stacks aliasados, documentar la decisión y corregir
   el fixture con stacks independientes, manteniendo una prueba que verifique
   explícitamente la ruta de throw.

## Verificación final nativa

Usar el ejecutable normal, no una build x86:

```text
build-arm64-release/game/gk.app/Contents/MacOS/gk -g jak2 --proj-path . -- -boot -fakeiso
```

Auditar:

```text
file build-arm64-release/game/gk.app/Contents/MacOS/gk
lipo -archs build-arm64-release/game/gk.app/Contents/MacOS/gk
otool -hv build-arm64-release/game/gk.app/Contents/MacOS/gk
/usr/libexec/PlistBuddy -c 'Print :LSSupportsGameMode' \
  build-arm64-release/game/gk.app/Contents/Info.plist
sysctl -n sysctl.proc_translated
```

Los resultados esperados son `arm64`, `arm64`, header `PIE`, `true` y `0`.
No firmar/notarizar hasta el ticket de release correspondiente.
